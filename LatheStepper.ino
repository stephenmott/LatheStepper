/*
  LatheStepper v2  —  Positional lathe carriage controller
  Sieg C0 Z-axis lead screw, Raspberry Pi Pico W + TMC2100

  Board:  Raspberry Pi Pico W — arduino-pico core v4.4.0 (NOT Arduino Mbed OS RP2040).
          ⚠ Do not upgrade core past v4.4.0 — v5.x has a known Pico W linker bug.
  Driver: TMC2100, CFG pins hardwired on module (not driven by Pico).
          Update MICROSTEPS below to match CFG1/CFG2 setting if known.

  Pins:
    GP0/GP1   LCD SDA/SCL (I2C0)
    GP2       ENABLE (active LOW)
    GP3/GP4   DIR / STEP
    GP8/GP9   Speed encoder CLK/DT  (enc1 — adjusts cut/rapid RPM)
    GP10/11   Forward / Reverse buttons  (INPUT_PULLUP)
    GP12      Start/Stop — remote box   (INPUT_PULLUP)
    GP13/14   Jog encoder CLK/DT        (enc2 — jogs carriage during setup)
    GP15      Jog encoder SW            (press to set home, then limit)

  Session setup (required after each power-on):
    1. Startup screen — enc1 adjusts cut RPM, enc2 adjusts rapid RPM.
       Press Start/Stop to confirm and proceed.
    2. Homing — jog carriage right to your start position with enc2.
       Press enc2 SW to zero position (home = 0.0 mm).
    3. Limit — jog left near the chuck with enc2.
       Press enc2 SW to store as the left limit.
    Limit position and speeds are saved to flash and reload next session.

  Cutting cycle (once home + limit are set):
    Start/Stop  →  cuts left to limit at cut speed
                →  rapid returns right to home
                →  stops, waiting for next pass
    Start/Stop again  →  next pass
    Start/Stop mid-move  →  emergency stop
    After e-stop:  Fwd = resume cut,  Rev = return home,  Start = resume cut

  If carriage moves the wrong way during jog, flip DIRECTION_SIGN to -1.
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <TMC2100.h>
#include <EEPROM.h>

// ── Pins ──────────────────────────────────────────────────────────────────────
#define PIN_LCD_SDA    0
#define PIN_LCD_SCL    1
#define PIN_ENABLE     2
#define PIN_DIR        3
#define PIN_STEP       4
// GP5–GP7 free
#define PIN_ENC1_CLK   8    // speed encoder
#define PIN_ENC1_DT    9
#define PIN_BTN_FWD   10    // resume cut (also: confirm startup)
#define PIN_BTN_REV   11    // return home after e-stop
#define PIN_BTN_SS    12    // Start/Stop — remote box
#define PIN_ENC2_CLK  13    // jog encoder
#define PIN_ENC2_DT   14
#define PIN_ENC2_SW   15    // set home (first press), then limit (second press)

// ── Motor ─────────────────────────────────────────────────────────────────────
#define MOTOR_STEPS    400     // 0.9°/step NEMA 17
#define MICROSTEPS      16     // update to match TMC2100 CFG1/CFG2 — verify empirically
#define LEADSCREW_MM   1.5f   // M8 leadscrew: 1.5 mm per revolution
#define DIRECTION_SIGN   1     // flip to -1 if carriage moves wrong way

// ── Speed ─────────────────────────────────────────────────────────────────────
#define RPM_MIN        10
#define RPM_MAX       400
#define RPM_STEP       10
#define RPM_CUT_DEF    80     // default cutting speed
#define RPM_RAPID_DEF 400     // default rapid return speed
#define RPM_JOG        40     // fixed jog speed during homing/setup

// ── EEPROM ────────────────────────────────────────────────────────────────────
#define EEPROM_SIZE    64
// Increment magic when Settings struct layout changes — forces defaults on next boot.
#define SETTINGS_MAGIC 0xC0LA0002UL

struct Settings {
  uint32_t magic;
  int      cutRPM;
  int      rapidRPM;
  long     limitSteps;
  bool     limitValid;
};

// ── Derived constants ─────────────────────────────────────────────────────────
// steps per mm = (motor full steps × microsteps) / leadscrew pitch
const float STEPS_PER_MM = (float)(MOTOR_STEPS * MICROSTEPS) / LEADSCREW_MM;

// ── Objects ───────────────────────────────────────────────────────────────────
TMC2100           stepper(MOTOR_STEPS, PIN_DIR, PIN_STEP, PIN_ENABLE);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ── State machine ─────────────────────────────────────────────────────────────
enum State : uint8_t {
  ST_STARTUP,    // show/adjust stored settings, confirm to proceed
  ST_HOMING,     // jog to home position, press enc2 SW to zero
  ST_SETUP,      // jog to left limit, press enc2 SW to store
  ST_READY,      // home + limit set, waiting for Start
  ST_CUTTING,    // running toward limit at cut speed
  ST_RETURNING,  // rapid return to home
  ST_STOPPED     // emergency stop mid-move
};
State state = ST_STARTUP;

// ── Volatile (written by ISRs, read by main loop) ─────────────────────────────
volatile int    enc1Ticks    = 0;   // speed encoder accumulator
volatile int    enc2Ticks    = 0;   // jog encoder accumulator
volatile long   currentSteps = 0;   // absolute position from home, counted by stepISR
volatile int8_t stepDir      = 1;   // +1 = toward limit, -1 = toward home; set before moves

// ── Settings (persisted to flash) ────────────────────────────────────────────
int  cutRPM     = RPM_CUT_DEF;
int  rapidRPM   = RPM_RAPID_DEF;
long limitSteps = 0;
bool limitSet   = false;

// ── Runtime flags ─────────────────────────────────────────────────────────────
bool motionActive = false;
bool displayDirty = true;

// ── Button debounce ───────────────────────────────────────────────────────────
// Struct declared before any function — prevents Arduino IDE auto-prototype
// being inserted before the type is defined (Philhower core quirk).
struct DebouncedBtn { uint8_t pin; bool last; unsigned long changed; };
DebouncedBtn btnFwd  = { PIN_BTN_FWD,  HIGH, 0 };
DebouncedBtn btnRev  = { PIN_BTN_REV,  HIGH, 0 };
DebouncedBtn btnSS   = { PIN_BTN_SS,   HIGH, 0 };
DebouncedBtn btnEnc2 = { PIN_ENC2_SW,  HIGH, 0 };

// ── ISRs ──────────────────────────────────────────────────────────────────────
void enc1ISR() {
  enc1Ticks += (digitalRead(PIN_ENC1_DT) != digitalRead(PIN_ENC1_CLK)) ? 1 : -1;
}

void enc2ISR() {
  enc2Ticks += (digitalRead(PIN_ENC2_DT) != digitalRead(PIN_ENC2_CLK)) ? 1 : -1;
}

// Attached to PIN_STEP (an output pin — valid on RP2040).
// Fires on every step pulse so currentSteps stays accurate after an e-stop.
void stepISR() {
  currentSteps += stepDir;
}

// ── Button debounce ───────────────────────────────────────────────────────────
bool pressed(DebouncedBtn &b) {
  bool reading = digitalRead(b.pin);
  if (reading != b.last && millis() - b.changed > 50) {
    b.changed = millis();
    b.last    = reading;
    return reading == LOW;
  }
  return false;
}

// ── Unit helpers ──────────────────────────────────────────────────────────────
float stepsToMm(long steps) { return (float)steps / STEPS_PER_MM; }

// ── Display helper ────────────────────────────────────────────────────────────
// Printf-style, always pads or truncates to exactly 20 chars (one LCD row).
void printRow(uint8_t row, const char *fmt, ...) {
  char buf[21];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (int i = strlen(buf); i < 20; i++) buf[i] = ' ';
  buf[20] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ── EEPROM ────────────────────────────────────────────────────────────────────
void loadSettings() {
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    cutRPM     = constrain(s.cutRPM,   RPM_MIN, RPM_MAX);
    rapidRPM   = constrain(s.rapidRPM, RPM_MIN, RPM_MAX);
    limitSteps = s.limitSteps;
    limitSet   = s.limitValid;
  }
}

void saveSettings() {
  Settings s = { SETTINGS_MAGIC, cutRPM, rapidRPM, limitSteps, limitSet };
  EEPROM.put(0, s);
  EEPROM.commit();
}

// ── Motor control ─────────────────────────────────────────────────────────────

// Begin a move from currentSteps to targetSteps at rpm.
// stepDir is set so the stepISR counts in the correct logical direction.
// DIRECTION_SIGN inverts the motor if carriage mechanics require it.
void startMove(long targetSteps, int rpm) {
  long delta = targetSteps - currentSteps;
  if (delta == 0) return;
  stepDir = (delta > 0) ? 1 : -1;
  float degrees = (float)delta / (MOTOR_STEPS * MICROSTEPS) * 360.0f * DIRECTION_SIGN;
  stepper.setRPM(rpm);
  stepper.enable();
  stepper.startRotate(degrees);
  motionActive = true;
}

void stopMotor() {
  stepper.stop();
  stepper.disable();
  motionActive = false;
}

// Move enc2 ticks × 0.1 mm at jog speed.  No-op if motor already moving.
void startJog(int ticks) {
  if (ticks == 0 || motionActive) return;
  long delta = (long)(ticks * 0.1f * STEPS_PER_MM);
  if (delta == 0) return;
  stepDir = (delta > 0) ? 1 : -1;
  float degrees = (float)delta / (MOTOR_STEPS * MICROSTEPS) * 360.0f * DIRECTION_SIGN;
  stepper.setRPM(RPM_JOG);
  stepper.enable();
  stepper.startRotate(degrees);
  motionActive = true;
}

// ── Display ───────────────────────────────────────────────────────────────────
void updateDisplay() {
  float pos = stepsToMm(currentSteps);
  float lim = stepsToMm(limitSteps);

  switch (state) {

    case ST_STARTUP:
      printRow(0, "  LATHE STEPPER v2  ");
      printRow(1, "Cut speed: %3d RPM", cutRPM);
      printRow(2, "Rapid ret: %3d RPM", rapidRPM);
      printRow(3, "Enc1=cut Enc2=rapid");
      break;

    case ST_HOMING:
      printRow(0, "Pos: %6.1f mm", pos);
      printRow(1, "--- SET HOME -------");
      printRow(2, "Jog enc2 to start");
      printRow(3, "Enc2 SW = Set Home");
      break;

    case ST_SETUP:
      printRow(0, "Pos: %6.1f mm", pos);
      printRow(1, "--- SET LIMIT ------");
      printRow(2, "Jog enc2 near chuck");
      printRow(3, "Enc2 SW = Set Limit");
      break;

    case ST_READY:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "Cut:%3dRPM Rp:%3dRPM", cutRPM, rapidRPM);
      printRow(2, "READY");
      printRow(3, limitSet ? "Start/Stop = Cut" : "! Set limit first");
      break;

    case ST_CUTTING:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "Cut: %3d RPM", cutRPM);
      printRow(2, ">> CUTTING");
      printRow(3, "Start/Stop = E-Stop");
      break;

    case ST_RETURNING:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "Rapid: %3d RPM", rapidRPM);
      printRow(2, "<< RETURNING");
      printRow(3, "Start/Stop = E-Stop");
      break;

    case ST_STOPPED:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "*** E-STOP ***");
      printRow(2, "Fwd=cont  Rev=home");
      printRow(3, "Start/Stop = Resume");
      break;
  }

  displayDirty = false;
}

// ── setup() ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  // Pico I2C0 defaults to GP4/GP5 — override to our wiring (GP0/GP1).
  Wire.setSDA(PIN_LCD_SDA);
  Wire.setSCL(PIN_LCD_SCL);
  lcd.init();
  lcd.backlight();

  stepper.begin(cutRPM, MICROSTEPS);
  stepper.setEnableActiveState(LOW);
  stepper.disable();

  pinMode(PIN_BTN_FWD,  INPUT_PULLUP);
  pinMode(PIN_BTN_REV,  INPUT_PULLUP);
  pinMode(PIN_BTN_SS,   INPUT_PULLUP);
  pinMode(PIN_ENC1_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC1_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC2_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC2_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC2_SW,  INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_ENC1_CLK), enc1ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC2_CLK), enc2ISR, CHANGE);
  // PIN_STEP is driven as an output by the stepper library.
  // Attaching RISING interrupt to an output pin is valid on RP2040 —
  // fires on every step pulse to keep currentSteps accurate after e-stop.
  attachInterrupt(digitalPinToInterrupt(PIN_STEP), stepISR, RISING);

  EEPROM.begin(EEPROM_SIZE);
  loadSettings();

  updateDisplay();
  Serial.println("LatheStepper v2 ready.");
}

// ── loop() ────────────────────────────────────────────────────────────────────
void loop() {

  // ── Drive motor; detect move completion ─────────────────────────────────
  if (motionActive && stepper.nextAction() == 0) {
    motionActive = false;
    switch (state) {
      case ST_CUTTING:
        currentSteps = limitSteps;   // snap to exact target
        startMove(0, rapidRPM);      // auto-start rapid return
        state = ST_RETURNING;
        break;
      case ST_RETURNING:
        currentSteps = 0;            // snap to exact home
        stepper.disable();
        state = ST_READY;
        break;
      default:                       // jog move complete (HOMING / SETUP)
        stepper.disable();
        break;
    }
    displayDirty = true;
  }

  // ── Read inputs (atomic snapshot of volatile counters) ──────────────────
  int e1, e2;
  noInterrupts();
  e1 = enc1Ticks; enc1Ticks = 0;
  e2 = enc2Ticks; enc2Ticks = 0;
  interrupts();

  bool fwdPressed  = pressed(btnFwd);
  bool revPressed  = pressed(btnRev);
  bool ssPressed   = pressed(btnSS);
  bool enc2Pressed = pressed(btnEnc2);

  // ── State machine ────────────────────────────────────────────────────────
  switch (state) {

    case ST_STARTUP:
      // Enc1 = cut RPM,  Enc2 = rapid RPM
      if (e1) {
        cutRPM   = constrain(cutRPM   + e1 * RPM_STEP, RPM_MIN, RPM_MAX);
        displayDirty = true;
      }
      if (e2) {
        rapidRPM = constrain(rapidRPM + e2 * RPM_STEP, RPM_MIN, RPM_MAX);
        displayDirty = true;
      }
      // Start/Stop or Forward confirms settings and moves to homing
      if (ssPressed || fwdPressed) {
        saveSettings();
        state = ST_HOMING;
        displayDirty = true;
      }
      break;

    case ST_HOMING:
      if (e2) { startJog(e2); displayDirty = true; }
      if (enc2Pressed && !motionActive) {
        currentSteps = 0;     // this position is now home
        limitSet     = false; // new home invalidates any stored limit
        state        = ST_SETUP;
        displayDirty = true;
      }
      break;

    case ST_SETUP:
      if (e2) { startJog(e2); displayDirty = true; }
      if (enc2Pressed && !motionActive) {
        limitSteps = currentSteps;
        limitSet   = true;
        saveSettings();
        state        = ST_READY;
        displayDirty = true;
      }
      break;

    case ST_READY:
      // Enc1 adjusts cut speed while waiting
      if (e1) {
        cutRPM = constrain(cutRPM + e1 * RPM_STEP, RPM_MIN, RPM_MAX);
        saveSettings();
        displayDirty = true;
      }
      if (ssPressed && limitSet) {
        startMove(limitSteps, cutRPM);
        state        = ST_CUTTING;
        displayDirty = true;
      }
      break;

    case ST_CUTTING:
      // Allow speed tweak mid-cut
      if (e1) {
        cutRPM = constrain(cutRPM + e1 * RPM_STEP, RPM_MIN, RPM_MAX);
        stepper.setRPM(cutRPM);
        saveSettings();
        displayDirty = true;
      }
      if (ssPressed) {
        stopMotor();
        state        = ST_STOPPED;
        displayDirty = true;
      }
      break;

    case ST_RETURNING:
      if (ssPressed) {
        stopMotor();
        state        = ST_STOPPED;
        displayDirty = true;
      }
      break;

    case ST_STOPPED:
      // Forward or Start/Stop = continue cut from current position
      if (ssPressed || fwdPressed) {
        startMove(limitSteps, cutRPM);
        state        = ST_CUTTING;
        displayDirty = true;
      }
      // Reverse = return to home
      if (revPressed) {
        startMove(0, rapidRPM);
        state        = ST_RETURNING;
        displayDirty = true;
      }
      break;
  }

  // ── Refresh display ──────────────────────────────────────────────────────
  // Redraw on state/value change, or every 250 ms to update position readout
  // while moving.
  static unsigned long lastDisplay = 0;
  if (displayDirty || millis() - lastDisplay > 250) {
    updateDisplay();
    lastDisplay  = millis();
    displayDirty = false;
  }
}
