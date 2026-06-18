# Serial protocol

The Teensy uses USB serial at **2,000,000 baud**.

## Startup

The firmware performs a three-second unloaded baseline calibration. Keep all
sensors unloaded and still during this period. It then sends:

```text
CAL_DONE
```

## Data frames

```text
S:<status>,<value1>,<value2>,...
```

Examples:

```text
S:O,12,-8,4,0,21,17,-3,5
S:OOO,12,-8,4,0,21,17,-3,5,...24 values total
```

Each value is baseline-corrected microvolts (`µV`). The values are not force
measurements unless a separate sensor calibration is applied.

There is one status character per ADC board. One board serves two sensors:

- `O`: data read successfully
- `T`: ADC data-ready timeout
- `R`: ADC board was reinitialized
- `C`: checksum failure
- `X`: unexpected ADC reset

The 2-sensor version has one status character and 8 values. The 6-sensor
version has three status characters and 24 values.

## Diagnostic lines

Lines beginning with `#` are diagnostics and must not be interpreted as
measurements. Sending `d` or `D` to the Teensy reruns bus diagnostics.

## Legacy compatibility

The Python reader also accepts older value-only CSV frames. They are saved
with status `LEGACY`.
