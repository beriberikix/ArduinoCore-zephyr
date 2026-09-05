/*
 * Golioth link test.
 *
 * Touches every symbol the Golioth library imports from the loader, so a
 * missing entry in llext_exports.c shows up as a load failure rather than
 * later, at the first call site.
 *
 * Wi-Fi is not brought up here. <WiFi.h> currently does not compile for
 * frdm_rw612 - NXP's wm_utils.h is not valid C++ - and the loader already
 * associates and runs DHCP from the credentials built into it, so
 * Golioth.begin() is all a sketch needs on this board.
 */

#include <Golioth.h>

void setup() {
  Serial.begin(115200);
  Golioth.begin();
}

void loop() {
  Serial.print("status ");
  Serial.println(Golioth.status());

  if (Golioth.connected()) {
    Golioth.stream("sensor", (float)(millis() / 1000.0));
    Golioth.log("link test alive");
    Golioth.sync(5000);
  }

  delay(10000);
}
