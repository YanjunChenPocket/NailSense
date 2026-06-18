# UIUC Nail Sensor Streaming

This repository contains the Teensy firmware and host-side tools for the
2-sensor and 6-sensor nail measurement systems.

The browser interface provides live visualization and CSV recording. The
Python tool is a lightweight CSV recorder for scripted or long experiments.

## Repository contents

```text
firmware/
  NailSense-2CH/   Teensy firmware: 1 ADC board, 2 sensors, 8 values
  NailSense-6CH/   Teensy firmware: 3 ADC boards, 6 sensors, 24 values
web/
  NailSense-2CH/   Browser live display and CSV recorder
  NailSense-6CH/   Browser live display and CSV recorder
src/          Python streaming package
docs/         Serial protocol and channel map
```

“2-sensor” and “6-sensor” are used intentionally. Each sensor contains four
channels, so the serial stream contains 8 or 24 measurement values.

## Option A: browser interface

1. Flash the matching firmware with Arduino IDE/Teensyduino.
2. Connect the Teensy by USB.
3. Keep the sensors unloaded and still for the first three seconds.
4. Open the matching `web/.../index.html` in Chrome or Edge.
5. Click **Connect**, select the Teensy serial port, and wait for data.
6. Click **Select Folder**, then **Record**, to save a CSV file.

If the browser does not expose Web Serial when opening the file directly,
serve the repository with any local static-file server and open it from
`localhost`.

The web pages require a Chromium-based browser with Web Serial support.
Safari and Firefox do not currently provide this interface.

## Option B: Python recorder

Python 3.9 or newer is required.

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e .
nail-stream --list-ports
nail-stream --port /dev/cu.usbmodemXXXX --sensors 6
```

On Windows:

```powershell
.venv\Scripts\activate
nail-stream --port COM4 --sensors 2
```

The sensor count can also be detected automatically:

```bash
nail-stream --port COM4
```

By default, recordings are written under `recordings/`. Use a custom path
with:

```bash
nail-stream --port COM4 --output experiment_01.csv
```

Press `Ctrl+C` to stop safely. Web and Python recordings use the same columns:
`Sequence`, `Time_s`, followed by the sensor channels. Every recording starts
at sequence 1 and time 0.

## Firmware and hardware

- Controller: Teensy 4.0
- ADC: ADS1263
- USB serial: 2,000,000 baud
- Shared SPI: SCK 13, MOSI 11, MISO 12
- Shared ADC RESET/PWDN line: Teensy pin 10, driven high by firmware
- Board 1: CS 23, DRDY 2
- Board 2: CS 22, DRDY 3
- Board 3: CS 21, DRDY 4
- Output unit: baseline-corrected microvolts (`µV`)

The RESET/PWDN line must not float. The firmware performs checksum validation,
register verification, timeout recovery, and unexpected-reset detection.

## Important measurement notes

- Keep sensors unloaded during the three-second startup baseline.
- A firmware reinitialization does not repeat the baseline calibration.
- Healthy status is hidden on the web page. Errors are shown on the page with
  their board number, such as `B2:T` or `B3:C`, but are not added to the CSV.
- The output is electrical strain-sensor signal in `µV`, not force. Converting
  to force requires the appropriate calibration model.
- Stacked board connectors should be firmly seated before long experiments.

See [the serial protocol](docs/SERIAL_PROTOCOL.md) and
[the channel map](docs/CHANNEL_MAP.md) for details.
