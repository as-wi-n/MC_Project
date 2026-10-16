/*
 * LDR test - STM32F103 (Blue Pill)
 * ------------------------------------------------------------------
 * Standalone check for the four quadrant LDRs used by the solar
 * tracker. It averages each sensor, tracks min/max since the last
 * reset, prints a report plus a direction verdict on the debug
 * serial, and blinks the on-board LED as a code for the brightest
 * sensor so it still reports with no serial attached.
 *
 * Target : STM32F103C8/CB (Blue Pill), STM32duino core
 * Build  : arduino-cli compile --fqbn STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB
 * Flash  : via the ESP8266 SWD probe (see host/flash_stm32.py)
 *
 * Pin map
 *   LDR top-left     PA0   (ADC1_IN0)   3.3V -- LDR -- pin -- 10k -- GND
 *   LDR top-right    PA1   (ADC1_IN1)
 *   LDR bottom-left  PA4   (ADC1_IN4)
 *   LDR bottom-right PA5   (ADC1_IN5)
 *   Debug serial     PA9 (TX) / PA10 (RX)   USART1, 115200
 *   Status LED       PC13  (active low, on-board)
 *     blink 1 = TL, 2 = TR, 3 = BL, 4 = BR, 5 = balanced
 */

/* ---------------- Pin map ---------------- */
#define LDR_TL_PIN   PA0
#define LDR_TR_PIN   PA1
#define LDR_BL_PIN   PA4
#define LDR_BR_PIN   PA5
#define LED_PIN      PC13

/* ---------------- Tunables ---------------- */
#define SERIAL_BAUD       115200
#define LDR_SAMPLES       32      /* ADC averages per reading            */
#define LDR_SETTLE_US     200     /* settle delay between samples        */
#define BALANCE_BAND      100     /* within this = "balanced"            */
#define PRINT_MS          500     /* serial report period (ms)           */
#define BLINK_MS          3000    /* LED blink-code period (ms)          */
#define WARN_LOW          20      /* near 0    -> GND short / LDR open   */
#define WARN_HIGH         4075    /* near 4095 -> 10k missing / LDR short*/
#define FLAT_RANGE        30      /* "barely changes" note threshold     */
#define FLAT_AFTER_MS     15000

/* ---------------- Globals ---------------- */
const uint8_t LDR_PINS[4]  = { LDR_TL_PIN, LDR_TR_PIN, LDR_BL_PIN, LDR_BR_PIN };
const char *  LDR_NAMES[4] = { "TL", "TR", "BL", "BR" };

uint16_t val[4];
uint16_t vmin[4] = { 4095, 4095, 4095, 4095 };
uint16_t vmax[4] = { 0, 0, 0, 0 };

uint32_t lastPrint = 0;
uint32_t lastBlink = 0;

/* ---------------- Helpers ---------------- */
static uint16_t readAvg(uint8_t pin)
{
  uint32_t acc = 0;
  for (int i = 0; i < LDR_SAMPLES; i++) {
    acc += analogRead(pin);
    delayMicroseconds(LDR_SETTLE_US);
  }
  return (uint16_t)(acc / LDR_SAMPLES);
}

static void blinkCode(int n)
{
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_PIN, LOW);   /* LED on  (active low) */
    delay(150);
    digitalWrite(LED_PIN, HIGH);  /* LED off */
    delay(250);
  }
}

/* ---------------- Setup ---------------- */
void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);    /* LED off (active low) */

  analogReadResolution(12);
  for (int i = 0; i < 4; i++)
    pinMode(LDR_PINS[i], INPUT_ANALOG);

  Serial.println();
  Serial.println(F("=== LDR test ==="));
  Serial.println(F("Cover each LDR in turn. Higher value = more light."));
  Serial.println(F("Send 'r' to reset min/max."));
}

/* ---------------- Main loop ---------------- */
void loop()
{
  /* 1. Sample all four sensors and track min/max */
  for (int i = 0; i < 4; i++) {
    val[i] = readAvg(LDR_PINS[i]);
    if (val[i] < vmin[i]) vmin[i] = val[i];
    if (val[i] > vmax[i]) vmax[i] = val[i];
  }

  /* 2. 'r' resets the min/max window */
  while (Serial.available()) {
    if (Serial.read() == 'r') {
      for (int i = 0; i < 4; i++) { vmin[i] = 4095; vmax[i] = 0; }
      Serial.println(F("min/max reset"));
    }
  }

  /* 3. Brightest / dimmest and balance */
  int bright = 0, dim = 0;
  for (int i = 1; i < 4; i++) {
    if (val[i] > val[bright]) bright = i;
    if (val[i] < val[dim])    dim    = i;
  }
  bool balanced = (val[bright] - val[dim]) < BALANCE_BAND;

  /* 4. Direction errors (same formulas as the tracker) */
  int azErr = (val[0] + val[2]) - (val[1] + val[3]);   /* + = light on left   */
  int elErr = (val[0] + val[1]) - (val[2] + val[3]);   /* + = light is higher */

  /* 5. Report */
  if (millis() - lastPrint >= PRINT_MS) {
    lastPrint = millis();

    for (int i = 0; i < 4; i++) {
      Serial.print(LDR_NAMES[i]); Serial.print('=');
      Serial.print(val[i]);
      Serial.print(F(" ("));
      Serial.print(val[i] * 100 / 4095);
      Serial.print(F("%) "));
    }
    Serial.println();

    Serial.print(F("  dAz=")); Serial.print(azErr);
    Serial.print(F("  dEl=")); Serial.print(elErr);
    Serial.print(F("  light->"));
    if (balanced) {
      Serial.println(F(" balanced"));
    } else {
      Serial.print(elErr >  BALANCE_BAND ? " UP"   : (elErr < -BALANCE_BAND ? " DOWN"  : ""));
      Serial.print(azErr >  BALANCE_BAND ? " LEFT" : (azErr < -BALANCE_BAND ? " RIGHT" : ""));
      Serial.println();
    }

    /* Wiring checks */
    for (int i = 0; i < 4; i++) {
      if (val[i] < WARN_LOW)
        { Serial.print(F("  WARN ")); Serial.print(LDR_NAMES[i]); Serial.println(F(": near 0 -> pin shorted to GND or LDR open/dark")); }
      if (val[i] > WARN_HIGH)
        { Serial.print(F("  WARN ")); Serial.print(LDR_NAMES[i]); Serial.println(F(": near 4095 -> 10k resistor missing or LDR shorted")); }
      if (vmax[i] - vmin[i] > 0 && vmax[i] - vmin[i] < FLAT_RANGE && millis() > FLAT_AFTER_MS)
        { Serial.print(F("  NOTE ")); Serial.print(LDR_NAMES[i]); Serial.println(F(": barely changes - cover it with your hand to test")); }
    }
    Serial.println();
  }

  /* 6. LED blink code (works without serial) */
  if (millis() - lastBlink >= BLINK_MS) {
    lastBlink = millis();
    blinkCode(balanced ? 5 : bright + 1);
  }
}
