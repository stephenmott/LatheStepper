/*
  LatheStepper - Lathe carriage motor control
  Sieg C0 Z-axis (lead screw) continuous-run controller.

  Board:  Raspberry Pi Pico W (RP2040)
          arduino-pico core v4.4.0 — GP pin numbers used directly.
  Driver: TMC2100 — CFG pins hardwired on module, not driven by Pico.

  Controls:
    Forward button  (GP10) - run carriage forward
    Reverse button  (GP11) - run carriage in reverse
    Start/Stop      (GP12) - stop / resume motor (fits a separate box by the motor)
    Rotary encoder  (GP8, GP9) - adjust speed in RPM_STEP increments

  Display (20x4 I2C LCD @ 0x27, I2C0 on GP0/GP1):
    Row 0: direction with movement arrows when running
    Row 1: speed in RPM
    Row 2: RUNNING / STOPPED status
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "BasicStepperDriver.h"

// ── Motor ──────────────────────────────────────────────────────────────────
#define MOTOR_STEPS  400    // 0.9°/step NEMA 17
#define MICROSTEPS    16    // Set by CFG1/CFG2 on TMC2100 module (both VCC = 16x)
#define DIR_PIN       3
#define STEP_PIN      4
#define ENABLE_PIN    2
// CFG1/CFG2 are hardwired on the TMC2100 module — not driven by the Pico.
// GP5, GP6, GP7 are free.

// ── Buttons (active-LOW, internal pull-up) ─────────────────────────────────
#define BTN_FORWARD   10
#define BTN_REVERSE   11
#define BTN_STARTSTOP 12

// ── Rotary encoder ─────────────────────────────────────────────────────────
#define ENC_CLK  8          // all Pico GPIO pins are interrupt-capable
#define ENC_DT   9

// ── Speed limits ───────────────────────────────────────────────────────────
#define RPM_MIN      10
#define RPM_MAX     400
#define RPM_DEFAULT 100
#define RPM_STEP     10

// ── Objects ────────────────────────────────────────────────────────────────
BasicStepperDriver stepper(MOTOR_STEPS, DIR_PIN, STEP_PIN, ENABLE_PIN);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ── State ──────────────────────────────────────────────────────────────────
volatile int encTicks = 0;
int    rpm     = RPM_DEFAULT;
bool   running = false;
int8_t dir     = 1;          // 1 = forward, -1 = reverse

// ── Button debounce ────────────────────────────────────────────────────────
// Struct must be declared before any functions to avoid Arduino preprocessor
// inserting auto-prototypes before the type is defined.
struct DebouncedBtn { uint8_t pin; bool last; unsigned long changed; };
DebouncedBtn btnFwd = { BTN_FORWARD,   HIGH, 0 };
DebouncedBtn btnRev = { BTN_REVERSE,   HIGH, 0 };
DebouncedBtn btnSS  = { BTN_STARTSTOP, HIGH, 0 };

// ── Encoder ISR ────────────────────────────────────────────────────────────
void encoderISR() {
  encTicks += (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) ? 1 : -1;
}

// Returns true once on the falling edge (button press), with 50 ms debounce.
bool pressed(DebouncedBtn &b) {
  bool state = digitalRead(b.pin);
  if (state != b.last && millis() - b.changed > 50) {
    b.changed = millis();
    b.last = state;
    return state == LOW;
  }
  return false;
}

// ── Display ────────────────────────────────────────────────────────────────
void updateDisplay() {
  char buf[21];

  // Row 0 – direction label; arrows shown only while running
  lcd.setCursor(0, 0);
  sprintf(buf, "%-20s", running
    ? (dir > 0 ? ">> FORWARD          " : "<< REVERSE          ")
    : (dir > 0 ? "   FORWARD          " : "   REVERSE          "));
  lcd.print(buf);

  // Row 1 – speed
  lcd.setCursor(0, 1);
  sprintf(buf, "Speed:  %3d RPM     ", rpm);
  lcd.print(buf);

  // Row 2 – status
  lcd.setCursor(0, 2);
  lcd.print(running ? "Status: RUNNING     " : "Status: STOPPED     ");

  // Row 3 – blank
  lcd.setCursor(0, 3);
  lcd.print("                    ");
}

// ── Motor helpers ──────────────────────────────────────────────────────────

// Start (or restart after direction change) continuous rotation.
void startMotor() {
  stepper.setRPM(rpm);
  stepper.enable();
  // 360000° ≈ 1000 revolutions per block; restarted seamlessly in loop().
  stepper.startRotate((long)dir * 360000L);
  running = true;
  updateDisplay();
  Serial.print(dir > 0 ? "FORWARD" : "REVERSE");
  Serial.print(" @ "); Serial.print(rpm); Serial.println(" RPM");
}

void stopMotor() {
  stepper.stop();
  stepper.disable();
  running = false;
  updateDisplay();
  Serial.println("STOPPED");
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  // Pico I2C0 defaults to GP4/GP5 — override to our wiring (GP0/GP1).
  Wire.setSDA(0);
  Wire.setSCL(1);
  lcd.init();
  lcd.backlight();

  stepper.begin(rpm, MICROSTEPS);
  stepper.setEnableActiveState(LOW);
  stepper.disable();

  pinMode(BTN_FORWARD,   INPUT_PULLUP);
  pinMode(BTN_REVERSE,   INPUT_PULLUP);
  pinMode(BTN_STARTSTOP, INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);

  updateDisplay();
  Serial.println("LatheStepper ready.");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {

  // Keep the motor stepping; restart the next 1000-rev block seamlessly.
  if (running && stepper.nextAction() == 0) {
    stepper.startRotate((long)dir * 360000L);
  }

  // Encoder – adjust speed
  if (encTicks != 0) {
    noInterrupts();
    int ticks = encTicks;
    encTicks = 0;
    interrupts();
    rpm = constrain(rpm + ticks * RPM_STEP, RPM_MIN, RPM_MAX);
    if (running) stepper.setRPM(rpm);
    updateDisplay();
    Serial.print("RPM: "); Serial.println(rpm);
  }

  // Forward button – set direction and run
  if (pressed(btnFwd)) {
    dir = 1;
    startMotor();
  }

  // Reverse button – set direction and run
  if (pressed(btnRev)) {
    dir = -1;
    startMotor();
  }

  // Start/Stop button – toggle
  if (pressed(btnSS)) {
    running ? stopMotor() : startMotor();
  }
}
