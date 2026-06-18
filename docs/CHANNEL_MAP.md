# Sensor and channel map

Each nail sensor contains four strain-gauge channels. One ADS1263 board reads
two sensors, for eight values per board.

| Serial positions | ADC board | Sensor | CSV columns |
|---|---:|---:|---|
| 1–4 | 1 | SG1 | `SG1_CH1`–`SG1_CH4` |
| 5–8 | 1 | SG2 | `SG2_CH1`–`SG2_CH4` |
| 9–12 | 2 | SG3 | `SG3_CH1`–`SG3_CH4` |
| 13–16 | 2 | SG4 | `SG4_CH1`–`SG4_CH4` |
| 17–20 | 3 | SG5 | `SG5_CH1`–`SG5_CH4` |
| 21–24 | 3 | SG6 | `SG6_CH1`–`SG6_CH4` |

Within every ADC board, the positive ADS1263 inputs are read in this order:

```text
AIN6, AIN7, AIN8, AIN9, AIN2, AIN3, AIN4, AIN5
```

All channels use AIN1 as the fixed negative input. If the physical connector
labels differ, document that cable-specific mapping before collecting data.
