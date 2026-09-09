/*
 * Exercise every Golioth API reachable from an Arduino sketch on the
 * NXP FRDM-RW612, acting as a direct Wi-Fi/CoAP endpoint.
 *
 * Covered: session events, Stream uplink (typed, JSON and raw CBOR), Logs,
 * downlink, Settings for all four value types, and OTA manifests for both the
 * loader image and this sketch.
 *
 * Not covered, because Pouch does not implement them: Golioth RPC and LightDB
 * State. Neither has a header, Kconfig or source file in the pouch module.
 * Logging has no device API either - Golioth.log() encodes CBOR onto .s/logs
 * and a Pipeline picks it up.
 *
 * The loader owns the Pouch session (see libraries/Golioth/README.md), so all
 * this sketch does is register callbacks and produce data. Wi-Fi credentials
 * live in the loader, not here.
 *
 * Set up in the Golioth console to see it all work:
 *   Settings: LOOP_DELAY (int), VERBOSE (bool), THRESHOLD (float), LABEL (string)
 *             Golioth setting keys accept only [A-Z], [0-9] and '_' - a
 *             lowercase key is rejected by the cloud, so it can never match.
 *   OTA:      packages "loader" and "sketch"
 */

#include <Golioth.h>

/* Values owned by the cloud, via Settings. Marked volatile: every one of these
   is written from the loader's Pouch thread and read from loop(). */
static volatile int32_t loopDelayMs = 5000;
static volatile bool verbose = true;
static volatile double threshold = 25.0;
static char label[32] = "unset";

static volatile bool sessionUp = false;
static volatile uint32_t sessionCount = 0;

/* ----------------------------------------------------------- session events */

static void onSession(int event, void *)
{
  sessionUp = (event == GOLIOTH_SESSION_START);
  if (sessionUp) sessionCount++;
  /* Serial from a loader thread is safe here, but keep it to one line. */
  Serial.println(sessionUp ? "[session] start" : "[session] end");
}

/* ---------------------------------------------------------------- downlink */

static void onDownlinkStart(unsigned int streamId, const char *path, uint16_t contentType, void *)
{
  Serial.print("[downlink] stream ");
  Serial.print(streamId);
  Serial.print(" path=");
  Serial.print(path);
  Serial.print(" type=");
  Serial.println(contentType);
}

static void onDownlinkData(unsigned int, const void *data, size_t len, int isLast, void *)
{
  Serial.print("[downlink] ");
  Serial.print(len);
  Serial.print(" bytes");
  Serial.println(isLast ? " (last)" : "");
}

/* ---------------------------------------------------------------- settings */

static int setLoopDelay(int32_t value, void *)
{
  if (value < 500 || value > 60000) return -EINVAL;   /* rejected, reported to Golioth */
  loopDelayMs = value;
  Serial.print("[setting] loop_delay = ");
  Serial.println(value);
  return 0;
}

static int setVerbose(bool value, void *)
{
  verbose = value;
  Serial.print("[setting] verbose = ");
  Serial.println(value ? "true" : "false");
  return 0;
}

static int setThreshold(double value, void *)
{
  threshold = value;
  Serial.print("[setting] threshold = ");
  Serial.println((float)value);
  return 0;
}

static int setLabel(const char *value, size_t len, void *)
{
  if (len >= sizeof(label)) return -ENAMETOOLONG;
  memcpy(label, value, len);
  label[len] = '\0';
  Serial.print("[setting] label = ");
  Serial.println(label);
  return 0;
}

/* --------------------------------------------------------------------- OTA */

static void onManifest(const char *package, const char *current, const char *target,
                       size_t size, void *)
{
  Serial.print("[ota] ");
  Serial.print(package);
  Serial.print(": ");
  Serial.print(current);
  Serial.print(" -> ");
  Serial.print(target);
  Serial.print(" (");
  Serial.print((unsigned long)size);
  Serial.println(" bytes)");

  if (strcmp(current, target) == 0) return;

  /* Taking the decision here is the whole point of registering: a real product
     would gate on battery, user consent or a maintenance window. Accept both. */
  Serial.print("[ota] accepting update for ");
  Serial.println(package);
  Golioth.otaDownload(package);
}

/* -------------------------------------------------------------------------- */

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 5000) { }

  Serial.println("=== GoliothFull ===");

  /* Report our version for the "sketch" OTA package. Bump this in step with
     whatever you publish to Golioth, or the manifest never differs. */
  Golioth.version("1.0.0");

  Golioth.onSession(onSession);
  Golioth.onDownlink(onDownlinkStart, onDownlinkData);

  Golioth.onSetting("LOOP_DELAY", setLoopDelay);
  Golioth.onSetting("VERBOSE", setVerbose);
  Golioth.onSetting("THRESHOLD", setThreshold);
  Golioth.onSetting("LABEL", setLabel);

  Golioth.onFirmwareManifest(onManifest);

  /* The loader autostarts the session on this board, so begin() is a no-op
     here - it is called for portability to boards that do not. */
  Golioth.begin();

  Serial.println("[setup] registered, waiting for the session");
}

void loop()
{
  static uint32_t seq = 0;

  if (!Golioth.connected()) {
    int st = Golioth.status();
    Serial.print("[status] ");
    Serial.println(st < 0 ? "error" : (st == GOLIOTH_CONNECTING ? "connecting" : "idle"));
    if (st < 0) {
      Serial.print("[status] errno ");
      Serial.println(-st);
    }
    delay(1000);
    return;
  }

  /* Stream: typed helpers, a JSON document, and pre-encoded bytes. */
  float reading = 20.0f + (float)(seq % 15);
  Golioth.stream("temperature", reading);
  Golioth.stream("sequence", (int)seq);

  char json[96];
  snprintf(json, sizeof(json), "{\"seq\":%lu,\"label\":\"%s\",\"over\":%s}",
           (unsigned long)seq, label, (reading > threshold) ? "true" : "false");
  Golioth.stream("status", json);

  /* CBOR {"seq": <n>} written raw, to prove streamRaw()'s content type path. */
  uint8_t cbor[8];
  size_t n = 0;
  cbor[n++] = 0xa1;                    /* map(1) */
  cbor[n++] = 0x63;                    /* text(3) */
  cbor[n++] = 's'; cbor[n++] = 'e'; cbor[n++] = 'q';
  cbor[n++] = (uint8_t)(seq & 0x17);   /* small uint, 0..23 */
  Golioth.streamRaw("raw", cbor, n, GOLIOTH_CBOR);

  if (verbose) {
    char msg[64];
    snprintf(msg, sizeof(msg), "seq %lu, %s, session %lu",
             (unsigned long)seq, label, (unsigned long)sessionCount);
    Golioth.log("info", msg);
  }

  /* Flush now rather than waiting out the sync interval, and block briefly so
     the return value actually reflects delivery. */
  int err = Golioth.sync(5000);
  Serial.print("[loop] seq ");
  Serial.print(seq);
  Serial.print(" sync=");
  Serial.println(err);

  seq++;
  delay(loopDelayMs);
}
