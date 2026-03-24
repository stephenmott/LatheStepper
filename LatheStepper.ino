/*
  LatheStepper - Lathe carriage motor control
  Sieg C0 Z-axis (lead screw) continuous-run controller.

  Controls:
    Forward button  (A0) - run carriage forward
    Reverse button  (A1) - run carriage in reverse
    Start/Stop      (A2) - stop / resume motor (fits a separate box by the motor)
    Rotary encoder  (2,3) - adjust speed in RPM_STEP increments

  Display (20x4 I2C LCD @ 0x27):
    Row 0: direction with movement arrows when running
    Row 1: speed in RPM
    Row 2: RUNNING / STOPPED status
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DRV8825.h"

// ── Motor ──────────────────────────────────────────────────────────────────
#define MOTOR_STEPS  400    // 0.9°/step NEMA 17
#define DIR_PIN      8
#define STEP_PIN     9
#define ENABLE_PIN   4
#define MODE0        10
#define MODE1        11
#define MODE2        12

// ── Buttons (active-LOW, internal pull-up) ─────────────────────────────────
#define BTN_FORWARD   A0
#define BTN_REVERSE   A1
#define BTN_STARTSTOP A2

// ── Rotary encoder ─────────────────────────────────────────────────────────
#define ENC_CLK  2          // must be an interrupt-capable pin
#define ENC_DT   3

// ── Speed limits ───────────────────────────────────────────────────────────
#define RPM_MIN      10
#define RPM_MAX     400
#define RPM_DEFAULT 100
#define RPM_STEP     10

// ── Objects ────────────────────────────────────────────────────────────────
DRV8825 stepper(MOTOR_STEPS, DIR_PIN, STEP_PIN, ENABLE_PIN, MODE0, MODE1, MODE2);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ── State ──────────────────────────────────────────────────────────────────
volatile int encTicks = 0;
int    rpm     = RPM_DEFAULT;
bool   running = false;
int8_t dir     = 1;          // 1 = forward, -1 = reverse

// ── Encoder ISR ────────────────────────────────────────────────────────────
void encoderISR() {
  encTicks += (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) ? 1 : -1;
}

// ── Button debounce ────────────────────────────────────────────────────────
struct Button { uint8_t pin; bool last; unsigned long changed; };
Button btnFwd = { BTN_FORWARD,   HIGH, 0 };
Button btnRev = { BTN_REVERSE,   HIGH, 0 };
Button btnSS  = { BTN_STARTSTOP, HIGH, 0 };

// Returns true once on the falling edge (button press), with 50 ms debounce.
bool pressed(Button &b) {
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

  lcd.init();
  lcd.backlight();

  stepper.begin(rpm);
  stepper.setEnableActiveState(LOW);
  stepper.setMicrostep(32);
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
