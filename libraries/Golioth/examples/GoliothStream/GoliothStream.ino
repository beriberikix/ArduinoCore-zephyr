/*
 * Stream the FRDM-RW612's on-board temperature sensor to Golioth.
 *
 * The P3T1755DP sits on flexcomm2 at 0x48, which is also arduino_i2c, so Wire
 * reaches it with no wiring and no overlay change.
 *
 * Wi-Fi and the Pouch session are owned by the loader; Golioth.begin() just
 * starts them. The loader must be built with CONFIG_ARDUINO_POUCH=y and carry
 * a device certificate (see libraries/Golioth/README.md).
 */

#include <Wire.h>
#include <Golioth.h>

static const uint8_t P3T1755_ADDR = 0x48;
static const uint8_t P3T1755_REG_TEMP = 0x00;

float readTemperatureC() {
  Wire.beginTransmission(P3T1755_ADDR);
  Wire.write(P3T1755_REG_TEMP);
  Wire.endTransmission();

  if (Wire.requestFrom(P3T1755_ADDR, (size_t)2) != 2) {
    return NAN;
  }

  /* 12 bits, left-aligned in 16, two's complement, 0.0625 C per LSB. */
  int16_t raw = (int16_t)((Wire.read() << 8) | Wire.read());
  return (raw >> 4) * 0.0625f;
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Golioth.begin();
}

void loop() {
  float celsius = readTemperatureC();

  Serial.print("temperature ");
  Serial.println(celsius);

  if (!isnan(celsius)) {
    Golioth.stream("temperature", celsius);

    /* Also log it, so the reading is visible in the Golioth console even
     * before a Pipeline is routing .s/temperature anywhere. */
    char msg[32];
    snprintf(msg, sizeof(msg), "temperature %.2f C", celsius);
    Golioth.log(msg);
  }

  delay(30000);
}
