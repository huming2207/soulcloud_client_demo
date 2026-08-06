# Setup & bring-up guide (Linux / macOS)

Everything needed to take this project from a fresh machine to a running
device — real hardware or QEMU — talking to the soulcloud backend.

## Repositories

| Repo | Purpose | Notes |
|---|---|---|
| `soulcloud_client_demo` | this project: ESP32 firmware (components + main), docs | nested submodules! |
| `soulcloud.js` | backend: REST API + MQTT-over-WS broker (Bun + Prisma + Postgres) | separate repo |
| `docs` (optional) | design proposals (soulcloudjs/14-19) | reference only |

## Prerequisites

- **ESP-IDF 6.0.x** (`~/.espressif` toolchain; the project pins `idf: ">=6.0"`)
- **Bun ≥ 1.3** (backend) — `curl -fsSL https://bun.sh/install | bash`
- **PostgreSQL 16+** — via Docker/OrbStack (`docker compose up -d --wait postgres`
  in `soulcloud.js`) or a local server
- **QEMU (optional, for E2E)** — Espressif fork:
  `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa` (Linux/macOS
  prebuilts), or see docs/e2e-qemu.md

## Clone

The demo repo contains nested submodules:

```
components/soulcloud            (git submodule → esp_soulcloud_client)
  └── external/mpack            (git submodule → ludocode/mpack)
components/on9log               (git submodule → huming2207/on9log)
```

```sh
git clone --recurse-submodules https://github.com/huming2207/soulcloud_client_demo
cd soulcloud_client_demo
git submodule update --init --recursive   # in case of a shallow/filtered clone
```

`components/soulcloud`'s own `.gitmodules` lists `external/mpack` — a plain
`git clone` without `--recurse-submodules` will leave mpack empty and the
build will fail. Managed components (`espressif/mqtt`) are downloaded by
the IDF component manager at build time.

## Backend bring-up (soulcloud.js)

```sh
cd soulcloud.js
bun install
docker compose up -d --wait postgres        # or point DATABASE_URL elsewhere
export DATABASE_URL="postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud"
export JWT_SECRET="$(openssl rand -base64 48)"   # >= 32 chars, same for api+broker
bun run db:generate
bun run db:deploy
bun run dev                                   # api :8080 + broker :1883/mqtt
```

Health check: `curl http://127.0.0.1:8080/health/ready`.

> Note: the local repo may be ahead of `origin` — run the E2E suite with
> `pytest tests/e2e` (see docs/e2e-qemu.md) before relying on it.

## Provision a device

There is no self-service device-create API yet; provision via SQL + the
credentials endpoint (helper script in this repo):

```sh
./scripts/provision-device.sh my-device-0001
```

It prints the MQTT username (== device uid) and password (shown once) plus
the Kconfig lines to bake into the firmware. Re-running with the same UID
fails on the unique constraint — pick a new UID or delete the old row.

## Build the firmware

```sh
source $IDF_PATH/export.sh          # or ~/esp/esp-idf/export.sh
idf.py set-target esp32s3           # once
idf.py build                        # WiFi mode (default)
```

Two network modes (Kconfig `SOULCLOUD_DEMO_NET_IF`):

| Mode | Config | Use |
|---|---|---|
| WiFi STA | default | real hardware |
| Ethernet (OpenETH) | `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.eth"` | QEMU |

Set the provisioned credentials + broker/API addresses via `idf.py
menuconfig` (Soulcloud client → device/broker/api) or by editing the
`sdkconfig`; they can also be overridden at runtime with the `setConfig`
command (NVS-backed).

### QEMU flash image (E2E)

```sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.eth" build
idf.py merge-bin -o build/qemu/flash_image.bin --fill-flash-size 8MB @build/flash_args
```

## QEMU E2E run

Full scripted flow (connect/auth, commands, stat, logs, OTA) is documented
in docs/e2e-qemu.md. Quick start:

```sh
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=build/qemu/flash_image.bin,if=mtd,format=raw \
  -nic user,model=open_eth -serial stdio
```

The device reaches the host backend at `10.0.2.2` (slirp gateway), which is
already baked into `sdkconfig.defaults.eth`.

## Real hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor     # adjust the port
```

The console carries ESP_LOG text plus the on9log SLIP stream (both sinks
are always on). For readable on9log packets, point the host decoder at
the serial port: `on9log -p /dev/ttyUSB0 -b 115200 --elf build/hello_world.elf`
(see github.com/huming2207/on9log_host).

## Troubleshooting

- **Build fails on missing `external/mpack`**: run
  `git submodule update --init --recursive`.
- **`WS_TRANSPORT` / `MQTT_TRANSPORT_WEBSOCKET` disabled**: both default to
  `y`; if a stripped `sdkconfig` was generated earlier, re-run
  `idf.py fullclean` after deleting `sdkconfig`.
- **Device never connects**: check the credential was issued by
  `POST /v1/devices/:id/credentials` (the placeholder `pending-rotation`
  hash refuses auth), and that `CONFIG_SOULCLOUD_DEVICE_UID` equals the
  MQTT username (the broker requires clientId == username).
- **QEMU shows no network**: the firmware must be the Ethernet build
  (`sdkconfig.defaults.eth`) — Wi-Fi is not emulated.
- **Logs arrive but do not decode**: the uploaded ELF must come from the
  same build as the running firmware (stat.fw == artifact buildId); the
  backend accepts both `.noload_keep_in_elf.*` and IDF 6.0's merged
  `.noload` section names (soulcloud.js ≥ 4352fa2).
