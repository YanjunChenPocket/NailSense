# NailSense

Firmware and CSV recording tools for the NailSense 2CH and 6CH systems.

## 1. Upload the Teensy firmware

Install Arduino IDE and Teensy support (Teensyduino), then connect the Teensy
4.0 by USB.

Open the matching firmware:

- 2CH: `firmware/NailSense-2CH/NailSense-2CH.ino`
- 6CH: `firmware/NailSense-6CH/NailSense-6CH.ino`

In Arduino IDE:

1. Select **Tools > Board > Teensyduino > Teensy 4.0**.
2. Select **Tools > USB Type > Serial**.
3. Select **Tools > Port**, then choose the Teensy serial port.
4. Click **Verify**.
5. Click **Upload**.
6. Keep all sensors unloaded and still for three seconds after startup.

## 2. Browser interface

Open the matching page in Chrome or Edge:

- 2CH: `web/NailSense-2CH/index.html`
- 6CH: `web/NailSense-6CH/index.html`

Click **Connect**, select the Teensy port, select a folder, and click
**Record**.

## 3. Python CSV recorder

Python 3.9 or newer is required.

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e .
nailsense --list-ports
```

Record 2CH:

```bash
nailsense --port /dev/cu.usbmodemXXXX --sensors 2
```

Record 6CH:

```bash
nailsense --port /dev/cu.usbmodemXXXX --sensors 6
```

On Windows, use a port such as `COM4`. Press `Ctrl+C` to stop recording.

## CSV format

```text
Sequence,Time_s,SG1_CH1,SG1_CH2,...
```

The data values are baseline-corrected microvolts (`µV`).
