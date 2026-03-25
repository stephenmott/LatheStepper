# Background

So I wrote this program ages ago to control the z axis on my mini lathe, its a tiny Sieg C0 lathe, so the axis is fixed to the lead screw so the only way to move it is to spin the handle, which became a real pain in my wrist. It sort of worked, but I've ended up never really using it, because it was just too unreliable and didn't work as you would naturally use the lathe, plus I mounted the controller on top of the lathe so you have to reach over the workpiece to move the carriage which is never a good idea. I need to disassemble it all and bring it down to re-programme, so it's just sat there for years unused. So if I'm going to go the pain of removing it and re-programming, I think I'm just going to re-do the whole thing apart from the Nema 17 mount.

## Requirements

**Display**

Keep the display mounted on top of the lathe — it's the only place visible from the operating position. Must clearly show direction and speed (not step angle).

**Controls**

- Forward button — sets direction forward and starts motor
- Reverse button — sets direction reverse and starts motor
- Start/Stop button — toggles motor on/off; lives in a separate box near the motor/tailstock end
- Rotary encoder — adjusts speed in 10 RPM steps
- Fourth button removed

**Enclosures**

- **Top box** (mounted above lathe, visible): LCD, rotary encoder, Forward button, Reverse button
- **Motor box** (near motor/tailstock end): Start/Stop button only

---

## Hardware / Pin Mapping

**Board: Raspberry Pi Pico W (RP2040)**
**Driver: TMC2100**

All signals use 3.3V logic. All GP pins support interrupts. The TMC2100 CFG pins are hardwired on the module — not connected to the Pico — so only EN/DIR/STEP are needed, freeing GP5–GP7.

| Pin | Function | Notes |
|-----|----------|-------|
| GP0 | LCD SDA | I2C0 hardware |
| GP1 | LCD SCL | I2C0 hardware |
| GP2 | ENABLE | Active LOW |
| GP3 | DIR | TMC2100 direction |
| GP4 | STEP | TMC2100 step |
| GP5–GP7 | (free) | Previously MODE0–2 for DRV8825 |
| GP8 | Encoder CLK | Interrupt |
| GP9 | Encoder DT | |
| GP10 | Forward button | INPUT_PULLUP, LOW = pressed |
| GP11 | Reverse button | INPUT_PULLUP, LOW = pressed |
| GP12 | Start/Stop button | INPUT_PULLUP, LOW = pressed — remote box |

GP0–GP12 all sit on the left-hand side of the Pico, keeping wiring to one edge.

**TMC2100 CFG pin wiring (on driver module, not Pico):**

| CFG1 | CFG2 | Microsteps | Mode |
|------|------|------------|------|
| GND  | GND  | 256        | SpreadCycle |
| VCC  | GND  | 128        | SpreadCycle |
| GND  | VCC  | 64         | SpreadCycle |
| VCC  | VCC  | **16**     | SpreadCycle — **recommended default** |
| OPEN | GND  | 16 → 256   | StealthChop + MicroPlyer |

CFG3: tie to GND for StealthChop (quiet), VCC for SpreadCycle (more torque). For lathe carriage speeds StealthChop is fine.

Update `MICROSTEPS` in the sketch to match whatever CFG1/CFG2 are set to.

---

## Wiring Diagram

```
                      Raspberry Pi Pico
                    ┌──────────────────────┐
              GP0 ──┤ LCD SDA        VBUS  ├── (USB 5V, not used)
              GP1 ──┤ LCD SCL        VSYS  ├── 5V power in
              GND ──┤ GND             GND  ├──
              GP2 ──┤ ENABLE          3V3  ├── 3.3V out → encoder +
              GP3 ──┤ DIR          3V3_EN  ├──
              GP4 ──┤ STEP           GP28  ├── (free)
              GP5 ──┤ (free)         GP27  ├── (free)
              GND ──┤ GND            GP26  ├── (free)
              GP6 ──┤ (free)          RUN  ├──
              GP7 ──┤ (free)         GP22  ├── (free)
              GP8 ──┤ ENC_CLK         GND  ├──
              GP9 ──┤ ENC_DT         GP21  ├── (free)
              GND ──┤ GND            GP20  ├── (free)
             GP10 ──┤ Forward btn    GP19  ├── (free)
             GP11 ──┤ Reverse btn    GP18  ├── (free)
             GP12 ──┤ Start/Stop btn  GND  ├──
             GP13 ──┤ (free)         GP17  ├── (free)
              GND ──┤ GND            GP16  ├── (free)
             GP14 ──┤ (free)         GP15  ├── (free)
                    └──────────────────────┘

TMC2100:  ENABLE←GP2, DIR←GP3, STEP←GP4
          CFG1/CFG2/CFG3 hardwired on module (see CFG table above)
Buttons:  one leg to pin, other leg to GND  (INPUT_PULLUP, no resistor needed)
Encoder:  CLK→GP8, DT→GP9, GND→GND, +→3V3
LCD:      SDA→GP0, SCL→GP1, GND→GND, VCC→5V (VSYS)
```

---

## EMI — Remote Start/Stop Button

The motor box is a noisy environment. The TMC2100 switches at high frequency and the motor wires carry fast current spikes. For the cable run from the remote box back to the Arduino:

**Do:**
- Use **twisted pair** cable (cheap alarm cable works fine) — one wire is the signal, one is the dedicated GND return. Twisting cancels induced noise.
- Route it **away from motor and stepper wires** — cross at right angles if paths cross; never run parallel.
- Add a **100 nF ceramic capacitor** across the button terminals at the Arduino end — this hardware-filters spikes that get through. Simple cap from the pin to GND, alongside the button.
- Optionally add a **1 kΩ series resistor** in the signal line to limit any induced current into the input pin.

**Don't:**
- Don't share the GND return with a motor wire — dedicated GND wire back to Arduino only.
- Shielded cable isn't necessary for a simple momentary button at distances under ~2 m; twisted pair is sufficient.

**Already handled in software:**
- 50 ms debounce on `pressed()` rejects any glitch shorter than 50 ms, which covers most interference.
