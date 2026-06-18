"""Command-line serial recorder and optional live plot."""

from __future__ import annotations

import argparse
import csv
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
import sys
import time

from .protocol import DataFrame, channel_names, parse_line


BAUD_RATE = 2_000_000


def available_ports():
    from serial.tools import list_ports

    return sorted(list_ports.comports(), key=lambda port: port.device)


def choose_port(requested: str | None) -> str:
    if requested:
        return requested
    ports = available_ports()
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        raise RuntimeError("No serial ports found. Connect the Teensy and retry.")
    details = "\n".join(
        f"  {port.device}: {port.description}" for port in ports
    )
    raise RuntimeError(f"Multiple serial ports found; use --port:\n{details}")


def default_output() -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return Path("recordings") / f"nail_sensor_{stamp}.csv"


class LivePlot:
    def __init__(self, sensor_count: int, history: int = 500):
        try:
            import matplotlib.pyplot as plt
        except ImportError as exc:
            raise RuntimeError(
                'Live plotting requires: pip install -e ".[plot]"'
            ) from exc

        self.plt = plt
        self.names = channel_names(sensor_count)
        self.times = deque(maxlen=history)
        self.values = [deque(maxlen=history) for _ in self.names]
        self.figure, axes = plt.subplots(
            sensor_count, 1, sharex=True, figsize=(12, max(5, sensor_count * 2))
        )
        if sensor_count == 1:
            axes = [axes]
        self.lines = []
        for sensor, axis in enumerate(axes):
            axis.set_ylabel(f"SG{sensor + 1}\nµV")
            axis.grid(alpha=0.25)
            sensor_lines = []
            for channel in range(4):
                index = sensor * 4 + channel
                line, = axis.plot([], [], label=f"CH{channel + 1}", linewidth=1)
                sensor_lines.append((index, line))
            axis.legend(loc="upper right", ncol=4, fontsize=8)
            self.lines.extend(sensor_lines)
        axes[-1].set_xlabel("Elapsed time (s)")
        self.figure.tight_layout()
        plt.ion()
        plt.show(block=False)
        self.last_draw = 0.0

    def add(self, elapsed: float, frame: DataFrame) -> None:
        self.times.append(elapsed)
        for index, value in enumerate(frame.values):
            self.values[index].append(value)
        now = time.monotonic()
        if now - self.last_draw < 0.08:
            return
        for index, line in self.lines:
            line.set_data(self.times, self.values[index])
            line.axes.relim()
            line.axes.autoscale_view()
        self.figure.canvas.draw_idle()
        self.figure.canvas.flush_events()
        self.last_draw = now


def write_header(writer: csv.writer, sensor_count: int) -> None:
    writer.writerow(
        ["host_time_utc", "elapsed_s", "sequence", "status"]
        + channel_names(sensor_count)
    )


def stream(args: argparse.Namespace) -> int:
    import serial

    port_name = choose_port(args.port)
    output = Path(args.output) if args.output else default_output()
    output.parent.mkdir(parents=True, exist_ok=True)

    expected = None if args.sensors == "auto" else int(args.sensors)
    serial_port = serial.Serial(port_name, BAUD_RATE, timeout=1)
    serial_port.reset_input_buffer()
    print(f"Connected to {port_name} at {BAUD_RATE:,} baud")
    print("Waiting for calibration and the first data frame…")

    data_start = None
    sequence = 0
    malformed = 0
    sensor_count = expected
    plot = None
    file_handle = output.open("w", newline="", encoding="utf-8")
    writer = csv.writer(file_handle)

    try:
        while True:
            raw = serial_port.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            try:
                frame = parse_line(line, expected)
            except ValueError as exc:
                malformed += 1
                if malformed <= 5 or malformed % 100 == 0:
                    print(f"Skipped malformed line ({malformed}): {exc}", file=sys.stderr)
                continue
            if frame is None:
                if line == "CAL_DONE":
                    print("Calibration complete")
                elif args.diagnostics and line.startswith("#"):
                    print(line)
                continue

            if sensor_count is None:
                sensor_count = frame.sensor_count
            if sequence == 0:
                data_start = time.monotonic()
                write_header(writer, sensor_count)
                if args.plot:
                    plot = LivePlot(sensor_count, args.history)
                print(f"Detected {sensor_count} sensors; recording to {output}")

            elapsed = time.monotonic() - data_start
            writer.writerow(
                [
                    datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                    f"{elapsed:.6f}",
                    sequence,
                    frame.status,
                    *frame.values,
                ]
            )
            sequence += 1

            if plot:
                plot.add(elapsed, frame)
            if sequence % args.flush_every == 0:
                file_handle.flush()
            if sequence % 200 == 0:
                rate = sequence / max(elapsed, 1e-9)
                print(
                    f"\rFrames: {sequence:,}  Average rate: {rate:.1f} Hz  "
                    f"Status: {frame.status}",
                    end="",
                    flush=True,
                )
    except KeyboardInterrupt:
        print("\nStopping…")
    finally:
        file_handle.flush()
        file_handle.close()
        serial_port.close()

    print(f"Saved {sequence:,} frames to {output}")
    if malformed:
        print(f"Skipped {malformed:,} malformed serial lines")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Stream UIUC nail-sensor data to CSV."
    )
    parser.add_argument("--port", help="Serial port, such as COM4 or /dev/cu.usbmodem...")
    parser.add_argument(
        "--sensors",
        choices=("auto", "2", "6"),
        default="auto",
        help="Expected sensor configuration (default: auto)",
    )
    parser.add_argument("--output", help="CSV output path")
    parser.add_argument("--plot", action="store_true", help="Show a live plot")
    parser.add_argument(
        "--history", type=int, default=500, help="Samples shown in the live plot"
    )
    parser.add_argument(
        "--flush-every",
        type=int,
        default=20,
        help="Flush the CSV after this many frames",
    )
    parser.add_argument(
        "--diagnostics", action="store_true", help="Print firmware diagnostic lines"
    )
    parser.add_argument(
        "--list-ports", action="store_true", help="List serial ports and exit"
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.list_ports:
        ports = available_ports()
        if not ports:
            print("No serial ports found")
        for port in ports:
            print(f"{port.device}\t{port.description}")
        return 0
    try:
        return stream(args)
    except (OSError, RuntimeError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
