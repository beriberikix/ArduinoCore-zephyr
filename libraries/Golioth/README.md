# Golioth

Stream data to [Golioth](https://golioth.io) from an Arduino sketch, over the
[Pouch](https://github.com/golioth/pouch) protocol.

Validated on the **NXP FRDM-RW612** over Wi-Fi with the CoAP/DTLS transport.

## How it fits together

Pouch registers **all six** of its handler kinds through **linker iterable
sections** (`POUCH_UPLINK_HANDLER`, `POUCH_EVENT_HANDLER`,
`POUCH_DOWNLINK_HANDLER`, `GOLIOTH_SETTINGS_HANDLER`, `GOLIOTH_OTA_COMPONENT`,
`GOLIOTH_OTA_MANIFEST_HANDLER`). There is no runtime `register()` anywhere in
Pouch. An llext cannot contribute to the loader's iterable sections, so a sketch
can never register one and every session, service and handler has to live in the
loader.

The loader therefore owns everything — Wi-Fi association, DHCP, credentials, the
PSA key, the CoAP transport, the sync thread, and one section entry per handler —
and re-exports runtime registration shims (`loader/pouch_glue.c`,
`loader/pouch_services.c`, `loader/pouch_ota.c`, declared in
`loader/arduino_pouch.h`, exported in `loader/llext_exports.c`):

| symbol | purpose |
|---|---|
| `arduino_pouch_begin` | start the session |
| `arduino_pouch_set_credentials` | override the built-in cert/key |
| `arduino_pouch_stream` | write an uplink entry |
| `arduino_pouch_status` | idle / connecting / online, or `-errno` |
| `arduino_pouch_sync_now` | flush without waiting out the interval |
| `arduino_pouch_on_event` | session start/end callback |
| `arduino_pouch_on_downlink` | downlink start/data callbacks |
| `arduino_pouch_on_setting_{int,bool,float,string}` | bind a Settings key to a callback |
| `arduino_pouch_on_ota_manifest` | take over the OTA download decision |
| `arduino_pouch_ota_mark` | request download / go idle |
| `arduino_pouch_ota_sketch_version` | declare the running sketch's version |
| `arduino_pouch_gateway_bonding` | open/close the BLE pairing window |

They exchange only scalars and pointers, so sketches need no Pouch or PSA
headers and are unaffected by Pouch's API churn.

Callbacks run on the loader's Pouch thread — never in ISR context, never on the
sketch's own thread. Calling into an llext is safe here because
`CONFIG_USERSPACE=n` and the sketch runs via `llext_bootstrap()` in supervisor
mode, and it is never unloaded. Keep callbacks short: the session waits on them.

### Two tricks worth knowing

Both services needed something the registration macros do not offer.

**Settings keys are bound at runtime by pointing `.key` at RAM.**
`GOLIOTH_SETTINGS_HANDLER(_name, _fn)` bakes the key in as `.key = #_name` and
infers the type from the callback via `_Generic`. But `.key` is only a
`const char *`, and nothing says it must point at a literal. The loader
pre-declares `CONFIG_ARDUINO_POUCH_SETTING_SLOTS` slots for each of the four
types whose keys live in mutable RAM and start empty. `strcmp()` never matches an
empty slot, and the status uplink reports only `{"version": N}` rather than
enumerating handlers, so unused slots are invisible to the cloud.

**The sketch OTA component expands the macro by hand.**
`GOLIOTH_OTA_COMPONENT` puts `_version` into both `.version` (a `const char *`,
so RAM is fine) *and* `.target` (a `char[]`, which needs a literal initialiser).
A sketch's version is not knowable when the loader is compiled — the zsk header
carries no semantic version — so `loader/pouch_ota.c` expands that macro
manually, seeds `.target` with `"0.0.0"`, and lets a sketch declare its real
version with `Golioth.version()`.

## What is and is not supported

| Golioth service | Status |
|---|---|
| Stream (LightDB Stream) | yes — `Golioth.stream()`, `streamRaw()` |
| Logs | yes, by convention — CBOR `{"level","message"}` on `.s/logs` + a Pipeline |
| Session events | yes — `Golioth.onSession()` |
| Downlink | yes — `Golioth.onDownlink()` |
| Settings | yes — `Golioth.onSetting()`, all four value types |
| OTA | yes — two packages, `loader` (MCUboot) and `sketch` (llext) |
| **RPC** | **not implemented by Pouch** — no header, Kconfig or source |
| **LightDB State** | **not implemented by Pouch** |

RPC and State are absent from the Pouch module entirely, not merely unbridged.
Adding either would mean implementing Golioth's wire protocol on top of
`GOLIOTH_DOWNLINK_HANDLER` (`golioth_sdk/dispatch.h`), not wrapping an API.

**Golioth setting keys accept only `[A-Z]`, `[0-9]` and `_`.** The cloud rejects
a lowercase key outright, so a sketch registering `"loop_delay"` can never match
anything. Use `LOOP_DELAY`.

## Flash layout and MCUboot

The loader is built through **sysbuild with MCUboot** on this board
(`frdm_rw612.build.zephyr_sysbuild=true` in `boards.txt`,
`SB_CONFIG_BOOTLOADER_MCUBOOT=y` in `loader/sysbuild.conf`), so a Golioth OTA can
replace the whole OS. That collided with the sketch: `user_sketch` was carved out
of the front of `slot1_partition`, which is MCUboot's staging slot.

**`image-1` moves, not the sketch.** Nothing hardcodes a slot address — MCUboot
resolves them by devicetree label — whereas `0x18323000` is committed in the
released `arduino:zephyr_contrib` `boards.txt`, so moving the sketch would
silently break every upload made with that core.

| partition | offset | size | |
|---|---|---|---|
| `boot_partition` | `0x000000` | 128 K | MCUboot |
| `slot0_partition` | `0x020000` | 3 M | signed loader, XIP |
| `user_sketch` | `0x323000` | 3 M − 12 K | llext, raw, never touched by a swap |
| `slot1_partition` | `0x620000` | 3 M | OTA staging |
| `storage_partition` | `0x920000` | ~54.9 M | |

The sketch upload address is therefore **unchanged** at `0x18323000`, identical
to the community core.

The map lives in `variants/frdm_rw612_rw612/frdm_rw612_rw612-partitions.dtsi` and
is included by the loader overlay **and** handed to the MCUboot image
(`-Dmcuboot_EXTRA_DTC_OVERLAY_FILE`, wired up in `extra/build.sh`). That sharing
is not optional: each sysbuild image is a separate Zephyr build with its own
devicetree, and MCUboot locates the slots from its own copy. If only the loader
moved a slot, it would stage a downloaded image where MCUboot is not looking —
a silent corruption rather than a clean failure.

Note that `extra/build.sh` publishes `zephyr-<variant>.signed.bin` / `.signed.hex`
alongside the unsigned image; the signed one is what goes in slot0 and what you
upload to Golioth. The build uses MCUboot's **default debug signing key** — fine
for development, not for production.

## OTA in practice

Both packages are registered by the loader and reported independently every
sync. Verified end to end on hardware: a sketch went from a Golioth artifact
into `user_sketch` and ran (`sketch` 1.0.0 -> 1.1.0), and the whole OS was
replaced through an MCUboot swap (`loader` 1.0.1 -> 1.0.2, MCUboot then
chainloading `Image version: v1.0.2` from slot0).

Publishing an update:

```bash
# sketch
arduino-cli compile --fqbn arduino-git:zephyr:frdm_rw612 --output-dir /tmp/gf \
  libraries/Golioth/examples/GoliothFull
goliothctl dfu artifact create /tmp/gf/GoliothFull.ino.elf-zsk.bin \
  --version 1.1.0 --pkg sketch

# loader - pin the version so the built image reports what you publish
PINNED_CORE_VERSION=1.0.2 bash extra/build.sh frdm_rw612
goliothctl dfu artifact create firmwares/zephyr-frdm_rw612_rw612.signed.bin \
  --version 1.0.2 --pkg loader
```

Then create a deployment on the device's cohort. OTA is modelled as Packages +
Cohorts + Deployments under an **org-scoped** API; `goliothctl dfu release
create` does not work against it:

```bash
ORG=https://api.golioth.io/v1/organizations/<org>/projects/<project>
curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  "$ORG/cohorts/<cohort>/deployments" \
  -d '{"name":"...","artifactIds":["<artifactId>"]}'
```

Two rules that are easy to get wrong:

- **Pin the loader version.** `APP_VERSION_STRING` comes from `loader/VERSION`,
  which normally embeds a build timestamp. If the published artifact version and
  the image's compiled-in version disagree, the swapped-in loader reports the
  old version, the manifest still differs, and the device update-reboots forever.
  `PINNED_CORE_VERSION` is what keeps them equal.
- **One artifact per deployment.** A manifest carrying two components fails to
  deserialize on the device - `<wrn> zcbor_util: Did not start CBOR map
  correctly` followed by `<err> ota: Failed to deserialize manifest`, on every
  sync, and nothing ever updates. The cloud accepts an `artifactIds` array quite
  happily; the device cannot parse the result.

  The cause is upstream: `zcbor_map_decode()` stops once it has matched the
  caller's registered entries, and `ota_receive_manifest()` registers four of a
  component's six keys, so the payload pointer is left stranded inside the first
  component. Reported as
  [golioth/pouch#354](https://github.com/golioth/pouch/issues/354). Until that
  lands, ship the loader and the sketch as separate deployments.

Expect `Package <other> is not included in the active deployment` in the cloud
logs while doing that: the device reports every registered component on every
sync, so whichever package the active deployment does not name gets flagged.
It is cosmetic.

## Compatibility with the community core

`arduino:zephyr_contrib` ships its own prebuilt `frdm_rw612` loader and hardcodes
`frdm_rw612.upload.address=0x18323000`. This tree keeps that address, so sketch
uploads line up either way and there is no user-visible address change.

The loader binary itself is still not interchangeable: Pouch registers its
handlers through linker iterable sections, so the session has to be compiled in,
and the community prebuilt loader has no `CONFIG_ARDUINO_POUCH`. Golioth sketches
therefore need the loader from this tree — but a *non*-Golioth sketch built with
either core loads under either loader, and upstreaming needs no change to the
community `boards.txt`.

## Known gaps

- Golioth setting keys are uppercase-only (`[A-Z0-9_]`); a lowercase key is
  rejected by the cloud and can never match on the device.
- `loader/main.c`'s `LOADER_MAX_SIZE` resolves via `DT_HAS_PARTITION_LABEL(image_0)`
  to the full 3 M of slot0, ignoring the MCUboot header and trailer, so it
  overstates the usable size slightly.
- The loader can strand itself in the `uart_line_ctrl_get(DTR)` loop in
  `loader/main.c` when CDC ACM registration fails
  (`<err> usbd_class: Failed to register cdc_acm_0 to HS configuration 1`).
  The board then runs with a silent console and no network until it is reset.
- `<WiFi.h>` still does not compile for `frdm_rw612` (NXP's `wm_utils.h` is not
  valid C++). Harmless here - the loader owns Wi-Fi.
- The MCUboot **default debug signing key** is in use. Fine for development,
  not for production.
