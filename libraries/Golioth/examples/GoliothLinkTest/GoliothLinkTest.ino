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

/* Callbacks exist only so the registration shims get referenced; the link test
   is about symbol resolution, not behaviour. */
static void onSession(int event, void *) { (void)event; }
static void onDownlinkStart(unsigned int, const char *, uint16_t, void *) { }
static void onDownlinkData(unsigned int, const void *, size_t, int, void *) { }
#if defined(CONFIG_GOLIOTH_SETTINGS)
static int onInt(int32_t, void *) { return 0; }
static int onBool(bool, void *) { return 0; }
static int onFloat(double, void *) { return 0; }
static int onString(const char *, size_t, void *) { return 0; }
#endif
#if defined(CONFIG_GOLIOTH_OTA)
static void onManifest(const char *, const char *, const char *, size_t, void *) { }
#endif

void setup() {
  Serial.begin(115200);

  Golioth.onSession(onSession);
  Golioth.onDownlink(onDownlinkStart, onDownlinkData);
#if defined(CONFIG_GOLIOTH_SETTINGS)
  Golioth.onSetting("LT_INT", onInt);
  Golioth.onSetting("LT_BOOL", onBool);
  Golioth.onSetting("LT_FLOAT", onFloat);
  Golioth.onSetting("LT_STRING", onString);
#endif
#if defined(CONFIG_GOLIOTH_OTA)
  Golioth.onFirmwareManifest(onManifest);
  Golioth.version("0.0.1");
  Golioth.otaIdle("sketch");
#endif

  Golioth.begin();
}

void loop() {
  Serial.print("status ");
  Serial.println(Golioth.status());

  if (Golioth.connected()) {
    Golioth.stream("sensor", (float)(millis() / 1000.0));
    Golioth.stream("count", (int)(millis() / 1000));
    Golioth.stream("doc", "{\"alive\":true}");
    uint8_t cbor[] = { 0xa0 };  /* empty CBOR map */
    Golioth.streamRaw("raw", cbor, sizeof(cbor), GOLIOTH_CBOR);
    Golioth.log("link test alive");
    Golioth.sync(5000);
  }

  delay(10000);
}
