/*
 * Stream the Nicla Vision's on-board time-of-flight distance to Golioth,
 * relayed over BLE by a Pouch gateway.
 *
 * The VL53L1CB sits at 0x29 on i2c2, which the variant exposes to sketches as
 * Wire1. Zephyr's st,vl53l1x driver is not built into this loader (no room and
 * no exported sensor API), so this talks to the part directly - it uses 16-bit
 * register addresses, and needs its 91-byte default configuration written once
 * before ranging.
 *
 * Wi-Fi is not used: the loader advertises Pouch over BLE and a gateway
 * collects. See libraries/Golioth/README.md.
 */

#include <Wire.h>
#include <Golioth.h>

static const uint8_t TOF_ADDR = 0x29;

/* VL53L1X default configuration, registers 0x2D..0x87 (ST ULD). */
static const uint8_t TOF_CONFIG[] = {
  0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x02, 0x08, 0x00, 0x08, 0x10, 0x01,
  0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x20, 0x0b, 0x00, 0x00, 0x02, 0x0a, 0x21, 0x00, 0x00, 0x05,
  0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x38, 0xff, 0x01, 0x00, 0x08,
  0x00, 0x00, 0x01, 0xdb, 0x0f, 0x01, 0xf1, 0x0d, 0x01, 0x68, 0x00, 0x80,
  0x08, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x89, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x01, 0x0f, 0x0d, 0x0e, 0x0e, 0x00, 0x00, 0x02, 0xc7,
  0xff, 0x9b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00
};

static void tofWrite(uint16_t reg, const uint8_t *data, size_t len) {
  Wire1.beginTransmission(TOF_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)(reg & 0xff));
  if (len) Wire1.write(data, len);
  Wire1.endTransmission();
}

static void tofWrite8(uint16_t reg, uint8_t value) { tofWrite(reg, &value, 1); }

static uint32_t tofRead(uint16_t reg, uint8_t bytes) {
  Wire1.beginTransmission(TOF_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)(reg & 0xff));
  Wire1.endTransmission();
  if (Wire1.requestFrom(TOF_ADDR, (size_t)bytes) != bytes) return 0xffffffff;
  uint32_t v = 0;
  for (uint8_t i = 0; i < bytes; i++) v = (v << 8) | (uint8_t)Wire1.read();
  return v;
}

void setup() {
  Serial.begin(115200);
  Wire1.begin();

  /* Bounded: a sensor still held in shutdown would otherwise hang setup(). */
  for (int i = 0; i < 100 && (tofRead(0x00e5, 1) & 0x01) == 0; i++) {
    delay(10);
  }
  tofWrite(0x002d, TOF_CONFIG, sizeof(TOF_CONFIG));
  tofWrite8(0x0087, 0x40);                     /* start ranging */

  Golioth.begin();
}

void loop() {
  /*
   * Report unconditionally rather than only when data-ready is set: on this
   * board Serial goes to a USB CDC that is not always attached, so Golioth is
   * the only reliable way to see what the sketch is doing.
   */
  uint8_t ready = (uint8_t)tofRead(0x0031, 1);
  uint16_t mm = (uint16_t)tofRead(0x0096, 2);
  tofWrite8(0x0086, 0x01);                     /* clear interrupt */

  Serial.print("distance_mm ");
  Serial.println(mm);

  char json[64];
  snprintf(json, sizeof(json), "{\"mm\":%u,\"ready\":%u}", mm, ready & 0x01);
  Golioth.stream("distance", json);

  /* Also log it: .s/distance needs a Pipeline routing it somewhere, whereas
   * the default CBOR logs Pipeline makes this visible with no project setup. */
  char msg[96];
  snprintf(msg, sizeof(msg),
           "mm=%u ready=%u id=%04x fwstat=%02x rangestat=%02x",
           mm, ready & 0x01,
           (unsigned)tofRead(0x010f, 2),   /* IDENTIFICATION__MODEL_ID, expect 0xeacc */
           (unsigned)tofRead(0x00e5, 1),   /* FIRMWARE__SYSTEM_STATUS  */
           (unsigned)tofRead(0x0089, 1));  /* RESULT__RANGE_STATUS     */
  Golioth.log(msg);

  delay(5000);
}
