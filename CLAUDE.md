# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LatheStepper is an Arduino sketch that controls a stepper motor for lathe tool positioning. It runs on a **Raspberry Pi Pico (RP2040)** and uses a DRV8825 driver with 32x microstepping, a 20x4 I2C LCD display, a rotary encoder for speed, and three push buttons.

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

A clean build looks like:
```
Sketch uses 330160 bytes (15%) of program storage space.
Global variables use 71388 bytes (27%) of dynamic memory
```

```bash
# Install the rp2040 core (once)
arduino-cli core install rp2040:rp2040

# Compile
arduino-cli compile --fqbn rp2040:rp2040:rpipico LatheStepper/

# Upload (Pico appears as a USB serial port on Mac — adjust port as needed)
arduino-cli upload -p /dev/cu.usbmodem* --fqbn rp2040:rp2040:rpipico LatheStepper/

# Monitor serial output (9600 baud)
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=9600
```

Libraries must be installed in `~/Documents/Arduino/libraries/`:
- `StepperDriver` — motor control
- `OneButton` — multi-state button events
- `LiquidCrystal_I2C` — I2C LCD driver

## Hardware Configuration

All logic runs at 3.3V — fully compatible with DRV8825 logic inputs. GP0–GP12 all sit on the left-hand edge of the Pico, keeping wiring to one side.

| Pin | Function |
|-----|----------|
| GP0 | LCD SDA (I2C0) |
| GP1 | LCD SCL (I2C0) |
| GP2 | ENABLE (active LOW) |
| GP3 | DIR |
| GP4 | STEP |
| GP5–GP7 | MODE0/MODE1/MODE2 (microstep select) |
| GP8 | Encoder CLK (interrupt) |
| GP9 | Encoder DT |
| GP10 | Forward button |
| GP11 | Reverse button |
| GP12 | Start/Stop button (separate box near motor) |

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
