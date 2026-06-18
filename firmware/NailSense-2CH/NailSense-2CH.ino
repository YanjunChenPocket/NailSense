#include <Arduino.h>
#include <SPI.h>

/**
 * ADS1263 2-sensor HARDENED firmware (Teensy 4.0)
 * One ADC board reads two 4-channel nail sensors (8 values total).
 * Shared SPI bus (SCK=13, MOSI=11, MISO=12); per-board CS + DRDY.
 * This build enables one ADC board. Use the 6-sensor build for three boards.
 *
 * Fixes vs ADS1263_4CH.ino:
 *  1. DRDY_TIMEOUT was 30ms but FIR@20SPS converts every 50ms (first data
 *     ~52ms) -> constant false timeouts -> endless reset loop ("hang").
 *     Timeout is now 150ms.
 *  2. STATUS + CHECKSUM bytes enabled: SPI transmission errors detected
 *     and re-read instead of silently accepted.
 *  3. Unexpected chip reset (power glitch) detected via STATUS RESET bit
 *     (cleared at init with POWER=0x01) -> immediate re-init.
 *  4. Config registers read back every REG_VERIFY_MS; corruption -> re-init.
 *  5. Boards convert in PARALLEL: INPMUX written to all boards first, then
 *     each board is read. Scan time stays ~constant as boards are added.
 *
 * Serial protocol:
 *   "S:<flags>,v1,...,v(8*N)"   flags = one char per board:
 *     O=ok  T=timeout  R=reinit happened  C=checksum fail  X=unexpected reset
 *   "CAL_DONE" once after baseline calibration.
 *   Lines starting with '#' are diagnostics; parsers should ignore them.
 *
 * HARDWARE CHECKLIST (per board):
 *   - RESET/PWDN pulled up to DVDD (10k). Floating = random power-down!
 *   - START tied to DGND (command-controlled).
 *   - Shared SCK/MOSI: keep stubs short; 22-33R series resistor at the
 *     Teensy end helps when stacking several boards.
 *   - DOUT is Hi-Z while CS is high, so sharing MISO is safe.
 */

// ---------------- Boards: add entries here to scale ----------------
struct AdcBoard { int csPin; int drdyPin; };

// Teensy pin wired to the shared bus REST line. The ADS1263 RESET/PWDN
// input must NEVER float (random resets/power-down!). Set to the actual
// pin number and the firmware will drive it HIGH; -1 if hard-wired to 3.3V.
static const int RESET_PIN = 10; // bus REST line -> Teensy D10, must be driven HIGH

static const uint8_t NUM_BOARDS = 1;
static const AdcBoard BOARDS[NUM_BOARDS] = {
  {23, 2},   // Circuit 1
};

// ---------------- ADS1263 Commands ----------------
static const uint8_t CMD_RESET  = 0x06;
static const uint8_t CMD_START1 = 0x08;
static const uint8_t CMD_STOP1  = 0x0A;
static const uint8_t CMD_RDATA1 = 0x12;

// ---------------- ADS1263 Registers ----------------
static const uint8_t REG_POWER     = 0x01;
static const uint8_t REG_INTERFACE = 0x02;
static const uint8_t REG_MODE0     = 0x03;
static const uint8_t REG_MODE1     = 0x04;
static const uint8_t REG_MODE2     = 0x05;
static const uint8_t REG_INPMUX    = 0x06;
static const uint8_t REG_REFMUX    = 0x0F;

// ---------------- Configuration values ----------------
// FILTER choice (MODE1 bits 7:5) — affects speed vs noise at 4800SPS:
//   sinc1=0x00 (latency 1/DR ~208us, noisiest)
//   sinc2=0x20 (~417us)  sinc3=0x40 (~625us)  sinc4=0x60 (~833us, cleanest)
//   FIR =0x80 is ONLY valid at 2.5-20SPS — was misused before at 4800SPS!
static const uint8_t FILTER_SINC1  = 0x00; // fastest, noisiest (~208us)
static const uint8_t FILTER_SINC2  = 0x20; // ~417us
static const uint8_t FILTER_SINC3  = 0x40; // ~625us  <- current default
static const uint8_t FILTER_SINC4  = 0x60; // ~833us, cleanest
static const uint8_t POWER_VAL     = 0x01; // RESET flag cleared, INTREF on
static const uint8_t INTERFACE_VAL = 0x05; // STATUS byte + CHECKSUM mode
static const uint8_t MODE0_VAL     = 0x00;
static const uint8_t MODE1_VAL     = FILTER_SINC3; // speed/noise tradeoff knob
static const uint8_t MODE2_VAL     = 0x0B; // Gain=1, DR=1011 -> 4800 SPS (NOT 20SPS; 20SPS = 0x04)
static const uint8_t REFMUX_VAL    = 0x09; // External VREF (REFP0/REFN0)
static const float   ADC_GAIN      = 1.0f;

// ---------------- MUX ----------------
static const uint8_t MUX_AIN1 = 0x01;
static const uint8_t NUM_CHANNELS = 8;
// CH1~CH4: AIN6~AIN9, CH5~CH8: AIN2~AIN5
static const uint8_t CH_POS[NUM_CHANNELS] = {0x06, 0x07, 0x08, 0x09, 0x02, 0x03, 0x04, 0x05};

// ---------------- Constants ----------------
static const double   FS         = 2147483648.0; // 2^31
static const float    VREF_VOLTS = 1.65f;
// 4MHz: same as the original single-board build; checksum validation
// will catch (and # STATS will count) any integrity problem — if
// chkfail counters rise, drop back to 2000000.
static const uint32_t SPI_HZ     = 4000000;
static const uint32_t CAL_MS     = 3000;
// Generous recovery timeout; normal sinc3 conversion latency is ~625 us.
static const uint32_t DRDY_TIMEOUT_US = 150000;
static const uint8_t  MAX_TIMEOUTS_BEFORE_REINIT = 3;
static const uint32_t REG_VERIFY_MS = 5000;
static const uint32_t STATS_MS      = 30000;

// STATUS byte bits
static const uint8_t ST_RESET = 0x01;

// Result of a validated channel read. Declared before any function so the
// Arduino auto-generated prototypes can see it.
enum ReadResult : uint8_t { R_OK, R_TIMEOUT, R_RESET, R_CHKFAIL };

// ---- Raw frame debug: prints first frames in hex so framing problems
// (byte offset, repeated bytes, stuck MISO) become visible. ----
static uint8_t  gFrame[6];           // last raw frame: ST D0 D1 D2 D3 CHK
static uint16_t rawDebugBudget = 60; // raw lines to print, then quiet

// ---------------- State ----------------
static int64_t  baseline_uV[NUM_BOARDS][NUM_CHANNELS] = {0};
static int64_t  accum_uV[NUM_BOARDS][NUM_CHANNELS]    = {0};
static int32_t  last_uV[NUM_BOARDS][NUM_CHANNELS]     = {0};
static uint8_t  consecTimeouts[NUM_BOARDS] = {0};
static char     statusFlags[NUM_BOARDS];
static uint32_t sampleCount = 0;
static bool     calibrated  = false;
static uint32_t calStartMs  = 0;
static uint32_t lastVerifyMs = 0, lastStatsMs = 0;
static uint32_t reinitCount[NUM_BOARDS] = {0};
static uint32_t timeoutCount[NUM_BOARDS] = {0};
static uint32_t chkFailCount[NUM_BOARDS] = {0};
static uint32_t resetFlagCount[NUM_BOARDS] = {0};

// ---------------- SPI low-level ----------------
static inline void csLow(uint8_t b)  { digitalWrite(BOARDS[b].csPin, LOW);  delayMicroseconds(1); }
static inline void csHigh(uint8_t b) { delayMicroseconds(1); digitalWrite(BOARDS[b].csPin, HIGH); }

void deselectAll() {
  for (uint8_t b = 0; b < NUM_BOARDS; b++) digitalWrite(BOARDS[b].csPin, HIGH);
  delayMicroseconds(2);
}

void writeCmd(uint8_t b, uint8_t cmd) {
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE1));
  csLow(b);
  SPI.transfer(cmd);
  csHigh(b);
  SPI.endTransaction();
}

void writeReg(uint8_t b, uint8_t reg, uint8_t val) {
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE1));
  csLow(b);
  SPI.transfer(0x40 | reg);
  SPI.transfer(0x00);
  SPI.transfer(val);
  csHigh(b);
  SPI.endTransaction();
}

uint8_t readReg(uint8_t b, uint8_t reg) {
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE1));
  csLow(b);
  SPI.transfer(0x20 | reg);
  SPI.transfer(0x00);
  uint8_t v = SPI.transfer(0xFF);
  csHigh(b);
  SPI.endTransaction();
  return v;
}

/**
 * Wait for DRDY low. Writing INPMUX restarts the conversion and forces
 * DRDY high, so after a MUX write any LOW level means settled new data
 * is ready (no need to wait for a fresh falling edge).
 */
bool waitDrdyLow(uint8_t b, uint32_t timeout_us = DRDY_TIMEOUT_US) {
  uint32_t t0 = micros();
  const int pin = BOARDS[b].drdyPin;
  while (digitalRead(pin) == HIGH) {
    if ((micros() - t0) > timeout_us) return false;
  }
  return true;
}

/** Read one ADC1 frame: STATUS + 4 data + CHECKSUM. True if checksum OK. */
bool readFrame(uint8_t b, uint8_t &status, int32_t &code) {
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE1));
  csLow(b);
  SPI.transfer(CMD_RDATA1);
  status = SPI.transfer(0xFF);
  uint8_t d0 = SPI.transfer(0xFF);
  uint8_t d1 = SPI.transfer(0xFF);
  uint8_t d2 = SPI.transfer(0xFF);
  uint8_t d3 = SPI.transfer(0xFF);
  uint8_t chk = SPI.transfer(0xFF);
  csHigh(b);
  SPI.endTransaction();

  code = (int32_t)(((uint32_t)d0 << 24) | ((uint32_t)d1 << 16) |
                   ((uint32_t)d2 << 8) | d3);
  gFrame[0]=status; gFrame[1]=d0; gFrame[2]=d1;
  gFrame[3]=d2; gFrame[4]=d3; gFrame[5]=chk;
  return (uint8_t)(d0 + d1 + d2 + d3 + 0x9B) == chk;
}

void printRawFrame(uint8_t b, uint8_t ch, bool ok) {
  if (rawDebugBudget == 0) return;
  rawDebugBudget--;
  Serial.print("# RAW b="); Serial.print(b + 1);
  Serial.print(" ch=");     Serial.print(ch + 1);
  Serial.print(ok ? " OK " : " BAD ");
  static const char* names[6] = {"st", "d0", "d1", "d2", "d3", "chk"};
  for (int i = 0; i < 6; i++) {
    Serial.print(names[i]); Serial.print('=');
    if (gFrame[i] < 16) Serial.print('0');
    Serial.print(gFrame[i], HEX); Serial.print(' ');
  }
  Serial.print("calc=");
  uint8_t calc = (uint8_t)(gFrame[1] + gFrame[2] + gFrame[3] + gFrame[4] + 0x9B);
  if (calc < 16) Serial.print('0');
  Serial.println(calc, HEX);
}

int32_t code_to_uV(int32_t code) {
  double volts = ((double)code / FS) * (VREF_VOLTS / ADC_GAIN);
  return (int32_t)(volts * 1e6);
}

// ---------------- Init / verify / recover ----------------

struct RegDef { uint8_t reg; uint8_t val; const char* name; };
static const RegDef CFG_REGS[] = {
  {REG_POWER,     POWER_VAL,     "POWER"},
  {REG_INTERFACE, INTERFACE_VAL, "INTERFACE"},
  {REG_MODE0,     MODE0_VAL,     "MODE0"},
  {REG_MODE1,     MODE1_VAL,     "MODE1"},
  {REG_MODE2,     MODE2_VAL,     "MODE2"},
  {REG_REFMUX,    REFMUX_VAL,    "REFMUX"},
};
static const uint8_t NUM_CFG_REGS = sizeof(CFG_REGS) / sizeof(CFG_REGS[0]);

bool verifyRegs(uint8_t b, bool verbose) {
  bool ok = true;
  for (uint8_t i = 0; i < NUM_CFG_REGS; i++) {
    uint8_t got = readReg(b, CFG_REGS[i].reg);
    if (got != CFG_REGS[i].val) {
      ok = false;
      if (verbose) {
        Serial.print("# REG_MISMATCH b="); Serial.print(b + 1);
        Serial.print(" "); Serial.print(CFG_REGS[i].name);
        Serial.print(" got=0x"); Serial.print(got, HEX);
        Serial.print(" want=0x"); Serial.println(CFG_REGS[i].val, HEX);
      }
    }
  }
  return ok;
}

bool initBoard(uint8_t b) {
  deselectAll();
  writeCmd(b, CMD_RESET);
  delay(10);
  writeCmd(b, CMD_STOP1);

  for (uint8_t i = 0; i < NUM_CFG_REGS; i++) {
    writeReg(b, CFG_REGS[i].reg, CFG_REGS[i].val);
  }
  if (!verifyRegs(b, true)) return false;

  writeCmd(b, CMD_START1);
  consecTimeouts[b] = 0;
  return true;
}

void reinitBoard(uint8_t b, const char* cause) {
  reinitCount[b]++;
  statusFlags[b] = 'R';
  Serial.print("# REINIT b="); Serial.print(b + 1);
  Serial.print(" cause="); Serial.print(cause);
  Serial.print(" total="); Serial.println(reinitCount[b]);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (initBoard(b)) return;
    Serial.print("# INIT_FAIL b="); Serial.println(b + 1);
    delay(50);
  }
}

// ---------------- Channel read with validation ----------------

/**
 * Read one settled conversion. The INPMUX write already RESTARTED the
 * conversion (datasheet: INPMUX is an "ADC restart" register), so the
 * first DRDY after it is fully settled — no discards needed. This is
 * 3x faster than the old discard-2-read-3rd pattern.
 */
ReadResult readSettled_uV(uint8_t b, uint8_t ch, int32_t &out_uV) {
  uint8_t st; int32_t code;

  if (!waitDrdyLow(b)) return R_TIMEOUT;

  for (int attempt = 0; attempt < 2; attempt++) {
    bool chkOk = readFrame(b, st, code);
    printRawFrame(b, ch, chkOk);     // debug: dump first frames raw
    if (st & ST_RESET) return R_RESET;
    if (chkOk) { out_uV = code_to_uV(code); return R_OK; }
    chkFailCount[b]++;
  }
  return R_CHKFAIL;
}

// ---------------- Boot bus diagnostics ----------------
// Maps which CS actually talks to which chip, and which DRDY pin follows
// which chip. Results print as '# DIAG' lines.

static const uint8_t REG_ID     = 0x00;
static const uint8_t REG_OFCAL0 = 0x07; // safe scratch register for ID test

uint16_t countDrdyEdges(uint8_t d, uint32_t window_us) {
  const int pin = BOARDS[d].drdyPin;
  uint16_t edges = 0;
  int prev = digitalRead(pin);
  uint32_t t0 = micros();
  while ((micros() - t0) < window_us) {
    int cur = digitalRead(pin);
    if (prev == HIGH && cur == LOW) edges++;
    prev = cur;
  }
  return edges;
}

void busDiagnostics() {
  Serial.println("# DIAG ===== CS/DRDY mapping test =====");

  // 0) Clock source per chip: STATUS byte bit5 (EXTCLK).
  //    0 = internal 7.3728MHz oscillator, 1 = external crystal/clock.
  for (uint8_t b = 0; b < NUM_BOARDS; b++) {
    uint8_t st; int32_t code;
    Serial.print("# DIAG b"); Serial.print(b + 1); Serial.print(" clock=");
    if (waitDrdyLow(b, 5000)) {
      (void)readFrame(b, st, code);
      Serial.println((st & 0x20) ? "EXTERNAL (crystal in use)" : "INTERNAL oscillator");
    } else {
      Serial.println("? (no DRDY)");
    }
  }

  // 1) Chip ID + register cross-write test.
  //    Write a distinct value into OFCAL0 of each board, then read back.
  //    If board1 returns board2's value -> CS23 reaches board2's chip.
  //    If readback is 00/FF garbage    -> no chip answers that CS.
  for (uint8_t b = 0; b < NUM_BOARDS; b++) writeReg(b, REG_OFCAL0, 0xA1 + b);
  for (uint8_t b = 0; b < NUM_BOARDS; b++) {
    Serial.print("# DIAG b"); Serial.print(b + 1);
    Serial.print(" ID=0x");   Serial.print(readReg(b, REG_ID), HEX);
    Serial.print(" OFCAL0=0x"); Serial.print(readReg(b, REG_OFCAL0), HEX);
    Serial.print(" (wrote 0x"); Serial.print(0xA1 + b, HEX);
    Serial.println(")");
  }
  for (uint8_t b = 0; b < NUM_BOARDS; b++) writeReg(b, REG_OFCAL0, 0x00);

  // 2) DRDY mapping: start one chip at a time, count edges on every
  //    DRDY pin. Expected: only its own pin toggles.
  for (uint8_t b = 0; b < NUM_BOARDS; b++) writeCmd(b, CMD_STOP1);
  delay(10);
  Serial.print("# DIAG all-stopped:");
  for (uint8_t d = 0; d < NUM_BOARDS; d++) {
    Serial.print(" DRDY"); Serial.print(d + 1);
    Serial.print('=');     Serial.print(countDrdyEdges(d, 20000));
  }
  Serial.println(" (expect all 0)");

  for (uint8_t b = 0; b < NUM_BOARDS; b++) {
    writeCmd(b, CMD_START1);
    delay(5);
    Serial.print("# DIAG started-b"); Serial.print(b + 1); Serial.print(':');
    for (uint8_t d = 0; d < NUM_BOARDS; d++) {
      Serial.print(" DRDY"); Serial.print(d + 1);
      Serial.print('=');     Serial.print(countDrdyEdges(d, 20000));
    }
    Serial.print(" (expect only DRDY"); Serial.print(b + 1);
    Serial.println(" > 0)");
    writeCmd(b, CMD_STOP1);
    delay(10);
  }

  // 3) Cross-reset experiment: does activity meant for board 1
  //    reset board 2? POWER bit4 is the chip's reset flag.
  //    T1: heavy SPI traffic with ALL CS HIGH -> tests bus coupling/REST.
  //    T2: RESET command on board1's CS    -> tests CS cross-wiring.
  if (NUM_BOARDS >= 2) {
    writeReg(1, REG_POWER, POWER_VAL);   // clear b2 reset flag
    delay(2);
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE1));
    for (int i = 0; i < 512; i++) SPI.transfer(0x06); // RESET opcodes, no CS
    SPI.endTransaction();
    delay(5);
    uint8_t p1 = readReg(1, REG_POWER);
    Serial.print("# DIAG T1 (traffic, no CS) b2 POWER=0x");
    Serial.print(p1, HEX);
    Serial.println((p1 & 0x10) ? "  ** b2 RESET by bus noise: coupling or floating REST line! **"
                               : "  ok, no reset");

    writeReg(1, REG_POWER, POWER_VAL);   // clear again
    delay(2);
    writeCmd(0, CMD_RESET);              // reset addressed to board 1 only
    delay(10);
    uint8_t p2 = readReg(1, REG_POWER);
    Serial.print("# DIAG T2 (RESET on CS_b1) b2 POWER=0x");
    Serial.print(p2, HEX);
    Serial.println((p2 & 0x10) ? "  ** b2 RESET via board1 CS: CS lines cross-wired! **"
                               : "  ok, CS truly independent");
    initBoard(1);                        // restore board 2 config
    initBoard(0);
  }

  for (uint8_t b = 0; b < NUM_BOARDS; b++) writeCmd(b, CMD_START1);
  Serial.println("# DIAG ===== end =====");
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(2000000);

  if (RESET_PIN >= 0) {            // never let the shared REST line float
    pinMode(RESET_PIN, OUTPUT);
    digitalWrite(RESET_PIN, LOW);  // clean hardware reset of ALL chips
    delayMicroseconds(100);        // >4 tCLK, well below power-down threshold
    digitalWrite(RESET_PIN, HIGH);
    delay(15);                     // chips operational ~9ms after release
  }

  for (uint8_t b = 0; b < NUM_BOARDS; b++) {
    pinMode(BOARDS[b].csPin, OUTPUT);
    digitalWrite(BOARDS[b].csPin, HIGH);
    pinMode(BOARDS[b].drdyPin, INPUT_PULLUP);
    statusFlags[b] = 'O';
  }

  SPI.begin();
  delay(20);

  for (uint8_t b = 0; b < NUM_BOARDS; b++) reinitBoard(b, "BOOT");

  delay(500);        // let the serial monitor attach
  busDiagnostics();  // print CS/DRDY mapping ('# DIAG' lines)

  calStartMs = millis();
  lastVerifyMs = millis();
  lastStatsMs = millis();
}

void loop() {
  // Send 'd' in the serial monitor to re-run bus diagnostics anytime.
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') {
      busDiagnostics();
      rawDebugBudget = 60;   // also dump the next 60 raw frames again
    }
  }

  int32_t current_uV[NUM_BOARDS][NUM_CHANNELS];
  bool scanValid = true;
  static uint32_t lastScanUs = 0;
  uint32_t scanT0 = micros();

  for (uint8_t b = 0; b < NUM_BOARDS; b++) statusFlags[b] = 'O';

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    // 1) Switch MUX on ALL boards first -> they settle in parallel.
    for (uint8_t b = 0; b < NUM_BOARDS; b++) {
      writeReg(b, REG_INPMUX, (uint8_t)((CH_POS[i] << 4) | (MUX_AIN1 & 0x0F)));
    }

    // 2) Then read each board (waits overlap, scan time ~constant in N).
    for (uint8_t b = 0; b < NUM_BOARDS; b++) {
      int32_t v = last_uV[b][i];
      ReadResult r = readSettled_uV(b, i, v);

      switch (r) {
        case R_OK:
          consecTimeouts[b] = 0;
          last_uV[b][i] = v;
          break;
        case R_TIMEOUT:
          scanValid = false;
          timeoutCount[b]++;
          statusFlags[b] = 'T';
          if (++consecTimeouts[b] >= MAX_TIMEOUTS_BEFORE_REINIT) {
            reinitBoard(b, "TIMEOUT");
          }
          break;
        case R_RESET:
          scanValid = false;
          resetFlagCount[b]++;
          statusFlags[b] = 'X';
          Serial.print("# RESET_FLAG b="); Serial.println(b + 1);
          reinitBoard(b, "RESET_FLAG");
          statusFlags[b] = 'X'; // preserve the root cause after reinit
          break;
        case R_CHKFAIL:
          scanValid = false;
          statusFlags[b] = 'C';
          break;
      }
      current_uV[b][i] = v;
    }
  }

  // Periodic register integrity check
  if (millis() - lastVerifyMs >= REG_VERIFY_MS) {
    lastVerifyMs = millis();
    for (uint8_t b = 0; b < NUM_BOARDS; b++) {
      if (!verifyRegs(b, true)) {
        reinitBoard(b, "REG_CORRUPT");
        scanValid = false;
      }
    }
  }

  // Periodic stats
  lastScanUs = micros() - scanT0;

  if (millis() - lastStatsMs >= STATS_MS) {
    lastStatsMs = millis();
    Serial.print("# STATS uptime_s="); Serial.print(millis() / 1000);
    Serial.print(" scan_us="); Serial.print(lastScanUs);
    for (uint8_t b = 0; b < NUM_BOARDS; b++) {
      Serial.print(" b"); Serial.print(b + 1);
      Serial.print("=reinit:");  Serial.print(reinitCount[b]);
      Serial.print(",timeout:"); Serial.print(timeoutCount[b]);
      Serial.print(",chk:");     Serial.print(chkFailCount[b]);
      Serial.print(",reset:");   Serial.print(resetFlagCount[b]);
    }
    Serial.println();
  }

  if (!calibrated) {
    if (millis() - calStartMs < CAL_MS) {
      if (scanValid) {
        for (uint8_t b = 0; b < NUM_BOARDS; b++)
          for (uint8_t i = 0; i < NUM_CHANNELS; i++)
            accum_uV[b][i] += current_uV[b][i];
        sampleCount++;
      }
    } else {
      if (sampleCount > 0) {
        for (uint8_t b = 0; b < NUM_BOARDS; b++)
          for (uint8_t i = 0; i < NUM_CHANNELS; i++)
            baseline_uV[b][i] = accum_uV[b][i] / (int64_t)sampleCount;
      }
      calibrated = true;
      Serial.println("CAL_DONE");
    }
  } else {
    Serial.print("S:");
    for (uint8_t b = 0; b < NUM_BOARDS; b++) Serial.print(statusFlags[b]);
    Serial.print(',');
    for (uint8_t b = 0; b < NUM_BOARDS; b++) {
      for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        Serial.print((int32_t)(current_uV[b][i] - baseline_uV[b][i]));
        if (!(b == NUM_BOARDS - 1 && i == NUM_CHANNELS - 1)) Serial.print(',');
      }
    }
    Serial.println();
  }
}
