/*
   Microstepping demo

   This requires that microstep control pins be connected in addition to STEP,DIR

   Copyright (C)2015 Laurentiu Badea

   This file may be redistributed under the terms of the MIT license.
   A copy of this license has been included with this distribution in the file LICENSE.
*/
#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "OneButton.h"

LiquidCrystal_I2C lcd(0x27, 20, 4); // set the LCD address to 0x27 for a 16 chars and 2 line display

// Motor steps per revolution. Most steppers are 200 steps or 1.8 degrees/step
#define MOTOR_STEPS 400
#define RPM 200

#define DIR 8
#define STEP 9
#define ENABLE 4
#define SLEEP 13 // optional (just delete SLEEP from everywhere if not used)

// Setup a new OneButton on pin A1.
OneButton button1(A1, true);
// Setup a new OneButton on pin A2.
OneButton button2(A2, true);
// Setup a new OneButton on pin A6.
OneButton button3(A0, true);
// Setup a new OneButton on pin A7.
OneButton button4(A3, true);

#define OneRev 0.9

/*
   Choose one of the sections below that match your board
*/

//#include "DRV8834.h"
//#define M0 10
//#define M1 11
//DRV8834 stepper(MOTOR_STEPS, DIR, STEP, SLEEP, M0, M1);

//#include "A4988.h"
//#define MS1 10
//#define MS2 11
//#define MS3 12
//A4988 stepper(MOTOR_STEPS, DIR, STEP, SLEEP, MS1, MS2, MS3);

#include "DRV8825.h"
#define MODE0 10
#define MODE1 11
#define MODE2 12
DRV8825 stepper(MOTOR_STEPS, DIR, STEP, ENABLE, MODE0, MODE1, MODE2);

// #include "DRV8880.h"
// #define M0 10
// #define M1 11
// #define TRQ0 6
// #define TRQ1 7
// DRV8880 stepper(MOTOR_STEPS, DIR, STEP, SLEEP, M0, M1, TRQ0, TRQ1);

// #include "BasicStepperDriver.h" // generic
// BasicStepperDriver stepper(DIR, STEP);

int angle = 40; // 0.1 mm
float distance = 0;
long aTotal = 0;

/*
     =============================================================================
     %d = signed integer               %f = floating point number
     %s = string                       %.1f = float to 1 decimal place
     %c = character                    %.3f = float to 3 decimal places
     %l = long int
     =============================================================================
*/

#define ARDBUFFER 16

int ardprintf(char *str, ...)
{
  int i, count = 0, j = 0, flag = 0;
  char temp[ARDBUFFER + 1];
  for (i = 0; str[i] != '\0'; i++)  if (str[i] == '%')  count++;

  va_list argv;
  va_start(argv, count);
  for (i = 0, j = 0; str[i] != '\0'; i++)
  {
    if (str[i] == '%')
    {
      temp[j] = '\0';
      Serial.print(temp);
      j = 0;
      temp[0] = '\0';

      switch (str[++i])
      {
        case 'd': Serial.print(va_arg(argv, int));
          break;
        case 'l': Serial.print(va_arg(argv, long));
          break;
        case 'f': Serial.print(va_arg(argv, double));
          break;
        case 'c': Serial.print((char)va_arg(argv, int));
          break;
        case 's': Serial.print(va_arg(argv, char *));
          break;
        default:  ;
      };
    }
    else
    {
      temp[j] = str[i];
      j = (j + 1) % ARDBUFFER;
      if (j == 0)
      {
        temp[ARDBUFFER] = '\0';
        Serial.print(temp);
        temp[0] = '\0';
      }
    }
  };
  Serial.println();
  return count + 1;
}
void setup() {

  Serial.begin(9600);

  lcd.init();                      // initialize the lcd
  lcd.init();
  lcd.backlight();

  /*
     Set target motor RPM.
  */
  stepper.begin(RPM);
  stepper.setEnableActiveState(LOW);
  stepper.enable();
  stepper.setMicrostep(32);   // Set microstep mode to 1:8

  // link the button 1 functions.
  button1.attachClick(click1);
  button1.attachDoubleClick(doubleclick1);
  button1.attachLongPressStart(longPressStart1);
  button1.attachLongPressStop(longPressStop1);
  button1.attachDuringLongPress(longPress1);

  // link the button 2 functions.
  button2.attachClick(click2);
  button2.attachDoubleClick(doubleclick2);
  button2.attachLongPressStart(longPressStart2);
  button2.attachLongPressStop(longPressStop2);
  button2.attachDuringLongPress(longPress2);

  // link the button 3 functions.
  button3.attachClick(click3);
  button3.attachDoubleClick(doubleclick3);
  button3.attachLongPressStart(longPressStart3);
  button3.attachLongPressStop(longPressStop3);
  button3.attachDuringLongPress(longPress3);

  // link the button 4 functions.
  button4.attachClick(click4);
  button4.attachDoubleClick(doubleclick4);
  button4.attachLongPressStart(longPressStart4);
  button4.attachLongPressStop(longPressStop4);
  button4.attachDuringLongPress(longPress4);

}

void loop() {
  char buf[100];
  button1.tick();
  button2.tick();
  button3.tick();
  button4.tick();
  delay(10);

  lcd.setCursor(0, 0);
  sprintf(buf, "Step Angle : %d", angle);
  lcd.print(buf);
  //lcd.print("Step Angle:");
  //lcd.setCursor(12, 0);
  //lcd.print(angle);

  float aval = aTotal * (OneRev / 360);
  lcd.setCursor(0, 1);
  ardprintf(buf, "Distance   : %.3f", aval);
  //sprintf(buf, "Distance   : %d.%02d", (int)aval, (int)(aval*100)%100);
  //strcpy(buf, "Distance   : ");
  //dtostrf(aval, 2, 2, &buf[strlen(buf)]);
  lcd.print(buf);
}

// ----- button 1 callback functions

// This function will be called when the button1 was pressed 1 time (and no 2. button press followed).
void click1() {
  Serial.println("Button 1 click.");
  stepper.setEnableActiveState(LOW);
  stepper.rotate(angle);
  stepper.setEnableActiveState(HIGH);
  aTotal += angle;
} // click1

// This function will be called when the button1 was pressed 2 times in a short timeframe.
void doubleclick1() {
  Serial.println("Button 1 doubleclick.");
  if (angle < 360) {
    angle++;
  }
  aTotal = 0;
} // doubleclick1

// This function will be called once, when the button1 is pressed for a long time.
void longPressStart1() {
  Serial.println("Button 1 longPress start");
} // longPressStart1

// This function will be called often, while the button1 is pressed for a long time.
void longPress1() {
  Serial.println("Button 1 longPress...");
  stepper.rotate(angle);
  aTotal += angle;
} // longPress1

// This function will be called once, when the button1 is released after beeing pressed for a long time.
void longPressStop1() {
  Serial.println("Button 1 longPress stop");
} // longPressStop1

// ... and the same for button 2:

void click2() {
  Serial.println("Button 2 click.");
  stepper.rotate((-1)*angle);
  aTotal -= angle;
} // click2

void doubleclick2() {
  Serial.println("Button 2 doubleclick.");
  if (angle > 1) {
    angle--;
  }
  aTotal = 0;
} // doubleclick2

void longPressStart2() {
  Serial.println("Button 2 longPress start");
} // longPressStart2

void longPress2() {
  Serial.println("Button 2 longPress...");
  stepper.rotate((-1)*angle);
  aTotal -= angle;
} // longPress2

void longPressStop2() {
  Serial.println("Button 2 longPress stop");
} // longPressStop2

// ... and the same for button 3:

void click3() {
  Serial.println("Button 3 click.");
  stepper.rotate((-1)*angle);
  aTotal += angle;
} // click3

void doubleclick3() {
  Serial.println("Button 3 doubleclick.");
  if (angle < 360) {
    angle++;
  }
  aTotal = 0;
} // doubleclick3

void longPressStart3() {
  Serial.println("Button 3 longPress start");
} // longPressStart3

void longPress3() {
  Serial.println("Button 3 longPress...");
  stepper.rotate(angle);
  aTotal += angle;
} // longPress3

void longPressStop3() {
  Serial.println("Button 3 longPress stop");
} // longPressStop3

// ... and the same for button 4:

void click4() {
  Serial.println("Button 4 click.");
  stepper.rotate((-1)*angle);
  aTotal -= angle;
} // click4

void doubleclick4() {
  Serial.println("Button 4 doubleclick.");
  if (angle > 1) {
    angle--;
  }
  aTotal = 0;
} // doubleclick4

void longPressStart4() {
  Serial.println("Button 4 longPress start");
} // longPressStart4

void longPress4() {
  Serial.println("Button 4 longPress...");
  stepper.rotate((-1)*angle);
  aTotal -= angle;
} // longPress4

void longPressStop4() {
  Serial.println("Button 4 longPress stop");
} // longPressStop4
