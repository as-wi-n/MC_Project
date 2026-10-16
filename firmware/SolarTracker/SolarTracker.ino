/*
 * Dual-Axis Solar Tracker - STM32F103 (Blue Pill)  [project build]
 * ------------------------------------------------------------------
 * Four quadrant LDRs sense the brightest direction. The firmware
 * filters the readings, computes horizontal (azimuth) and vertical
 * (elevation) errors and drives two hobby servos toward the light
 * with a proportional, dead-banded response. Angle limits protect the
 * mechanics and a dark threshold parks the panel at night.
 *
 * Design goals: precise, jitter-free tracking and a control loop that
 * never stalls. If the OLED proves slow (bad I2C wiring), the firmware
 * auto-detects it at boot and disables the display so tracking stays
 * fast -- it keeps working, just without the screen.
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
 *   Servo azimuth    PA8   (horizontal)
 *   Servo elevation  PA6   (vertical)
 *   OLED SDA / SCL   PB7 / PB6   (I2C1, SSD1306 128x64 @0x3C)
 *   Debug serial     PA9 (TX) / PA10 (RX)   USART1, 115200
 *   Status LED       PC13  (active low, on-board)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Servo.h>

/* ---------------- Build options ---------------- */
#define ENABLE_OLED        1        /* 0 = compile out the display entirely */
#define SERIAL_BAUD        115200

/* ---------------- Pin map ---------------- */
#define LDR_TL_PIN   PA0
#define LDR_TR_PIN   PA1
#define LDR_BL_PIN   PA4
#define LDR_BR_PIN   PA5
#define SERVO_AZ_PIN PA8
#define SERVO_EL_PIN PA6
#define LED_PIN      PC13

/* ---------------- Sensing ---------------- */
#define LDR_SAMPLES        9        /* odd; median rejects glitches          */
#define LDR_EMA_SHIFT      1        /* EMA smoothing: 0 = off, 1 = 1/2, 2 = 1/4.
                                      Keep light: the median of 9 already rejects
                                      spikes, and a heavy EMA lags the error enough
                                      to make the panel overshoot into a limit cycle
                                      (oscillate) instead of settling at balance. */

/* ---------------- Control ---------------- */
#define UPDATE_MS          60       /* control-loop period (ms)              */
#define DEADBAND           60       /* ADC counts: ignore smaller errors     */
#define AZ_GAIN_DIV        40       /* step degrees = |err|/DIV (+1, capped) */
#define EL_GAIN_DIV        40
#define AZ_MAX_STEP_DEG    2        /* max degrees moved per update. 2 deg @ 60 ms
                                      = 33 deg/s: fast enough to follow the sun,
                                      slow enough that the (small) filter delay
                                      cannot carry the panel past the balance point. */
#define EL_MAX_STEP_DEG    2

/* ---------------- Limits (degrees) ---------------- */
#define AZ_MIN             0
#define AZ_MAX             180
#define EL_MIN             10
#define EL_MAX             90
#define AZ_CENTER          90       /* power-on azimuth                      */
#define EL_CENTER          50       /* power-on elevation                    */

#define AZ_INVERT          1        /* flip to -1 if an axis turns the wrong way */
#define EL_INVERT          1

/* ---------------- Day / night ---------------- */
#define NIGHT_ON           120      /* brightest LDR below this -> night     */
#define NIGHT_OFF          260      /* rises above this to wake (hysteresis) */
#define PARK_AZ            90       /* night rest position                   */
#define PARK_EL            10

/* ---------------- OLED ---------------- */
#define OLED_ADDR          0x3C
#define SCREEN_W           128
#define SCREEN_H           64
#define OLED_UPDATE_MS     250      /* display refresh period                */
#define OLED_SLOW_MS       300      /* a single refresh slower than this = disable */

/* ---------------- Globals ---------------- */
Servo servoAz;
Servo servoEl;
#if ENABLE_OLED
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
bool oledOk = false;
#endif

int azAngle = AZ_CENTER;
int elAngle = EL_CENTER;
bool night = false;

int tl, tr, bl, br;             /* filtered readings (for display/serial) */
int ldrInit = 0;                /* count of readings used to seed the EMA  */

uint32_t lastUpdate = 0;
uint32_t lastDisplay = 0;

/* ---------------- Sensing ---------------- */
/* Median of LDR_SAMPLES raw reads -- rejects single-sample spikes. */
static uint16_t readMedian(uint8_t pin)
{
  uint16_t s[LDR_SAMPLES];
  for (uint8_t i = 0; i < LDR_SAMPLES; i++)
    s[i] = analogRead(pin);
  for (uint8_t i = 1; i < LDR_SAMPLES; i++) {      /* insertion sort */
    uint16_t k = s[i];
    int8_t j = i - 1;
    while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
    s[j + 1] = k;
  }
  return s[LDR_SAMPLES / 2];
}

/* Exponential moving average to smooth the display/control input. */
static int smoothen(int prev, int x)
{
#if LDR_EMA_SHIFT > 0
  return prev + ((x - prev) >> LDR_EMA_SHIFT);
#else
  return x;
#endif
}

/* ---------------- OLED ---------------- */
#if ENABLE_OLED
static void updateDisplay(int azErr, int elErr)
{
  if (!oledOk)
    return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(F("Solar Tracker"));

  display.setCursor(0, 12);
  display.print(F("TL")); display.print(tl);
  display.setCursor(64, 12);
  display.print(F("TR")); display.print(tr);

  display.setCursor(0, 22);
  display.print(F("BL")); display.print(bl);
  display.setCursor(64, 22);
  display.print(F("BR")); display.print(br);

  display.setCursor(0, 36);
  display.print(F("AZ:")); display.print(azAngle);
  display.setCursor(64, 36);
  display.print(F("EL:")); display.print(elAngle);

  display.setCursor(0, 48);
  display.print(F("dA:")); display.print(azErr);
  display.setCursor(64, 48);
  display.print(F("dE:")); display.print(elErr);

  display.setCursor(0, 57);
  display.print(night ? F("NIGHT - parked") : F("DAY - tracking"));

  display.display();
}
#endif

/* ---------------- Setup ---------------- */
void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);          /* LED off (active low) */

  analogReadResolution(12);

  servoAz.attach(SERVO_AZ_PIN);
  servoEl.attach(SERVO_EL_PIN);
  servoAz.write(azAngle);
  servoEl.write(elAngle);

#if ENABLE_OLED
  Wire.begin();
  Wire.setClock(400000);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Solar Tracker"));
    display.println(F("Booting..."));
    uint32_t t0 = millis();
    display.display();
    uint32_t dt = millis() - t0;
    if (dt > OLED_SLOW_MS) {
      /* I2C is stalling (usually missing pull-ups / long wires). Drop the
       * display so the control loop stays fast and precise. */
      oledOk = false;
    }
  }
#endif

  Serial.println();
  Serial.println(F("Solar Tracker ready"));
#if ENABLE_OLED
  if (!oledOk)
    Serial.println(F("WARN: OLED skipped (not found or I2C too slow - add 4.7k pull-ups)"));
#endif
}

/* ---------------- Main loop ---------------- */
void loop()
{
  uint32_t now = millis();

  if (now - lastUpdate < UPDATE_MS)
    return;
  lastUpdate = now;

  /* 1. Sense (median + EMA) */
  int ntl = readMedian(LDR_TL_PIN);
  int ntr = readMedian(LDR_TR_PIN);
  int nbl = readMedian(LDR_BL_PIN);
  int nbr = readMedian(LDR_BR_PIN);

  if (ldrInit < 8) {                    /* seed the filter quickly */
    tl = ntl; tr = ntr; bl = nbl; br = nbr;
    ldrInit++;
  } else {
    tl = smoothen(tl, ntl);
    tr = smoothen(tr, ntr);
    bl = smoothen(bl, nbl);
    br = smoothen(br, nbr);
  }

  /* 2. Errors:  + = light to the left / top */
  int azErr = (tl + bl) - (tr + br);
  int elErr = (tl + tr) - (bl + br);

  /* 3. Day / night with hysteresis */
  int maxLdr = max(max(tl, tr), max(bl, br));
  if (night && maxLdr > NIGHT_OFF)      night = false;
  else if (!night && maxLdr < NIGHT_ON) night = true;

  /* 4. Target angles (proportional within the dead-band) */
  int azTarget = azAngle;
  int elTarget = elAngle;

  if (night) {
    azTarget = PARK_AZ;
    elTarget = PARK_EL;
  } else {
    if (abs(azErr) > DEADBAND) {
      int dir = (azErr > 0) ? -1 : +1;                 /* toward brighter side */
      int mag = min(abs(azErr) / AZ_GAIN_DIV + 1, AZ_MAX_STEP_DEG);
      azTarget = azAngle + dir * mag * AZ_INVERT;
    }
    if (abs(elErr) > DEADBAND) {
      int dir = (elErr > 0) ? +1 : -1;                 /* top brighter -> raise */
      int mag = min(abs(elErr) / EL_GAIN_DIV + 1, EL_MAX_STEP_DEG);
      elTarget = elAngle + dir * mag * EL_INVERT;
    }
  }

  azTarget = constrain(azTarget, AZ_MIN, AZ_MAX);
  elTarget = constrain(elTarget, EL_MIN, EL_MAX);

  /* 5. Actuate only on change */
  if (azTarget != azAngle || elTarget != elAngle) {
    azAngle = azTarget;
    elAngle = elTarget;
    servoAz.write(azAngle);
    servoEl.write(elAngle);
  }

  digitalWrite(LED_PIN, night ? HIGH : LOW);   /* on while tracking */

  /* 6. Report */
  if (now - lastDisplay >= OLED_UPDATE_MS) {
    lastDisplay = now;
#if ENABLE_OLED
    updateDisplay(azErr, elErr);
#endif
    Serial.print(F("TL=")); Serial.print(tl);
    Serial.print(F(" TR=")); Serial.print(tr);
    Serial.print(F(" BL=")); Serial.print(bl);
    Serial.print(F(" BR=")); Serial.print(br);
    Serial.print(F(" dA=")); Serial.print(azErr);
    Serial.print(F(" dE=")); Serial.print(elErr);
    Serial.print(F(" AZ=")); Serial.print(azAngle);
    Serial.print(F(" EL=")); Serial.print(elAngle);
    Serial.println(night ? F(" [NIGHT]") : F(" [DAY]"));
  }
}
