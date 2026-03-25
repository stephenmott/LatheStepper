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
2. Tools → Board Manager → search `rp2040` → install **"Raspberry Pi RP2040" by Earle F. Philhower III** — use version **4.4.0**
3. Tools → Board → Raspberry Pi RP2040 Boards → **Raspberry Pi Pico W**

> ⚠️ **Do not upgrade past 4.4.0.** Version 5.x has a known linker bug with the Pico W where the CYW43 WiFi runtime symbols fail to resolve, producing a wall of `undefined reference to '__wrap_cyw43_*'` errors. If you accidentally upgrade via "Update All", roll back to 4.4.0 in Board Manager.

**Expected warnings on successful compile (both harmless):**
- `LiquidCrystal I2C claims to run on avr architecture` — the library predates non-AVR Arduino boards; it works fine on RP2040
- `'B00000001' is deprecated` — LiquidCrystal_I2C uses old AVR-style binary literals; functionally identical to modern `0b00000001` syntax

```bash
# Install the rp2040 core (once)
arduino-cli core install rp2040:rp2040

# Compile
arduino-cli compile --fqbn rp2040:rp2040:rpipicow LatheStepper/

# Upload (Pico W appears as a USB serial port on Mac — adjust port as needed)
arduino-cli upload -p /dev/cu.usbmodem* --fqbn rp2040:rp2040:rpipicow LatheStepper/

# Monitor serial output (9600 baud)
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=9600
```

Libraries must be installed in `~/Documents/Arduino/libraries/`:
- `StepperDriver` — motor control (by Laurentiu Badea — use the `TMC2100` class)
- `LiquidCrystal_I2C` — I2C LCD driver

## Hardware Configuration

All logic runs at 3.3V — fully compatible with TMC2100 logic inputs. GP0–GP15 all sit on the left-hand edge of the Pico, keeping wiring to one side. GP5–GP7 are free.

| Pin | Function |
|-----|----------|
| GP0 | LCD SDA (I2C0) |
| GP1 | LCD SCL (I2C0) |
| GP2 | ENABLE (active LOW) |
| GP3 | DIR |
| GP4 | STEP |
| GP5–GP7 | (free) |
| GP8 | Speed encoder CLK (interrupt) |
| GP9 | Speed encoder DT |
| GP10 | Forward button |
| GP11 | Reverse button |
| GP12 | Start/Stop button (remote box near motor) |
| GP13 | Jog encoder CLK (interrupt) |
| GP14 | Jog encoder DT |
| GP15 | Jog encoder SW (push button — set home / set limit) |

LCD: I2C address `0x27`, 20×4 characters.
Motor: 400 steps/rev (0.9°/step), leadscrew M8 × 1.5 mm pitch → 4267 steps/mm at 16× microstep.
Microstepping: set by TMC2100 CFG pins (hardwired on module). Update `MICROSTEPS` in sketch to match. Default assumes 16× — verify empirically if unsure.

## Architecture

All logic lives in `LatheStepper.ino`.

**State machine** (`enum State`):

| State | Description |
|-------|-------------|
| `ST_STARTUP` | Show stored speeds; enc1 adjusts cut RPM, enc2 adjusts rapid RPM. Start/Stop confirms. |
| `ST_HOMING` | Jog carriage to home position with enc2. Press enc2 SW to zero position. |
| `ST_SETUP` | Jog to left limit with enc2. Press enc2 SW to store limit. |
| `ST_READY` | Waiting to cut. Start/Stop begins a cycle. |
| `ST_CUTTING` | Running to left limit at cut speed. Completion auto-triggers return. |
| `ST_RETURNING` | Rapid return to home. Completion returns to ST_READY. |
| `ST_STOPPED` | Emergency stop. Fwd/Start = resume cut; Rev = return home. |

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

**Settings persistence (EEPROM emulation):**
- Philhower core provides `EEPROM.h` backed by a flash slice
- Stored: `cutRPM`, `rapidRPM`, `limitSteps`, `limitValid`
- Home is NOT stored — must be re-set each session by jogging + enc2 SW press
- `SETTINGS_MAGIC` constant guards against reading stale/uninitialised flash; increment it if the `Settings` struct layout changes

**Display** (refreshed on state change or every 250 ms while moving):
- Row 0: position readout `Pos: XX.X / YY.Ymm`
- Row 1: speeds or state label
- Row 2: status / direction arrow
- Row 3: context hint for next action

**`DIRECTION_SIGN`** (`#define`, default `1`): flip to `−1` if the carriage moves the wrong way when jogging. Independent of encoder wiring — if enc2 turns in the wrong direction, swap its CLK/DT wires instead.

## Physical layout

Two enclosures:
- **Top box** (mounted above lathe, visible): LCD, speed encoder (enc1), jog encoder (enc2) with push button, Forward button, Reverse button
- **Motor box** (near motor/tailstock end): Start/Stop button only
