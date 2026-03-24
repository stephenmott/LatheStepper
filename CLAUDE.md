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
| 13 | SLEEP (optional) |
| A0 | Button 3 |
| A1 | Button 1 |
| A2 | Button 2 |
| A3 | Button 4 |

LCD: I2C address `0x27`, 20×4 characters.
Motor: 400 steps/rev (0.9°/step), 200 RPM, 32x microstepping.

## Architecture

All logic lives in `LatheStepper.ino`. The motor driver class hierarchy from the StepperDriver library:

```
BasicStepperDriver  →  A4988  →  DRV8825 (active driver)
```

**State variables:**
- `angle` (int) — current step size in degrees (1–360), initialized to 40
- `aTotal` (long) — accumulated total rotation in degrees
- `distance` (float) — unused; display computes `aTotal * (OneRev / 360)` where `OneRev = 0.9` mm/rev

**Button behavior** (OneButton library, active-low):

| Button | Click | Double-click | Long press |
|--------|-------|--------------|------------|
| 1 (A1) | Rotate +`angle`° | Increment `angle`, reset `aTotal` | Continuous +`angle`° |
| 2 (A2) | Rotate −`angle`° | Decrement `angle`, reset `aTotal` | Continuous −`angle`° |
| 3 (A0) | Rotate −`angle`° (but adds to `aTotal`) | Increment `angle`, reset `aTotal` | Continuous +`angle`° |
| 4 (A3) | Rotate −`angle`° | Decrement `angle`, reset `aTotal` | Continuous −`angle`° |

Note: Button 3's click rotates *backward* but increments `aTotal` forward — this appears to be a bug.

**Display** (updated every loop iteration, ~10 ms):
- Row 0: `Step Angle : <angle>`
- Row 1: `Distance   : <aTotal × 0.9/360>` (mm)

**`ardprintf()`** is a custom serial printf that supports `%d`, `%l`, `%f`, `%c`, `%s`. It's also (mis)used to format strings into `buf` for LCD output — but it only prints to `Serial`, not into the buffer. The LCD `distance` line relies on this side effect.

## Switching Motor Drivers

Commented-out sections at the top of the sketch show how to swap to A4988, DRV8834, or DRV8880 drivers — just uncomment the appropriate block and comment out the DRV8825 block.
