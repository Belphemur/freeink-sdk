# Xteink X3 — UC8279d controller variant

Newer X3 production units (Xteink heads-up, July 2026) ship the same ESP32-C3
board and 792×528 glass with a **UC8279d** panel controller in place of the
UC8253. Everything else — pinout, ADC ladder input, BQ27220/DS3231/QMI8658
peripherals, SD wiring — is unchanged. The variant has its own sibling profile,
`BoardConfig::XTEINK_X3_UC8279` (`Board::XteinkX3Uc8279`).

Build: nothing new — `-DFREEINK_DEVICE_X3=1` links both X3 drivers
(`FREEINK_DRIVER_UC8253_X3` and `FREEINK_DRIVER_UC8279`); which one runs is
decided at boot.

The detection section reflects the stock V6.3.15 protocol. The driver notes
below describe the initial OTP-based proposal and are historical; the current
driver uses recovered external waveforms and register initialization. See the
[V6.3.15 audit](x3-v6.3.15-firmware-audit.md) for the verified comparison.

## Runtime detection

After the X3 I2C fingerprint, `detectX3DisplayController()` uses the fixed X3
pins (SCLK 8 / SDA 10 / CS 21 / DC 4 / RST 5 / BUSY 6), even while the active
profile still names X4. `detectXteinkDisplayController()` uses the same X3
protocol when either X3 profile is already active.

The probe follows stock V6.3.15: RESET high 10 ms, low 50 ms, high 50 ms;
wait up to 300 ms for BUSY high; release SDA to input and read three bytes of
**VER (0x70)**, sampling after SCLK rises. A BUSY timeout is recorded but does
not suppress the read, as in stock firmware. The third byte selects the panel:
`0x66` confirms UC8279, `0xFF` assumes UC8253, other IDs are inconclusive and
leave the default UC8253 selected. FLG and MTP are not read or required.

The legacy five-byte VER out-param contains the three read bytes and two zeros;
the FLG out-param is zero (not sampled). Diagnostics expose `verBytesRead=3`
and `busyTimedOut`. Boot logs show `[XTDET] X3 stock probe VER=...` with the
selection and timeout status. X4-family probes retain their existing protocol.

Hardware validation remains necessary: collect these logs on affected units
and check cold boot and sleep/wake. This change updates identification only;
display refresh reset timing, SPI frequency, and waveforms are separate tests.

## Driver — `Uc8279Driver`

KW mode (`PSR KW/R=1`): 1-bpp, DTM1 = OLD plane, DTM2 = NEW plane,
differential refresh — the same paradigm as the UC8253 X3 driver, and a
near-identical command set (PSR/PON/POF, DTM1 `0x10`, DSP `0x11`, DRF `0x12`,
DTM2 `0x13`, CDI `0x50`, TRES `0x61`, DSLP `0x07`+`0xA5`).

v1 uses the **factory OTP waveforms** (`PSR REG=0`): the 4K MTP carries 12
temperature-range LUT sets, each with its own frame rate and rail voltages, and
`TS_AUTO` re-senses temperature before every booster enable — so PWR/PLL/VDCS
stay at silicon defaults and every refresh is temperature-compensated by the
controller. Consequences, all **Pending** bench tuning:

- Full/Half/Fast currently run the same OTP waveform (likely a full GC-style
  flash on every page turn). Fast page turns need custom register banks
  (`REG=1`, commands `0x20`–`0x24`) — note the UC8279 LUT format is
  **group-based** (7-byte groups, 7 groups per LUT in KW mode), *not* the
  UC8253's 43-byte format, so the X3's six tuned banks cannot be copied over.
- No grayscale yet (`supportsStripGrayscale()` false); the X3 reader's 4-level
  AA path needs UC8279-format gray banks tuned on hardware.
- TRES is programmed 792×528. The UC8253 X3 init programs VRES=600 (OEM scans
  the full gate count); if the panel image is offset/compressed, try `0x02 0x58`
  (see the note in `Uc8279Driver::initController`).
- CDI default drives the border white each refresh (`0x97`); the datasheet
  default (`0xD7`) floats it instead. Injectable via `Uc8279Config`
  (`-DFREEINK_UC8279_CONFIG=yourConfig`, same idiom as the other drivers).

## Useful UC8279 features not yet wired

- **AUTO (0x17)**: `PON→DRF→POF(→DSLP)` as one command — could shave host
  round-trips on sleepy ESL-style updates.
- **PBC (0x44)**: panel-break check via the CHKGI/CHKGO wire loop, if the
  module bonds it.
- **CRC (0x72)**: MTP integrity check over `0x000–0xFFF`.
- On-chip temperature readback (**TSC 0x40**) if the consumer ever wants it.
