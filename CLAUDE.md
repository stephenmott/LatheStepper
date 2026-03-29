# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LatheStepper is an Arduino sketch that controls a stepper motor for lathe carriage positioning. It runs on a **Raspberry Pi Pico W (RP2040)** with a TMC2100 driver, 20×4 I2C LCD, two rotary encoders, and three push buttons. The controller is positional — it cuts to a stored left limit and rapid-returns to home, repeating on demand.

## Build & Upload

This project uses the Arduino IDE or `arduino-cli` with the **Earle Philhower arduino-pico core** — NOT the official "Arduino Mbed OS RP2040 Boards" core.

**Why Philhower, not the official Arduino core?**
The official Arduino Mbed RP2040 core does support the Pico, but in practice has worse compatibility with AVR-era Arduino libraries (LiquidCrystal_I2C, StepperDriver etc.) and uses a different Wire API that doesn't have `setSDA()`/`setSCL()`. The Philhower core is purpose-built for RP2040 and is the de facto standard for Arduino-on-Pico.

**Arduino IDE setup (once):**
1. File → Preferences → Additional boards manager URLs, add:
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
2. Tools → Board Manager → search `rp2040` → install **"Raspberry Pi RP2040" by Earle F. Philhower III**
3. Tools → Board → Raspberry Pi RP2040 Boards → **Raspberry Pi Pico W**

**Expected warnings on successful compile (both harmless):**
- `LiquidCrystal I2C claims to run on avr architecture` — the library predates non-AVR Arduino boards; it works fine on RP2040
- `'B00000001' is deprecated` — LiquidCrystal_I2C uses old AVR-style binary literals; functionally identical to modern `0b00000001` syntax

**Flash size — required for OTA:**
Tools → Flash Size → **"2MB (Sketch: 1MB, FS: 1MB)"**
The Philhower OTA stores incoming firmware in LittleFS before applying it, so a filesystem partition is required. The sketch is ~415 KB so 1 MB sketch space is ample. This setting must be used for both the initial USB flash and all subsequent OTA uploads.

```bash
# Install the rp2040 core (once)
arduino-cli core install rp2040:rp2040

# Compile (with filesystem partition for OTA)
arduino-cli compile --fqbn rp2040:rp2040:rpipicow:flash=2097152_1048576 LatheStepper/

# Upload via USB (first flash, or recovery)
arduino-cli upload -p /dev/cu.usbmodem* --fqbn rp2040:rp2040:rpipicow:flash=2097152_1048576 LatheStepper/

# Upload via WiFi OTA (once OTA firmware is running)
arduino-cli upload --port <pico-ip-address> --fqbn rp2040:rp2040:rpipicow:flash=2097152_1048576 LatheStepper/

# Monitor serial output (9600 baud)
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=9600
```

**OTA setup:**
1. Copy `secrets.h.example` → `secrets.h` and fill in your WiFi credentials
2. Flash once via USB with the filesystem partition setting above
3. On boot the LCD briefly shows the Pico's IP address; it also prints to Serial
4. Subsequent uploads: Arduino IDE → Tools → Port → Network Ports (select Pico IP), or use `arduino-cli upload --port <IP>` as above
5. If WiFi is unavailable at boot, the lathe starts normally after a 10 s timeout — OTA is just disabled that session

Libraries must be installed in `~/Documents/Arduino/libraries/`:
- `StepperDriver` — motor control (by Laurentiu Badea — use the `TMC2100` class)
- `LiquidCrystal_I2C` — I2C LCD driver

## Hardware Configuration

All logic runs at 3.3V — fully compatible with TMC2100 logic inputs. GP0–GP15 all sit on the left-hand edge of the Pico. The Pico is mounted USB-up so GP13–GP15 sit physically adjacent to the TMC2100, keeping motor signal traces short.

| Pin | Function |
|-----|----------|
| GP0 | Forward button |
| GP1 | Reverse button |
| GP2 | Start/Stop button (remote, twisted pair to motor box) |
| GP3 | DIR (TMC2100, hard-wired) |
| GP4 | Enc2 VCC (OUTPUT HIGH — supplies jog encoder ~1 mA) |
| GP5 | Jog encoder CLK (interrupt) |
| GP6 | Jog encoder DT |
| GP7 | Jog encoder SW (push button — set home / set limit) |
| GP8 | (free — gap between ENC2 and ENC1 plugs) |
| GP9 | Speed encoder CLK (interrupt) |
| GP10 | Speed encoder DT |
| GP11 | Enc1 VCC (OUTPUT HIGH — supplies speed encoder ~1 mA) |
| GP12 | STEP (TMC2100, hard-wired) |
| GP13 | ENABLE (active LOW, hard-wired to TMC2100) |
| GP14 | LCD SDA (I2C1 / Wire1) |
| GP15 | LCD SCL (I2C1 / Wire1) |

LCD: I2C address `0x27`, 20×4 characters.
Motor: 400 steps/rev (0.9°/step), leadscrew M8 × 1.5 mm pitch → 4267 steps/mm at 16× microstep.
Microstepping: set by TMC2100 CFG pins (hardwired on module). Update `MICROSTEPS` in sketch to match. Default assumes 16× — verify empirically if unsure.

## Architecture

All logic lives in `LatheStepper.ino`.

**State machine** (`enum State`):

| State | Description |
|-------|-------------|
| `ST_STARTUP` | Show stored speeds; RPM knob adjusts cut RPM, JOG knob adjusts rapid RPM. Start/Stop or LEFT confirms. |
| `ST_HOMING` | Jog carriage to home position with JOG knob. Press JOG SW to zero position. |
| `ST_SETUP` | Jog to left limit with JOG knob. Press JOG SW to store limit. Auto rapid-returns to home on confirm. |
| `ST_READY` | Waiting to cut. LEFT toggles RPM knob between cut/rapid speed editing. JOG SW enters jog mode. Start/Stop begins cycle. |
| `ST_JOG` | Free jog mode for loading/unloading. JOG knob moves carriage. JOG SW returns to ST_READY. Start/Stop begins cut. |
| `ST_CUTTING` | Running to left limit at cut speed. RPM knob adjusts speed mid-cut. Completion auto-triggers return. |
| `ST_RETURNING` | Rapid return to home. Completion returns to ST_READY. |
| `ST_STOPPED` | Emergency stop. LEFT/Start = resume cut; RIGHT = return home. |

**Dual-core architecture:**
- **Core 0** (`loop()`) — state machine, button/encoder polling, LCD updates, EEPROM
- **Core 1** (`loop1()`) — tight loop calling `stepper.nextAction()` as fast as possible for smooth step timing
- `motionActive` (volatile bool) — set by core 0 to start a move; cleared by core 1 when complete
- `motionComplete` (volatile bool) — set by core 1 when move finishes; core 0 acts on it and clears it
- This split is critical: LCD I2C updates on core 0 would otherwise cause jerky stepping

**Encoder ISRs:**
- Both encoders use a full quadrature state-machine decoder (`QEM[16]` lookup table)
- Interrupts on CHANGE of both CLK and DT pins — catches all four edges per detent
- Accumulates sub-counts; fires a tick every 4 valid quadrature steps (one physical detent on EC11 encoders)
- Invalid transitions (bounce) return 0 — self-debouncing, no time-based debounce needed
- If it takes two turns per step, change the `>= 4` threshold to `>= 2` in both ISRs

**Position tracking:**
- `currentSteps` (volatile long) — absolute position in steps from home
- A RISING interrupt on `PIN_STEP` (an output pin — valid on RP2040) fires `stepISR()` on every pulse
- `stepDir` (+1/−1) is set before each move so the ISR counts in the correct logical direction
- Position survives emergency stop because every step is counted in hardware

**Key functions:**
- `startMove(targetSteps, rpm)` — calculates degrees, sets `stepDir`, calls `stepper.startRotate()`
- `startJog(ticks)` — moves `ticks × 0.1 mm` at `RPM_JOG`; no-op if motor already moving
- `stopMotor()` — calls `stepper.stop()` + `stepper.disable()`
- `saveSettings()` / `loadSettings()` — persist cut RPM, rapid RPM and limit to flash via EEPROM emulation
- `printSoftKeys(l, c, r)` — renders the row 3 button legend: left (6 chars), centre (8 chars centred), right (6 chars)

**Settings persistence (EEPROM emulation):**
- Philhower core provides `EEPROM.h` backed by a flash slice
- Stored: `cutRPM`, `rapidRPM`, `limitSteps`, `limitValid`
- Home is NOT stored — must be re-set each session by jogging + JOG SW press
- `SETTINGS_MAGIC` constant guards against reading stale/uninitialised flash; increment it if the `Settings` struct layout changes

**Display** (refreshed on state change or every 250 ms while moving):
- Row 0: position readout `Pos: XX.X / YY.Ymm`
- Row 1: speeds or state label
- Row 2: status / encoder hint
- Row 3: soft-key bar `[LEFT  ][ START  ][RIGHT ]` — context labels for the three buttons

**`DIRECTION_SIGN`** (`#define`, default `1`): flip to `−1` if the carriage moves the wrong way when jogging. Independent of encoder wiring — if JOG knob turns wrong way, swap its CLK/DT wires instead.

## Physical layout

Two enclosures — both 3D printed on Bambu H2D with multi-colour for integrated labels:
- **Top box** (mounted above lathe, visible): landscape format, LCD centred, RPM knob left, JOG knob + LEFT/RIGHT buttons right, sealed 16mm buttons
- **Motor box** (near motor/tailstock end): single sealed Start/Stop button
