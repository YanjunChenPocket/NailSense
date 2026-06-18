"""Command-line serial recorder."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime
from pathlib import Path
import sys
import time

from .protocol import abnormal_status, channel_names, parse_line


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
    return Path("recordings") / f"NailSense_{stamp}.csv"


def write_header(writer: csv.writer, sensor_count: int) -> None:
    writer.writerow(["Sequence", "Time_s"] + channel_names(sensor_count))


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
                print(f"Detected {sensor_count} sensors; recording to {output}")

            elapsed = time.monotonic() - data_start
            writer.writerow(
                [
                    sequence + 1,
                    f"{elapsed:.6f}",
                    *frame.values,
                ]
            )
            sequence += 1

            if sequence % args.flush_every == 0:
                file_handle.flush()
            if sequence % 200 == 0:
                rate = sequence / max(elapsed, 1e-9)
                print(
                    f"\rFrames: {sequence:,}  Average rate: {rate:.1f} Hz  "
                    f"Status: {abnormal_status(frame.status) or '-'}",
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
        description="Record NailSense data to CSV."
    )
    parser.add_argument("--port", help="Serial port, such as COM4 or /dev/cu.usbmodem...")
    parser.add_argument(
        "--sensors",
        choices=("auto", "2", "6"),
        default="auto",
        help="Expected sensor configuration (default: auto)",
    )
    parser.add_argument("--output", help="CSV output path")
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
