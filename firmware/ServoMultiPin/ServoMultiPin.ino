/*
 * ServoMultiPin - find which pin (if any) actually drives the servo
 * -----------------------------------------------------------------
 * Sweeps ONE pin at a time. Before each pin it blinks the on-board LED a
 * number of times = the index of the pin being tested, so you can tell which
 * physical header hole the servo is really on.
 *
 *   1 blink  -> PA8  (labelled A8)
 *   2 blinks -> PA6  (labelled A6)
 *   3 blinks -> PA0  (labelled A0)
 *   4 blinks -> PA1  (labelled A1)
 *   5 blinks -> PB0  (labelled B0)
 *   6 blinks -> PB1  (labelled B1)
 *
 * Watch the servo. If it moves during, say, the "3 blink" phase, the signal
 * wire is on PA0 - not PA8. If it never moves in ANY phase, the servo itself
 * (or the 3.3V signal level) is the problem.
 *
 * Build : arduino-cli compile --fqbn STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB
 * Flash : host/flash_stm32.py flash <bin>
 */

#include <Servo.h>

#define LED_PIN PC13

const uint8_t pins[] = { PA8, PA6, PA0, PA1, PB0, PB1 };
const uint8_t NPINS   = sizeof(pins) / sizeof(pins[0]);

Servo sv;

static void blinkTimes(int n)
{
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_PIN, LOW);  delay(180);
    digitalWrite(LED_PIN, HIGH); delay(180);
  }
  delay(400);
}

void setup()
{
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);      /* LED off (active low) */
  delay(400);
  Serial.println(F("ServoMultiPin: cycling pins PA8,PA6,PA0,PA1,PB0,PB1"));
}

void loop()
{
  for (uint8_t i = 0; i < NPINS; i++) {
    blinkTimes(i + 1);              /* 1 blink = PA8, 2 = PA6, ... */
    sv.attach(pins[i]);
    sv.write(90);
    delay(400);
    for (int a = 0; a <= 180; a += 3) { sv.write(a); delay(20); }
    for (int a = 180; a >= 0; a -= 3) { sv.write(a); delay(20); }
    sv.write(90);
    delay(400);
    sv.detach();
    delay(600);
  }
}
