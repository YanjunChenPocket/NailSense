import pytest

from nail_sensor_streaming.protocol import channel_names, parse_line


def test_ignores_non_data_lines():
    assert parse_line("") is None
    assert parse_line("CAL_DONE") is None
    assert parse_line("# STATS uptime_s=30") is None


def test_parses_two_sensor_status_frame():
    frame = parse_line("S:O,1,2,3,4,5,6,7,8", expected_sensors=2)
    assert frame is not None
    assert frame.status == "O"
    assert frame.sensor_count == 2
    assert frame.values[-1] == 8


def test_parses_six_sensor_status_frame():
    values = ",".join(str(value) for value in range(24))
    frame = parse_line(f"S:OTX,{values}", expected_sensors=6)
    assert frame is not None
    assert frame.status == "OTX"
    assert len(frame.values) == 24


def test_accepts_legacy_value_only_frame():
    frame = parse_line(",".join("0" for _ in range(8)))
    assert frame is not None
    assert frame.status == "LEGACY"


def test_rejects_wrong_length():
    with pytest.raises(ValueError, match="expected 8 or 24"):
        parse_line("S:O,1,2,3")


def test_rejects_wrong_configuration():
    with pytest.raises(ValueError, match="expected 6 sensors"):
        parse_line("S:O,1,2,3,4,5,6,7,8", expected_sensors=6)


def test_channel_names_follow_sensor_order():
    names = channel_names(2)
    assert names == [
        "SG1_CH1",
        "SG1_CH2",
        "SG1_CH3",
        "SG1_CH4",
        "SG2_CH1",
        "SG2_CH2",
        "SG2_CH3",
        "SG2_CH4",
    ]
