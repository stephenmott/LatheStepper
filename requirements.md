# Background

So I wrote this program ages ago to control the z axis on my mini lathe, its a tiny Sieg C0 lathe, so the axis is fixed to the lead screw so the only way to move it is to spin the handle, which became a real pain in my wrist. It sort of worked, but I've ended up never really using it, because it was just too unreliable and didn't work as you would naturally use the lathe, plus I mounted the controller on top of the lathe so you have to reach over the workpiece to move the carriage which is never a good idea. I need to disassemble it all and bring it down to re-programme, so it's just sat there for years unused. So if I'm going to go the pain of removing it and re-programming, I think I'm just going to re-do the whole thing apart from the Nema 17 mount.

## Requirements

**Display**

Keep the display mounted on top of the lathe — it's the only place visible from the operating position. Must clearly show direction and speed (not step angle).

**Controls**

- **Speed encoder (enc1)** — adjusts cut RPM or rapid RPM in 10 RPM steps
- **Jog encoder (enc2)** — turns to jog carriage during setup; push button sets home then limit
- **Forward button** — resume cut after emergency stop
- **Reverse button** — return to home after emergency stop
- **Start/Stop button** — start cycle / emergency stop; lives in a separate box near the motor/tailstock end

**Cutting cycle**

1. Start/Stop → carriage cuts left to stored limit at cut speed
2. Auto rapid-returns right to home
3. Stops and waits
4. User advances cross-slide, presses Start/Stop → next pass

**Enclosures**

- **Top box** (mounted above lathe, visible): LCD, speed encoder (enc1), jog encoder with push button (enc2), Forward button, Reverse button
- **Motor box** (near motor/tailstock end): Start/Stop button only

---

## Hardware / Pin Mapping

**Board: Raspberry Pi Pico W (RP2040)**
**Driver: TMC2100**

All signals use 3.3V logic. All GP pins support interrupts. The TMC2100 CFG pins are hardwired on the module — not connected to the Pico — so only EN/DIR/STEP are needed.

The Pico is mounted with **USB pointing up and off the board edge** so it can be plugged in. This puts GP13–GP15 at the bottom of the left edge, physically close to the TMC2100, keeping motor signal traces short.

| Pin | Function | Notes |
|-----|----------|-------|
| GP0 | LCD SDA | I2C0 hardware |
| GP1 | LCD SCL | I2C0 hardware |
| GP2 | Forward button | INPUT_PULLUP, LOW = pressed |
| GP3 | Reverse button | INPUT_PULLUP, LOW = pressed |
| GP4 | Start/Stop button | INPUT_PULLUP, LOW = pressed — remote box |
| GP5 | Jog encoder CLK | Interrupt |
| GP6 | Jog encoder DT | |
| GP7 | Jog encoder SW | INPUT_PULLUP — press to set home, then limit |
| GP8 | Speed encoder CLK | Interrupt |
| GP9 | Speed encoder DT | |
| GP10–GP12 | (free) | |
| GP13 | ENABLE | Active LOW — bottom of left edge, close to TMC2100 |
| GP14 | DIR | TMC2100 direction |
| GP15 | STEP | TMC2100 step |

GP0–GP15 all sit on the left-hand edge of the Pico.

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
  USB ↑ (off board edge)
                      Raspberry Pi Pico W
                    ┌──────────────────────┐
              GP0 ──┤ LCD SDA        VBUS  ├── (USB 5V)
              GP1 ──┤ LCD SCL        VSYS  ├── 5V power in
              GND ──┤ GND             GND  ├──
              GP2 ──┤ Forward btn     3V3  ├── 3.3V out → encoder +
              GP3 ──┤ Reverse btn  3V3_EN  ├──
              GP4 ──┤ Start/Stop btn GP28  ├── (free)
              GP5 ──┤ ENC2 CLK       GP27  ├── (free)
              GND ──┤ GND            GP26  ├── (free)
              GP6 ──┤ ENC2 DT         RUN  ├──
              GP7 ──┤ ENC2 SW        GP22  ├── (free)
              GP8 ──┤ ENC1 CLK        GND  ├──
              GP9 ──┤ ENC1 DT        GP21  ├── (free)
              GND ──┤ GND            GP20  ├── (free)
             GP10 ──┤ (free)         GP19  ├── (free)
             GP11 ──┤ (free)         GP18  ├── (free)
             GP12 ──┤ (free)          GND  ├──
             GP13 ──┤ ENABLE         GP17  ├── (free)
              GND ──┤ GND            GP16  ├── (free)
             GP14 ──┤ DIR            GP15  ├── STEP
                    └──────────────────────┘
  TMC2100 ↓ (short jumpers to GP13/14/15)

TMC2100:  ENABLE←GP13, DIR←GP14, STEP←GP15
          CFG1/CFG2/CFG3 hardwired on module (see CFG table above)
Buttons:  one leg to pin, other leg to GND  (INPUT_PULLUP, no resistor needed)
Enc1:     CLK→GP8,  DT→GP9,  GND→GND, +→3V3              (speed)
Enc2:     CLK→GP5,  DT→GP6,  SW→GP7,  GND→GND, +→3V3     (jog + set home/limit)
LCD:      SDA→GP0,  SCL→GP1, GND→GND, VCC→5V (VSYS)
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
