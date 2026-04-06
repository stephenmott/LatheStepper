/*
  LatheStepper v2  —  Positional lathe carriage controller
  Sieg C0 Z-axis lead screw, Raspberry Pi Pico W + DRV8825

  Board:  Raspberry Pi Pico W — arduino-pico core (Earle Philhower, NOT Arduino Mbed OS RP2040).
  Driver: DRV8825, CFG pins hardwired on module (not driven by Pico).
          Update MICROSTEPS below to match CFG1/CFG2 setting if known.

  Pins (Pico mounted USB-up; GP0 = top of left edge, GP15 = bottom):
    GP0       Forward button  (INPUT_PULLUP)              ┐ 4-pin JST: FWD,REV,GND,SS
    GP1       Reverse button  (INPUT_PULLUP)              │   (GND pin between GP1 and GP2)
    GND
    GP2       Start/Stop — remote box (INPUT_PULLUP)     ┘
    GP3       DIR  → DRV8825 (hard-wired)
    GP4       ENC2 VCC (OUTPUT HIGH — encoder supply)    ┐
    GP5       Jog enc CLK     (interrupt)                 │ 5-pin JST: VCC,CLK,GND,DT,SW
    GND
    GP6       Jog enc DT                                  │   (GND between GP5 and GP6)
    GP7       Jog enc SW      (INPUT_PULLUP)              ┘
    GP8       (free — gap between ENC2 and ENC1 plugs)
    GP9       Speed enc CLK   (interrupt)                 ┐
    GND
    GP10      Speed enc DT                                 │ 4-pin JST: CLK,GND,DT,VCC
    GP11      ENC1 VCC (OUTPUT HIGH — encoder supply)     ┘   (GND between GP9 and GP10)
    GP12      STEP → DRV8825 (hard-wired)
    GP12      (free)
    GP13      ENABLE → DRV8825 (active LOW, hard-wired)
    GND
    GP14      LCD SDA (I2C1 / Wire1) — as physically wired ┐ 4-pin JST: GND,SDA,SCL,VCC
    GP15      LCD SCL (I2C1 / Wire1)                        ┘

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
#include <DRV8825.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "secrets.h"     // WIFI_SSID, WIFI_PASS, OTA_PASS — not committed to git

// ── Pins ──────────────────────────────────────────────────────────────────────
#define PIN_BTN_FWD    0    // resume cut (also: confirm startup)      ┐ 4-pin JST:
#define PIN_BTN_REV    1    // return home after e-stop               │ FWD,REV,GND,SS
#define PIN_BTN_SS     2    // Start/Stop — remote box, twisted pair  ┘ (GND at pin 3)
#define PIN_DIR        3    // DRV8825 — hard-wired, can reach anywhere on board
#define PIN_ENC2_VCC   4    // OUTPUT HIGH — supplies encoder 2 VCC via 5-pin JST
#define PIN_ENC2_CLK   5    // jog encoder CLK (interrupt)
#define PIN_ENC2_DT    6    // jog encoder DT
#define PIN_ENC2_SW    7    // jog encoder SW: set home (first press), then limit (second press)
// GP8 free — gap between ENC2 and ENC1 plugs
#define PIN_ENC1_CLK   9    // speed encoder CLK (interrupt)
#define PIN_ENC1_DT   10    // speed encoder DT
#define PIN_ENC1_VCC  11    // OUTPUT HIGH — supplies encoder 1 VCC via 4-pin JST
#define PIN_STEP      12    // DRV8825 — hard-wired
#define PIN_ENABLE    13    // active LOW
#define PIN_LCD_SDA   14    // I2C1 — as physically wired
#define PIN_LCD_SCL   15    // I2C1

// ── Motor ─────────────────────────────────────────────────────────────────────
#define MOTOR_STEPS    400     // 0.9°/step NEMA 17
#define MICROSTEPS      16     // DRV8825 M2=HIGH, M0/M1 unconnected = 1/16 step
#define LEADSCREW_MM   1.0f   // Empirically measured: M8 fine pitch 1.0 mm/rev
                              // (commanded 9.9 mm, got 6.5 mm with 1.5 → corrected to 1.0)
#define DIRECTION_SIGN   1     // flip to -1 if carriage moves wrong way

// ── Speed ─────────────────────────────────────────────────────────────────────
#define RPM_MIN        10
#define RPM_MAX       200     // raise once motor/driver tuned
#define RPM_STEP       10
#define RPM_CUT_DEF    40     // default cutting speed — conservative starting point
#define RPM_RAPID_DEF  80     // default rapid return — raise once stable
#define RPM_JOG        20     // jog speed — always slow to prevent missed steps
// Distance per encoder tick scales with spin speed instead of RPM:
//   1 tick (slow/precise) = JOG_MM_FINE mm
//   2+ ticks (fast/traverse) = JOG_MM_COARSE mm per tick
#define JOG_MM_FINE   0.1f   // fine positioning — one careful tick
#define JOG_MM_COARSE 0.5f   // traversing — spinning fast
#define IDLE_TIMEOUT_MS   300000UL  // 5 minutes — stepper disabled, any key wakes
#define DOUBLE_CLICK_MS      350UL  // JOG SW double-click window
#define ACCEL         300     // steps/s² — conservative to prevent missed steps on jog

// ── EEPROM ────────────────────────────────────────────────────────────────────
#define EEPROM_SIZE    64
// Increment magic when Settings struct layout changes — forces defaults on next boot.
#define SETTINGS_MAGIC 0xCAFE0003UL

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
DRV8825           stepper(MOTOR_STEPS, PIN_DIR, PIN_STEP, PIN_ENABLE);
LiquidCrystal_I2C lcd(0x27, 20, 4, Wire1);  // GP14/GP15 = I2C1

// ── State machine ─────────────────────────────────────────────────────────────
enum State : uint8_t {
  ST_STARTUP,    // show/adjust stored settings, confirm to proceed
  ST_HOMING,     // jog to home position, press enc2 SW to zero
  ST_SETUP,      // jog to left limit, press enc2 SW to store
  ST_READY,      // home + limit set, waiting for Start
  ST_JOG,        // free jog mode — move carriage for loading/unloading
  ST_CUTTING,    // running toward limit at cut speed
  ST_RETURNING,  // rapid return to home
  ST_STOPPED     // emergency stop mid-move
};
State state = ST_STARTUP;

// ── Volatile (written by ISRs, read by main loop) ─────────────────────────────
volatile int    enc1Ticks     = 0;     // speed encoder accumulator
volatile int    enc2Ticks     = 0;     // jog encoder accumulator
volatile long   currentSteps  = 0;     // absolute position from home, counted by stepISR
volatile int8_t stepDir       = 1;     // +1 = toward limit, -1 = toward home; set before moves
volatile bool   motionComplete = false; // set by core 1 when nextAction() returns 0

// ── Settings (persisted to flash) ────────────────────────────────────────────
int  cutRPM     = RPM_CUT_DEF;
int  rapidRPM   = RPM_RAPID_DEF;
long limitSteps = 0;
bool limitSet   = false;

// ── Runtime flags ─────────────────────────────────────────────────────────────
volatile bool motionActive = false;
bool displayDirty       = true;
bool editRapid          = false;   // true = RPM knob edits rapid speed; false = cut speed
bool sleeping           = false;   // true = idle timeout fired, stepper disabled
bool stepperManualOff   = false;   // true = user manually disabled stepper via RIGHT in ST_READY
unsigned long lastActivityMs = 0;
int  pendingJogTicks    = 0;       // ticks buffered while motor is busy — fired on next free cycle

// ── Button debounce ───────────────────────────────────────────────────────────
// Struct declared before any function — prevents Arduino IDE auto-prototype
// being inserted before the type is defined (Philhower core quirk).
struct DebouncedBtn { uint8_t pin; bool last; unsigned long changed; };
DebouncedBtn btnFwd  = { PIN_BTN_FWD,  HIGH, 0 };
DebouncedBtn btnRev  = { PIN_BTN_REV,  HIGH, 0 };
DebouncedBtn btnSS   = { PIN_BTN_SS,   HIGH, 0 };
DebouncedBtn btnEnc2 = { PIN_ENC2_SW,  HIGH, 0 };

// ── ISRs ──────────────────────────────────────────────────────────────────────
// Full quadrature state-machine decoder.
// QEM[prev<<2 | curr]: +1 = one CW step, -1 = one CCW step, 0 = invalid/bounce.
// Four accumulated steps = one physical detent on standard EC11 (4 PPR) encoders.
// If it takes TWO turns to move one step, change the threshold from 4 to 2.
// Bounce produces invalid transitions → QEM returns 0 → automatically ignored.
// No time-based debounce needed.
static const int8_t QEM[16] = { 0,-1, 1, 0, 1, 0, 0,-1,-1, 0, 0, 1, 0, 1,-1, 0 };

void enc1ISR() {
  static uint8_t prev = 0b11;   // idle state with INPUT_PULLUP: both pins HIGH
  static int8_t  sub  = 0;
  uint8_t curr = (digitalRead(PIN_ENC1_CLK) << 1) | digitalRead(PIN_ENC1_DT);
  sub += QEM[prev << 2 | curr];
  prev = curr;
  if (sub >=  4) { enc1Ticks++; sub -= 4; }
  if (sub <= -4) { enc1Ticks--; sub += 4; }
}

void enc2ISR() {
  static uint8_t prev = 0b11;
  static int8_t  sub  = 0;
  uint8_t curr = (digitalRead(PIN_ENC2_CLK) << 1) | digitalRead(PIN_ENC2_DT);
  sub += QEM[prev << 2 | curr];
  prev = curr;
  if (sub >=  4) { enc2Ticks++; sub -= 4; }
  if (sub <= -4) { enc2Ticks--; sub += 4; }
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
  stepperManualOff = false;   // starting a move always re-enables the stepper
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

/// Move enc2 ticks at a constant safe RPM.
// Distance scales with spin speed — fast spin = more mm per tick, not faster motor.
// This avoids missed steps (which corrupt position) while still allowing quick traverse.
//   1 tick  → JOG_MM_FINE   (0.1mm) — precise single-click positioning
//   2+ ticks → JOG_MM_COARSE (0.5mm per tick) — fast traverse
// No-op if motor already moving.
void startJog(int ticks) {
  if (ticks == 0 || motionActive) return;
  float mmPerTick = (abs(ticks) == 1) ? JOG_MM_FINE : JOG_MM_COARSE;
  long delta = (long)(ticks * mmPerTick * STEPS_PER_MM);
  if (delta == 0) return;
  stepDir = (delta > 0) ? 1 : -1;
  float degrees = (float)delta / (MOTOR_STEPS * MICROSTEPS) * 360.0f * DIRECTION_SIGN;
  stepper.setRPM(RPM_JOG);
  stepper.enable();
  stepper.startRotate(degrees);
  motionActive = true;
}

// ── Display ───────────────────────────────────────────────────────────────────
// Row 3 soft-key bar: [Left  ][Centre  ][Right ]  (6+8+6 = 20 chars)
// Centre label is centred within its 8-char slot.
void printSoftKeys(const char* l, const char* c, const char* r) {
  char buf[21];
  memset(buf, ' ', 20);
  buf[20] = '\0';
  // Left-aligned in cols 0-5
  int ll = min((int)strlen(l), 6);
  memcpy(buf, l, ll);
  // Centred in cols 6-13
  int cl = min((int)strlen(c), 8);
  memcpy(buf + 6 + (8 - cl) / 2, c, cl);
  // Left-aligned in cols 14-19
  int rl = min((int)strlen(r), 6);
  memcpy(buf + 14, r, rl);
  lcd.setCursor(0, 3);
  lcd.print(buf);
}

void updateDisplay() {
  float pos = stepsToMm(currentSteps);
  float lim = stepsToMm(limitSteps);

  switch (state) {

    case ST_STARTUP:
      printRow(0, "  LATHE STEPPER v2  ");
      printRow(1, "RPM knob: cut %3dRPM", cutRPM);
      printRow(2, "JOG knob: rtn %3dRPM", rapidRPM);
      printSoftKeys("GO", "CONFIRM", "");
      break;

    case ST_HOMING:
      printRow(0, "Pos: %6.1f mm", pos);
      printRow(1, "--- SET HOME -------");
      printRow(2, "JOG=move  SW=SetHome");
      printSoftKeys("", "", "");
      break;

    case ST_SETUP:
      printRow(0, "Pos: %6.1f mm", pos);
      printRow(1, "--- SET LIMIT ------");
      printRow(2, "JOG=move   SW=SetLim");
      printSoftKeys("", "", "");
      break;

    case ST_READY:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "Cut %3dRPM Rtn%3dRPM", cutRPM, rapidRPM);
      printRow(2, "JogSW=Jog 2x=ReHome ");
      printSoftKeys(editRapid ? ">CUT" : ">RTN", "CUT",
                    stepperManualOff ? "ENABLE" : "DISABL");
      break;

    case ST_JOG:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "--- JOG MODE -------");
      printRow(2, "JOG=move  JOGSW=Done");
      printSoftKeys("", "CUT", "");
      break;

    case ST_CUTTING:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, ">> CUTTING  %3dRPM", cutRPM);
      printRow(2, "RPM knob adjusts spd");
      printSoftKeys("", "E-STOP", "");
      break;

    case ST_RETURNING:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "<< RETURNING %3dRPM", rapidRPM);
      printRow(2, "");
      printSoftKeys("", "E-STOP", "");
      break;

    case ST_STOPPED:
      printRow(0, "Pos:%5.1f /%5.1fmm", pos, lim);
      printRow(1, "*** E-STOP ***");
      printRow(2, "");
      printSoftKeys("RESUME", "", "RETURN");
      break;
  }

  displayDirty = false;
}

// ── setup() ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  // LCD is on GP14/GP15 = I2C1 (Wire1).
  Wire1.setSDA(PIN_LCD_SDA);
  Wire1.setSCL(PIN_LCD_SCL);
  lcd.init();
  lcd.backlight();

  stepper.begin(cutRPM, MICROSTEPS);
  stepper.setEnableActiveState(LOW);
  stepper.setSpeedProfile(stepper.LINEAR_SPEED, ACCEL, ACCEL);
  stepper.disable();

  // GPIO pins used as encoder VCC rails (encoders draw ~1 mA — well within 12 mA limit).
  pinMode(PIN_ENC2_VCC, OUTPUT); digitalWrite(PIN_ENC2_VCC, HIGH);
  pinMode(PIN_ENC1_VCC, OUTPUT); digitalWrite(PIN_ENC1_VCC, HIGH);

  pinMode(PIN_BTN_FWD,  INPUT_PULLUP);
  pinMode(PIN_BTN_REV,  INPUT_PULLUP);
  pinMode(PIN_BTN_SS,   INPUT_PULLUP);
  pinMode(PIN_ENC1_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC1_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC2_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC2_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC2_SW,  INPUT_PULLUP);

  // Attach CHANGE interrupts on both CLK and DT so the state machine
  // sees every quadrature edge — gives solid direction detection and
  // self-filtering of bounce without any time-based debounce.
  attachInterrupt(digitalPinToInterrupt(PIN_ENC1_CLK), enc1ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC1_DT),  enc1ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC2_CLK), enc2ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC2_DT),  enc2ISR, CHANGE);
  // PIN_STEP is driven as an output by the stepper library.
  // Attaching RISING interrupt to an output pin is valid on RP2040 —
  // fires on every step pulse to keep currentSteps accurate after e-stop.
  attachInterrupt(digitalPinToInterrupt(PIN_STEP), stepISR, RISING);

  EEPROM.begin(EEPROM_SIZE);
  loadSettings();

  // ── WiFi + OTA ────────────────────────────────────────────────────────────
  // Try to connect to WiFi for OTA uploads. Non-blocking: if the network
  // isn't available we give up after 10 s and continue normally — the lathe
  // still works without WiFi, just no OTA.
  printRow(0, "Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setPassword(OTA_PASS);
    // Stop the motor before flashing — carriage must not move during OTA.
    ArduinoOTA.onStart([]() { stopMotor(); });
    ArduinoOTA.begin();
    Serial.print("OTA ready, IP: ");
    Serial.println(WiFi.localIP());
    printRow(0, ("OTA: " + WiFi.localIP().toString()).c_str());
    delay(1500);   // brief pause so IP is readable on LCD
  } else {
    Serial.println("WiFi not available — OTA disabled.");
  }

  updateDisplay();
  Serial.println("LatheStepper v2 ready.");
}

// ── loop() ────────────────────────────────────────────────────────────────────
void loop() {

  // ── OTA ─────────────────────────────────────────────────────────────────
  if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();

  // ── Detect move completion (nextAction() runs on core 1) ────────────────
  if (motionComplete) {
    motionComplete = false;
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

  bool anyActivity = e1 || e2 || fwdPressed || revPressed || ssPressed || enc2Pressed;

  // ── Idle sleep / wake ────────────────────────────────────────────────────
  if (anyActivity) {
    lastActivityMs = millis();
    if (sleeping) {
      // First input just wakes — don't process it further this cycle
      sleeping = false;
      if (!stepperManualOff) stepper.enable();
      displayDirty = true;
      return;
    }
  }
  if (!motionActive && !sleeping &&
      millis() - lastActivityMs > IDLE_TIMEOUT_MS) {
    sleeping = true;
    stepper.disable();
    displayDirty = true;
  }

  // ── State machine ────────────────────────────────────────────────────────
  if (sleeping) {
    if (displayDirty) {
      lcd.clear();
      printRow(0, "    STEPPER OFF     ");
      printRow(1, "");
      printRow(2, "  Press any button  ");
      printRow(3, "     to wake up     ");
      displayDirty = false;
    }
    return;
  }

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
      pendingJogTicks += e2;
      if (!motionActive && pendingJogTicks) {
        startJog(pendingJogTicks); pendingJogTicks = 0; displayDirty = true;
      }
      if (enc2Pressed && !motionActive) {
        pendingJogTicks = 0;
        currentSteps = 0;     // this position is now home
        limitSet     = false; // new home invalidates any stored limit
        state        = ST_SETUP;
        displayDirty = true;
      }
      break;

    case ST_SETUP:
      pendingJogTicks += e2;
      if (!motionActive && pendingJogTicks) {
        startJog(pendingJogTicks); pendingJogTicks = 0; displayDirty = true;
      }
      if (enc2Pressed && !motionActive) {
        limitSteps = currentSteps;
        limitSet   = true;
        saveSettings();
        startMove(0, rapidRPM);  // rapid-return to home after setting limit
        state        = ST_RETURNING;
        displayDirty = true;
      }
      break;

    case ST_READY:
      // RPM knob edits cut or rapid speed depending on editRapid toggle
      if (e1) {
        if (editRapid) rapidRPM = constrain(rapidRPM + e1 * RPM_STEP, RPM_MIN, RPM_MAX);
        else           cutRPM   = constrain(cutRPM   + e1 * RPM_STEP, RPM_MIN, RPM_MAX);
        saveSettings();
        displayDirty = true;
      }
      // LEFT button toggles which speed the RPM knob edits
      if (fwdPressed) {
        editRapid    = !editRapid;
        displayDirty = true;
      }
      if (ssPressed && limitSet) {
        startMove(limitSteps, cutRPM);
        state        = ST_CUTTING;
        displayDirty = true;
      }
      // RIGHT button manually disables/re-enables stepper (e.g. to move carriage by hand)
      if (revPressed) {
        stepperManualOff = !stepperManualOff;
        if (stepperManualOff) stepper.disable();
        else                  stepper.enable();
        displayDirty = true;
      }
      // JOG SW: single-click = jog mode, double-click = re-home
      // Hold off single-click by DOUBLE_CLICK_MS so double-click can be detected cleanly.
      {
        static unsigned long enc2FirstMs = 0;
        if (enc2Pressed) {
          if (enc2FirstMs && millis() - enc2FirstMs < DOUBLE_CLICK_MS) {
            // Double-click — go back to homing
            enc2FirstMs = 0;
            limitSet    = false;
            state       = ST_HOMING;
          } else {
            enc2FirstMs = millis();   // first click — start window
          }
          displayDirty = true;
        }
        if (enc2FirstMs && millis() - enc2FirstMs >= DOUBLE_CLICK_MS) {
          // Window expired with no second click — single-click confirmed
          enc2FirstMs = 0;
          state       = ST_JOG;
          displayDirty = true;
        }
      }
      break;

    case ST_JOG:
      // Free jog for loading/unloading — enc2 moves carriage, SW exits
      pendingJogTicks += e2;
      if (!motionActive && pendingJogTicks) {
        startJog(pendingJogTicks); pendingJogTicks = 0; displayDirty = true;
      }
      if (enc2Pressed && !motionActive) {
        pendingJogTicks = 0;
        state        = ST_READY;
        displayDirty = true;
      }
      if (ssPressed && limitSet && !motionActive) {
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

// ── Core 1 ────────────────────────────────────────────────────────────────────
// Dedicated tight loop for step generation.  Calling nextAction() from core 0
// caused jerky motion because LCD I2C updates etc. delayed step timing.
// Core 1 spins freely, calling nextAction() as fast as possible so step
// intervals are accurate regardless of what core 0 is doing.
void setup1() { /* nothing — stepper already initialised by core 0 setup() */ }

void loop1() {
  if (motionActive) {
    if (stepper.nextAction() == 0) {
      motionActive   = false;
      motionComplete = true;   // core 0 will see this and handle state transition
    }
  }
}
