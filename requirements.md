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
| GP0 | Forward button | INPUT_PULLUP, LOW = pressed |
| GP1 | Reverse button | INPUT_PULLUP, LOW = pressed |
| GP2 | Start/Stop button | INPUT_PULLUP, LOW = pressed — remote box (twisted pair) |
| GP3 | DIR | TMC2100 direction — hard-wired |
| GP4 | Enc2 VCC | OUTPUT HIGH — supplies 3.3 V to jog encoder (~1 mA) |
| GP5 | Jog encoder CLK | Interrupt |
| GP6 | Jog encoder DT | |
| GP7 | Jog encoder SW | INPUT_PULLUP — press to set home, then limit |
| GP8 | (free) | Gap between ENC2 and ENC1 plugs |
| GP9 | Speed encoder CLK | Interrupt |
| GP10 | Speed encoder DT | |
| GP11 | Enc1 VCC | OUTPUT HIGH — supplies 3.3 V to speed encoder (~1 mA) |
| GP12 | STEP | TMC2100 step — hard-wired |
| GP13 | ENABLE | Active LOW — hard-wired to TMC2100 |
| GP14 | LCD SDA | I2C1 hardware (Wire1) — as physically wired |
| GP15 | LCD SCL | I2C1 hardware (Wire1) — as physically wired |

GP0–GP15 all sit on the left-hand edge of the Pico.

**Why GP14/GP15 for LCD use Wire1, not Wire?** The RP2040 has two I2C hardware blocks. Each is fixed to specific pins in a repeating pattern: I2C0 covers GP0/1, GP4/5, GP8/9, GP12/13; I2C1 covers GP2/3, GP6/7, GP10/11, GP14/15. The `LiquidCrystal_I2C` library has been patched to accept an optional `TwoWire&` parameter (defaulting to `Wire`) so passing `Wire1` routes it to I2C1.

**Why GPIO for encoder VCC?** The left edge of the Pico has no VCC pins — only GNDs at physical positions 3, 8, 13, 17. Using a GPIO pin set OUTPUT HIGH avoids routing a wire to the right-edge 3V3 pin. A rotary encoder draws ~1 mA (just the internal pull-up resistors), well within the 12 mA GPIO source limit.

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
              GP0 ──┤ Forward btn    VBUS  ├── (USB 5V)
              GP1 ──┤ Reverse btn    VSYS  ├── 5V power in → LCD VCC
              GND ──┤ GND             GND  ├──
              GP2 ──┤ Start/Stop btn 3V3_EN├──
              GP3 ──┤ DIR             3V3  ├── (free)
              GP4 ──┤ ENC2 VCC*  ADC_VREF  ├──
              GP5 ──┤ ENC2 CLK       GP28  ├── (free)
              GND ──┤ GND             GND  ├──
              GP6 ──┤ ENC2 DT        GP27  ├── (free)
              GP7 ──┤ ENC2 SW        GP26  ├── (free)
              GP8 ──┤ (free)          RUN  ├──
              GP9 ──┤ ENC1 CLK       GP22  ├── (free)
              GND ──┤ GND             GND  ├──
             GP10 ──┤ ENC1 DT        GP21  ├── (free)
             GP11 ──┤ ENC1 VCC*      GP20  ├── (free)
             GP12 ──┤ STEP           GP19  ├── (free)
             GP13 ──┤ ENABLE         GP18  ├── (free)
              GND ──┤ GND             GND  ├──
             GP14 ──┤ LCD SDA        GP17  ├── (free)
             GP15 ──┤ LCD SCL        GP16  ├── (free)
                    └──────────────────────┘

* GP4 and GP10 are set OUTPUT HIGH in firmware — they supply ~3.3 V to encoder VCC.
  Encoders draw ~1 mA each, within the 12 mA GPIO source limit.

TMC2100:  ENABLE←GP13, DIR←GP3, STEP←GP12
          CFG1/CFG2/CFG3 hardwired on module (see CFG table above)
Buttons:  one leg to pin, other leg to GND  (INPUT_PULLUP, no resistor needed)
Enc1:     CLK→GP9,  DT→GP10, GND→GND, VCC→GP11*           (speed)
Enc2:     CLK→GP5,  DT→GP6,  SW→GP7,  GND→GND, VCC→GP4*  (jog + set home/limit)
LCD:      SDA→GP14, SCL→GP15, GND→GND, VCC→VSYS (5V, right edge)  [uses Wire1 / I2C1]
```

### JST connector groups (protoboard, left edge of Pico)

JST connectors soldered to the protoboard next to the Pico. The Pico left edge has GND pins at physical positions 3, 8, 13, 17 — the groups below exploit these.

| Connector | Pins (physical order) | Wires |
|-----------|----------------------|-------|
| **LCD** (4-pin) | GND · SDA · SCL · VCC | GND=pin 17; SDA=GP14; SCL=GP15; VCC from VSYS via protoboard trace |
| **Buttons** (4-pin) | FWD · REV · GND · SS | FWD=GP0; REV=GP1; GND=pin 3; SS=GP2 |
| **ENC2** (5-pin) | VCC · CLK · GND · DT · SW | VCC=GP4; CLK=GP5; GND=pin 8; DT=GP6; SW=GP7 |
| **ENC1** (4-pin) | CLK · GND · DT · VCC | CLK=GP9; GND=pin 13; DT=GP10; VCC=GP11 |
| **Motor** (3 jumpers) | DIR · STEP · ENABLE | GP3 · GP12 · GP13 → TMC2100 |

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
