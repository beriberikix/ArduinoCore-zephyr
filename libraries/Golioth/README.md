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
replace the whole OS. That forced the sketch partition to move: it used to sit at
`0x323000`, *inside* `slot1_partition`, which is MCUboot's staging slot.

| partition | offset | size | |
|---|---|---|---|
| `boot_partition` | `0x000000` | 128 K | MCUboot |
| `slot0_partition` | `0x020000` | 3 M | signed loader, XIP |
| `slot1_partition` | `0x320000` | 3 M | OTA staging |
| `storage_partition` | `0x620000` | ~54.9 M | |
| `user_sketch` | `0x3D00000` | 3 M | llext, raw, never touched by a swap |

**The J-Link sketch upload address is now `0x1BD00000`.** `boards.local.txt`
regenerates it from the devicetree, so trust that file.

Note that `extra/build.sh` publishes `zephyr-<variant>.signed.bin` / `.signed.hex`
alongside the unsigned image; the signed one is what goes in slot0 and what you
upload to Golioth. The build uses MCUboot's **default debug signing key** — fine
for development, not for production.

## Reproduce from scratch

### 1. Host prerequisites

```bash
brew install python cmake ninja zstd jq git wget bash gnu-sed arduino-cli
```

macOS ships bash 3.2, but `extra/build.sh` uses `mapfile` (bash 4+) and GNU
`sed -i`. Run the loader build through Homebrew's bash with GNU sed on PATH:

```bash
PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH" bash extra/build.sh frdm_rw612
```

### 2. Workspace

`west init -l` puts the workspace top directory at the *parent* of this repo:

```
<topdir>/
├── ArduinoCore-zephyr/     <- this repo
├── zephyr/  modules/  bootloader/
└── modules/lib/pouch/
```

```bash
cd ArduinoCore-zephyr
./extra/bootstrap.sh
. venv/bin/activate
pip install -r ../modules/lib/pouch/requirements.txt   # zcbor CLI, required by Pouch's CMake
pip install cryptography intelhex                      # imgtool, only if you use sysbuild
west blobs fetch arduino-api hal_nxp                   # interactive: NXP licences
```

`west.yml` adds `pouch` at a pinned revision and adds `zcbor` to the
`name-allowlist` — `CONFIG_POUCH` selects `ZCBOR`, and without the allowlist
entry west never clones it.

### 3. Credentials

Issue a device certificate from
[Golioth PKI](https://docs.golioth.io/connectivity/credentials/pki). Golioth
reads the certificate's `O=` as the project ID and `CN=` as the device ID; the
device is auto-provisioned on first connect.

```bash
cp loader/pouch_credentials.h.example loader/pouch_credentials.h
openssl x509 -in dev.crt.pem -outform DER | xxd -i    # -> device_crt_der[]
openssl pkey -in dev.key.pem -outform DER | xxd -i    # -> device_key_der[]
```

Set `POUCH_WIFI_SSID` / `POUCH_WIFI_PSK` in the same file. Leaving the SSID
empty is supported — the glue then waits for you to run
`wifi connect -s <ssid> -p <psk> -k 1` from the Zephyr shell.

`loader/pouch_credentials.h` is gitignored.

### 4. Build and flash the loader

```bash
PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH" bash extra/build.sh frdm_rw612
west flash -d build/frdm_rw612_rw612
```

### 5. Install the core for the IDE/CLI

```bash
ln -sfn "$PWD" ~/Documents/Arduino/hardware/arduino-git/zephyr
arduino-cli core install arduino:zephyr     # toolchain and sketch tools
```

The folder **must** be named `zephyr` — it becomes the FQBN architecture and
gates `architectures=` library selection.

### 6. Build and upload a sketch

```bash
arduino-cli compile --fqbn arduino-git:zephyr:frdm_rw612 \
  libraries/Golioth/examples/GoliothStream
```

`boards.txt` uploads with `pyocd` (CMSIS-DAP). If your MCU-Link runs SEGGER
firmware instead, write the sketch into the `user_sketch` partition directly —
the address is `frdm_rw612.upload.address` in the generated `boards.local.txt`:

```bash
arduino-cli compile --fqbn arduino-git:zephyr:frdm_rw612 --output-dir /tmp/gs \
  libraries/Golioth/examples/GoliothStream
JLinkExe -device RW612 -if SWD -speed 4000 -autoconnect 1 -CommanderScript - <<'EOF'
r
loadbin /tmp/gs/GoliothStream.ino.elf-zsk.bin, 0x18323000
r
g
q
EOF
```

### 7. Watch it

Two serial ports, and it matters which:

- **MCU-Link VCOM** — Zephyr logs. `zephyr,console` is `flexcomm3`.
- **Native USB CDC** (`Freedom RW612`, VID `0x1209` PID `0x0002`) — the sketch's
  `Serial`, and the Zephyr shell after the loader migrates it.

The loader **blocks in `main()` until the native USB CDC is opened** — see the
`uart_line_ctrl_get(DTR)` loop in `loader/main.c` — so nothing runs until you
attach a terminal to it.

```bash
goliothctl logs --device <id> --interval 15m
```

## Gotchas found while bringing this up

- **`<WiFi.h>` does not compile for `frdm_rw612`.** NXP's `wm_utils.h` calls
  `hex2bin(ibuf, strlen(ibuf), ...)` with a `const uint8_t *`, which is legal C
  and illegal C++. It reaches sketches through the llext EDK include tree, so
  any C++ sketch including `<WiFi.h>` fails. Pre-existing and unrelated to this
  library — the stock `WiFiWebClient` example fails identically. Not a problem
  here, because the loader owns Wi-Fi.
- **Nothing starts DHCP outside a sketch.** The core only calls
  `net_dhcpv4_start()` from `libraries/SocketWrapper`, via `WiFi.begin()`, and
  this variant sets no `CONFIG_NET_CONFIG_*`. The glue does it itself.
- **RSA is not optional.** The DTLS trust root for `coap.golioth.io` is ISRG
  Root X1, RSA-4096. Without `CONFIG_PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY` and
  friends, credential parsing fails with `MBEDTLS_ERR_X509_UNKNOWN_OID`
  (`-0x2100`) even though the device certificate is P-256.
- **The gateway rejects an empty pouch** with `4.00`. Pouch only queues its
  header once there is data, so a sync with nothing pending POSTs a zero-length
  body. The glue skips those syncs — which means **downlink only happens
  alongside an uplink or an explicit `Golioth.sync()`**. Relevant if you later
  add Settings or OTA.
- **`CONFIG_LOG_BACKEND_UART=y` is needed on this variant.** The loader moves
  the shell (and with it the shell's log backend) to USB CDC, leaving the
  console UART silent otherwise.
- **Golioth Logging has no Pouch API.** Logs are a CBOR `{"level","message"}`
  map streamed to `.s/logs`, picked up by the project's default CBOR logs
  Pipeline. That is what `Golioth.log()` does.

## Out of scope

Golioth RPC and LightDB State, because Pouch does not implement them (see the
table above). The BLE GATT device and gateway roles exist in this tree but are
configured per-variant and are not what this README documents.

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
- **One artifact per deployment.** Pouch's
  `golioth_sdk/ota.c:ota_receive_manifest()` fails at its first
  `zcbor_map_start_decode` on a two-component manifest -
  `<err> ota: Failed to deserialize manifest` on every sync, and nothing ever
  updates. The cloud accepts an `artifactIds` array quite happily; the device
  cannot parse the result. Ship the loader and the sketch as separate deployments.

Expect `Package <other> is not included in the active deployment` in the cloud
logs while doing that: the device reports every registered component on every
sync, so whichever package the active deployment does not name gets flagged.
It is cosmetic.

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
