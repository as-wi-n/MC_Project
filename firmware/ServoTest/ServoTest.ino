/*
 * ServoTest - simple servo test for STM32F103 Blue Pill
 * ------------------------------------------------------
 * Sweeps the two servos slowly so you can confirm they move and which
 * direction they turn.
 *
 *   Azimuth servo : signal on PA8   (board label A8)
 *   Elevation servo: signal on PA6  (board label A6)
 *
 * The on-board LED (PC13) blinks while the sketch runs, so a steady LED
 * means the program is alive and the servos are being commanded.
 *
 * Wiring (for a quick bench test, power one servo from the STM32):
 *   servo RED   -> STM32 5V
 *   servo BROWN -> STM32 GND
 *   servo ORANGE-> PA8  (azimuth)  or  PA6 (elevation)
 *
 * Build : arduino-cli compile --fqbn STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB
 * Flash : host/flash_stm32.py flash <bin>
 */

#include <Servo.h>

#define SERVO_AZ_PIN  PA8
#define SERVO_EL_PIN  PA6
#define LED_PIN       PC13

#define STEP_MS       20      // slow enough to see the sweep clearly

Servo azServo;
Servo elServo;

void blinkLed() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  delay(STEP_MS);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  azServo.attach(SERVO_AZ_PIN);
  elServo.attach(SERVO_EL_PIN);
  azServo.write(90);     // start centred
  elServo.write(50);
  delay(500);
}

void loop() {
  // Sweep AZIMUTH: 0 -> 180 -> 0
  for (int a = 0; a <= 180; a++) { azServo.write(a); blinkLed(); }
  for (int a = 180; a >= 0; a--) { azServo.write(a); blinkLed(); }
  delay(500);

  // Sweep ELEVATION: 10 -> 90 -> 10
  for (int a = 10; a <= 90; a++) { elServo.write(a); blinkLed(); }
  for (int a = 90; a >= 10; a--) { elServo.write(a); blinkLed(); }
  delay(500);
}
