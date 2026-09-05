# Golioth

Stream data to [Golioth](https://golioth.io) from an Arduino sketch, over the
[Pouch](https://github.com/golioth/pouch) protocol.

Validated on the **NXP FRDM-RW612** over Wi-Fi with the CoAP/DTLS transport.

## How it fits together

Pouch registers its uplink and event handlers through **linker iterable
sections** (`POUCH_UPLINK_HANDLER`, `POUCH_EVENT_HANDLER`). An llext cannot
contribute to the loader's iterable sections, so a sketch can never register
one and the Pouch session has to live in the loader.

The loader therefore owns everything — Wi-Fi association, DHCP, credentials,
the PSA key, the CoAP transport and the sync thread — and exports five plain-C
shims (`loader/pouch_glue.c`, exported in `loader/llext_exports.c`):

| symbol | purpose |
|---|---|
| `arduino_pouch_begin` | start the session |
| `arduino_pouch_set_credentials` | override the built-in cert/key |
| `arduino_pouch_stream` | write an uplink entry |
| `arduino_pouch_status` | idle / connecting / online, or `-errno` |
| `arduino_pouch_sync_now` | flush without waiting out the interval |

They exchange only scalars and pointers, so sketches need no Pouch or PSA
headers and are unaffected by Pouch's API churn.

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

BLE GATT transport, OTA, Settings, RPCs and State are not wired up. Note that
Pouch OTA implies MCUboot images while Arduino sketches are llext files in the
`user_sketch` partition — two different update mechanisms. This PoC uses no
MCUboot, so adding Pouch OTA later would mean re-partitioning the flash.
