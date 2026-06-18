"""Parser and channel naming for the NailSense serial protocol."""

from dataclasses import dataclass


VALID_SENSOR_COUNTS = (2, 6)
CHANNELS_PER_SENSOR = 4


@dataclass(frozen=True)
class DataFrame:
    status: str
    values: tuple[int, ...]

    @property
    def sensor_count(self) -> int:
        return len(self.values) // CHANNELS_PER_SENSOR


def channel_names(sensor_count: int) -> list[str]:
    if sensor_count not in VALID_SENSOR_COUNTS:
        raise ValueError(f"sensor_count must be one of {VALID_SENSOR_COUNTS}")
    return [
        f"SG{sensor}_CH{channel}"
        for sensor in range(1, sensor_count + 1)
        for channel in range(1, CHANNELS_PER_SENSOR + 1)
    ]


def abnormal_status(status: str) -> str:
    """Return only abnormal board states; healthy ``O`` states stay blank."""
    if not status or status == "LEGACY":
        return ""
    return " ".join(
        f"B{board}:{flag}"
        for board, flag in enumerate(status, start=1)
        if flag != "O"
    )


def parse_line(line: str, expected_sensors: int | None = None) -> DataFrame | None:
    """Parse one firmware line.

    Diagnostic lines and CAL_DONE return None. Both the current
    ``S:<flags>,...`` format and legacy value-only CSV are accepted.
    """
    text = line.strip()
    if not text or text.startswith("#") or text == "CAL_DONE":
        return None

    status = "LEGACY"
    payload = text
    if text.startswith("S:"):
        try:
            prefix, payload = text.split(",", 1)
        except ValueError as exc:
            raise ValueError("status frame has no data payload") from exc
        status = prefix[2:]
        if not status or any(flag not in "OTRCX" for flag in status):
            raise ValueError(f"invalid status flags: {status!r}")

    try:
        values = tuple(int(value) for value in payload.split(","))
    except ValueError as exc:
        raise ValueError("data payload contains a non-integer value") from exc

    valid_lengths = {count * CHANNELS_PER_SENSOR for count in VALID_SENSOR_COUNTS}
    if len(values) not in valid_lengths:
        raise ValueError(
            f"expected 8 or 24 values for 2 or 6 sensors; received {len(values)}"
        )

    sensor_count = len(values) // CHANNELS_PER_SENSOR
    if expected_sensors is not None and sensor_count != expected_sensors:
        raise ValueError(
            f"expected {expected_sensors} sensors, received {sensor_count}"
        )
    if status != "LEGACY" and len(status) != sensor_count // 2:
        raise ValueError(
            f"expected {sensor_count // 2} board-status flags, received {len(status)}"
        )

    return DataFrame(status=status, values=values)
