/*
 * Blue Pill LED blink -- test image for the ESP8266 SWD probe.
 * LED is PC13, active LOW (heartbeat: short pulse, long gap).
 */
#ifndef LED_BUILTIN
#define LED PC13
#else
#define LED LED_BUILTIN
#endif

void setup() {
  pinMode(LED, OUTPUT);
}

void loop() {
  digitalWrite(LED, LOW);   /* LED on  (active low) */
  delay(120);
  digitalWrite(LED, HIGH);  /* LED off */
  delay(880);
}
