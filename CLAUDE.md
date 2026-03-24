# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LatheStepper is an Arduino sketch that controls a stepper motor for lathe tool positioning. It uses a DRV8825 driver with 32x microstepping, a 20x4 I2C LCD display, and four push buttons.

## Build & Upload

This project uses the Arduino IDE or `arduino-cli`. The StepperDriver library (in `~/Documents/Arduino/libraries/StepperDriver/`) includes `arduino-cli.yaml` and `Makefile` for CLI builds.

```bash
# Compile with arduino-cli (adjust board FQBN as needed)
arduino-cli compile --fqbn arduino:avr:uno LatheStepper/

# Upload
arduino-cli upload -p /dev/ttyUSB0 --fqbn arduino:avr:uno LatheStepper/

# Monitor serial output (9600 baud)
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=9600
```

Libraries must be installed in `~/Documents/Arduino/libraries/`:
- `StepperDriver` — motor control
- `OneButton` — multi-state button events
- `LiquidCrystal_I2C` — I2C LCD driver

## Hardware Configuration

| Pin | Function |
|-----|----------|
| 4 | ENABLE (active LOW) |
| 8 | DIR |
| 9 | STEP |
| 10–12 | MODE0/MODE1/MODE2 (microstep select) |
| 2 | Encoder CLK (interrupt pin) |
| 3 | Encoder DT |
| A0 | Forward button |
| A1 | Reverse button |
| A2 | Start/Stop button (intended for separate box near motor) |

LCD: I2C address `0x27`, 20×4 characters.
Motor: 400 steps/rev (0.9°/step), default 100 RPM, 32x microstepping, range 10–400 RPM.

## Architecture

All logic lives in `LatheStepper.ino`. The motor driver class hierarchy from the StepperDriver library:

```
BasicStepperDriver  →  A4988  →  DRV8825 (active driver)
```

**State variables:**
- `rpm` (int) — current motor speed, adjusted by encoder in 10 RPM steps
- `running` (bool) — whether the motor is currently turning
- `dir` (int8_t) — 1 = forward, −1 = reverse
- `encTicks` (volatile int) — raw encoder tick accumulator, read and cleared in `loop()`

**Control behaviour:**
- **Forward / Reverse buttons** — set direction and call `startMotor()` (works whether stopped or already running, allowing instant direction change)
- **Start/Stop button** — toggles between `startMotor()` and `stopMotor()`; designed to live in a separate enclosure near the motor
- **Rotary encoder** — increments/decrements `rpm` by 10, clamped to 10–400; calls `stepper.setRPM()` mid-run if motor is already running

**Continuous-run motor control** (non-blocking):
- `startMotor()` calls `stepper.startRotate(dir × 360000°)` (~1000 revolutions per block)
- `loop()` calls `stepper.nextAction()` every iteration to pulse the STEP pin at the correct time
- When `nextAction()` returns 0 (block complete), `loop()` immediately starts the next block — no pause

**Display** (refreshed on any state change):
- Row 0: `>> FORWARD` / `<< REVERSE` (arrows shown only while running)
- Row 1: `Speed:  NNN RPM`
- Row 2: `Status: RUNNING` / `Status: STOPPED`

## Physical layout

Two enclosures are planned:
- **Top box** (mounted above lathe, visible): LCD, rotary encoder, Forward button, Reverse button
- **Motor box** (near motor/tailstock end): Start/Stop button only
