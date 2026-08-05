# PLAN: ESP32 Soulcloud Device Client

## Status (2026-08-05)

- Implemented and building: protocol layer (MPack, host-tested 91 checks),
  client core (lifecycle, config_store, command_registry, mqtt_bridge),
  on9log MQTT sink, OTA executor, demo app (WiFi + Ethernet/OpenETH),
  ESP32-S3 target with 8 MB partition table + PSRAM.
- Backend `.noload` incompatibility fixed upstream (soulcloud.js 4352fa2) —
  see docs/logging.md. Device side needed no changes.
- Remaining: hardware/QEMU bring-up and E2E runs — see docs/SETUP.md and
  docs/e2e-qemu.md.
- Onboarding a fresh machine: follow docs/SETUP.md.

## 1. Goal

Build a reusable ESP-IDF component `components/soulcloud` that turns an
ESP32-S3 (esp32-s3-devkitc-1) into a Soulcloud-managed IoT device, plus a
demo application in `main/` that exercises every feature end to end against
the local `soulcloud.js` backend (REST API `:8080` + MQTT-over-WebSocket
broker `:1883/mqtt`).

Wire contract, message shapes and topic scheme are **frozen by the backend
code** (`soulcloud.js/packages/core/src/protocol/*`, `broker/*`, `logging/*`,
`ota/*`). The client must be byte-compatible with them — the backend strictly
rejects anything else.

## 2. Backend contract the client must implement (verified from source)

### 2.1 Transport: MQTT 3.1.1 over WebSocket via esp-mqtt

- **No backend changes required.** Verified against the actual esp-mqtt
  v1.1.0 sources (managed component `espressif/mqtt`): the client supports
  `ws://` and `wss://` schemes (`MQTT_TRANSPORT_WEBSOCKET`/`_SECURE` default
  `y`), and the URI path is parsed by `http_parser_parse_url` and used as the
  WebSocket path — so `ws://host:1883/mqtt` works out of the box against the
  Aedes WS broker.
- Broker URL: `ws://<host>:1883/mqtt`.
- CONNECT: `clientId` MUST equal `username` = device UID; password = issued
  credential (argon2id verified server-side). Any mismatch → CONNACK failure.
- Subscriptions allowed (ACL, QoS 1): exactly
  - `soulcloud/v1/devices/{uid}/cmd/exec`
  - `soulcloud/v1/devices/{uid}/ota`
- Publications allowed (ACL): exactly
  - `soulcloud/v1/devices/{uid}/cmd/result`
  - `soulcloud/v1/devices/{uid}/ota/result`
  - `soulcloud/v1/devices/{uid}/log`
  - `soulcloud/v1/devices/{uid}/stat`
- UID constraints: non-empty, no `/`, `+`, `#`, whitespace.
- Commands are published at QoS 1 only while the device is online
  (server-side poller checks the live client map; offline commands stay queued
  and are delivered on reconnect). The device must therefore:
  - stay connected (keepalive + reconnects),
  - reply PUBACK to every inbound QoS 1 PUBLISH (handled by esp-mqtt),
  - resubscribe after every reconnect (clean session).
- Uplink guards (defaults): max packet 65536 B, 20 msg/s sustained, burst 100.

### 2.2 Command protocol (`cmd/exec`, `cmd/result`) — MessagePack

`cmd/exec` payload (strict map, no extra/unknown/duplicate keys):

| key | type | note |
|---|---|---|
| `id`   | bin, exactly 16 bytes | command UUID (raw bytes) |
| `seq`  | uint 64 | per-device monotonic; echo it back verbatim |
| `cmd`  | str, ≥1 char | command name |
| `args` | nil, or array of single-key maps | values: nil/str/bin/int/float/bool only; bin must be msgpack `bin` type |

`cmd/result` payload: `id` (16 B bin), `seq` (uint64), `code` (int32; 0 = ok,
negative = error), `payload` (nil or same args shape). `args`/`payload` are
always encoded as explicit nil when absent.

### 2.3 OTA (`ota` → download over HTTP → `ota/result`)

Inbound `ota` notice (MessagePack map): `release_id` (uuid str), `job_id`
(uuid str), `bin_sha256` (hex str), `bin_size` (int), `download` { `url`
(**relative** path `/v1/firmware-releases/{id}/bin`), `token` (JWT, opaque to
the device), `expires_at` }, optional `version`.

Download: `GET <api_base> + url` with `Authorization: Bearer <token>`.
Device reports via `ota/result` (MessagePack): `release_id`, `job_id`,
`state` ∈ {`downloaded`, `installed`, `failed`}, `code` (int), optional
`message` (≤512 chars).

Failure codes (frozen): `-1` download failed, `-2` checksum mismatch,
`-3` flash failed, `-4` invalid image, `-5` other, (server-side only: `-7`
stall timeout).

Server state machine: `downloaded`/`installed` are intermediate; **`completed`
is driven only by `stat.fw` matching the release's ELF build id** after the
new firmware boots.

### 2.4 Status (`stat`) — MessagePack, sent periodically

Top-level map, exactly: `sn` (bin), `fw` (bin), `up` (uint64), `rst` (str ≥1).

- `fw` = **SHA-256 of the firmware ELF** (hex-decoded as 32 raw bytes). The
  backend matches it against `firmware_artifacts.buildId` (ELF SHA-256) for
  OTA completion confirmation and log-decoding artifact association.
  → On ESP32: `esp_app_get_elf_sha256()` reads this from the running app's
  `esp_app_desc` (written at build time by esptool). This makes `fw`
  self-describing across OTA without recompiling anything.
- `sn` = device serial (demo: NVS-configured, default MAC hex).
- `up` = uptime in seconds.
- `rst` = reset reason string (demo mapping from `esp_reset_reason_t`).
- Note: backend currently persists only `fw`; the other fields are validated
  and dropped, but must still be present.

### 2.5 Logs (`log`) — on9log binary packets

One MQTT `log` message = one complete on9log packet (18-byte LE header +
payload), no SLIP (that is only for the serial host bridge):

```
u8  magic = 0x9a
u8  type_level      (hi nibble type, lo nibble level)
u16 seq             (wraps)
u32 time_ms         (ms since boot, wraps)
u32 tag_id          (ELF virtual address of tag string)
u32 fmt_id          (ELF virtual address of format string)
u16 payload_len     (0xffff = streaming; device always sends exact length)
```

- LOG payload: `u8 arg_count` + NUL-terminated arg-type table + args
  (types: 1=u32, 2=u64, 3=ptr/u32, 4/5=u32 len + bytes, len 0xffffffff=null).
- DROPPED payload: `u32 dropped_count`.
- TIME_SYNC payload: `u32 boot_time_ms + u32 utc_unix_ms` (optional on device).
- `payload_len` must equal the actual payload length (backend enforces
  `packet.length == 18 + payload_len`).

**String dictionary extraction rules (backend `artifact.ts`) — this dictates
how firmware strings must be placed in the ELF:**

- Strings in sections named `.noload_keep_in_elf*`: contain `%`/`{` → format,
  else → tag.
- Strings in allocated read-only sections matching
  `^\.(rodata|data\.rel\.ro|rodata\.str|srodata)`: tags only (no `%`/`{`).
- If the uploaded ELF yields zero strings, upload is rejected.

## 3. Reused components (user decisions)

### 3.1 `on9log` (github.com/huming2207/on9log) — git submodule

Added as submodule at `components/on9log` (pin the commit used by
`on9log_demo`: `8c11225` "Fix memory issues found by Kimi K3" — the version
the demo project was validated against).

The on9log ESP-IDF component implements: packet encoding (18 B header + typed
args), the `.noload_keep_in_elf.*` string section machinery
(`__attribute__((section))` + `__COUNTER__`), level/tag filtering, ISR-safe
path with ringbuffer, and a **sink callback API** (`on9log_add_sink`) that
delivers each encoded packet as header + payload chunks.

Integration: `components/soulcloud` registers an **MQTT sink** (start/payload/
end callbacks) that reassembles the packet and pushes it into a FreeRTOS
queue; a sender task throttles (default ≤10 msg/s, server allows 20) and
publishes to the `log` topic at QoS 0. Overflow → locally counted, emitted as
an on9log DROPPED packet. The default ESP VFS (UART/SLIP) sink stays
available for local debugging.

### 3.3 `MPack` (ludocode/mpack) — git submodule, already in place

User-provided: `components/soulcloud/external/mpack` is already added as a
git submodule inside the soulcloud component (gitlink `2789ea3`,
`v1.1.1-16-g2789ea3`, MIT). It is the MPack C library (mpack.h +
mpack-common/reader/writer/expect/platform).

Usage plan:
- `MPACK_HAS_CONFIG=1` with a component-local `mpack-config.h`: enable
  `MPACK_READER`/`MPACK_EXPECT`/`MPACK_WRITER`, **disable `MPACK_NODE`**
  (no DOM, no malloc), keep `MPACK_STDLIB` (newlib is fine on ESP-IDF).
- Decoding: `mpack_reader_t` over the payload buffer + `mpack_expect_*`
  typed reads → **streaming, zero heap allocation**. Strictness that MPack
  itself does not enforce (duplicate map keys, unknown fields, trailing
  bytes) is layered on top in `protocol.cpp` by walking keys explicitly
  (the backend applies exactly the same discipline).
- Encoding: `mpack_writer_t` initialized with a caller-owned stack/static
  buffer (no `mpack_writer_init_grow`); types are chosen explicitly
  (`mpack_write_bin` for 16-byte ids, `mpack_write_u64`/`mpack_write_int`
  for seq/code, `mpack_write_nil` for absent args/payload) so the wire
  output matches the backend codec byte-for-byte.
- Build: compile only `mpack-common.c`, `mpack-expect.c`, `mpack-platform.c`,
  `mpack-reader.c`, `mpack-writer.c` (no `mpack-node.c`).

### 3.4 `esp_mqtt` (espressif/mqtt v1.1.0) — managed component

- IDF 6.0 has no built-in MQTT client (removed; only test_apps remain), so it
  is pulled via `idf_component.yml` from the component registry
  (`espressif/mqtt`, repo github.com/espressif/esp-mqtt). **Already fetched
  and verified locally** (`managed_components/espressif__mqtt`).
- Verified capabilities: MQTT 3.1.1/5.0, QoS 0/1/2, automatic reconnection
  (`network.disable_auto_reconnect=false`, interval
  `network.reconnect_timeout_ms`), keepalive PINGREQ, QoS1 outbox with
  retransmission, TLS + cert bundle, and **MQTT over WS/WSS** (see §2.1).
- Client ID via `credentials.client_id` (MUST equal device UID and username).
- Buffer sizing: default 1024 B is too small for on9log packets (up to ~3 KB)
  — configure `session.buffer.size/out_size` via Kconfig (default 4 KiB in +
  8 KiB out; ESP32-S3 with PSRAM is fine).
- Reconnect: built-in fixed-interval reconnect plus an explicit
  `esp_mqtt_client_reconnect()` triggered from the WiFi-connected event for
  fast recovery. No hand-rolled MQTT stack.

## 4. Key design decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | Transport: **esp-mqtt over WebSocket** (`ws://host:1883/mqtt`), MQTT 3.1.1; TLS-ready via `wss://` | User decision; verified against actual esp-mqtt sources; zero backend changes. |
| D2 | MessagePack: use **MPack** (`external/mpack`, §3.3) for both encode and decode; strict wire-contract checks (duplicate keys, unknown fields, trailing bytes, exact types) live in `protocol.cpp` on top of the MPack expect API | User decision: no hand-written msgpack. MPack gives exact type control (bin/u64/nil) and zero-allocation streaming reads; the strictness layer mirrors the backend's own token-walker validation. |
| D3 | Logging: reuse **on9log component** (submodule), custom MQTT sink (§3.1) | Encoding, .noload sections, ISR path and filtering are already done and proven; only the transport changes. |
| D4 | `stat.fw` = `esp_app_get_elf_sha256()` (32 raw bytes) | Matches backend `buildId` exactly; automatically correct after OTA (reads running app descriptor); fallback: build-time generated header via CMake. |
| D5 | Config in Kconfig defaults + NVS overrides (UID, password, broker URI, API base URL, rates, periods). **Plaintext in NVS is accepted** (user decision); flash encryption is a production concern | Credential rotation must not require reflashing; runtime-updatable via a `setConfig` command. |
| D6 | Connection manager: esp-mqtt auto-reconnect + WiFi-event-triggered immediate reconnect; subscribe (cmd/exec, ota) on every `MQTT_EVENT_CONNECTED`; immediate `stat` on connect | Server only delivers commands while online; reconnect correctness is the top operational priority. |
| D7 | Command registry (`name` → handler) + QoS1 duplicate suppression (ring buffer of last 8 command ids; duplicates re-send cached result, never re-execute) | Re-execution of side-effectful commands is dangerous; server dedupes results but the device must not double-apply. |
| D8 | OTA executor: stream download → `esp_ota_begin/write/end` while computing SHA-256 → verify against `bin_sha256` → `esp_ota_set_boot_partition` → report `installed` → wait for PUBACK → restart | Firmware can be MBs; cannot buffer in RAM; checksum verified before boot switch. Commands are suspended during OTA. |
| D9 | Uplink QoS policy: `cmd/result`, `ota/result`, `stat` at QoS 1 (esp-mqtt outbox handles retransmission); `log` at QoS 0 with internal throttling | Results/stat are state; logs are telemetry. Server rate-limits uplinks, so the device throttles logs itself. |
| D10 | Demo runs plaintext `ws://` + `http://` (user decision); TLS paths (`wss://`/`https://`) supported in code but not exercised locally | Reverse proxy terminates TLS in production (per backend README). |
| D11 | Target board: **esp32-s3-devkitc-1** (8 MB flash, PSRAM). Custom partition table (see §5) | OTA requires two app partitions + otadata; NVS needs room for config. |
| D12 | Implementation language: **C++** (`.hpp`/`.cpp`), style per `sgnm` component (snake_case classes/methods/members, 4-space indent, K&R braces, `namespace soulcloud`, FreeRTOS primitives, `esp_log.h` + `static constexpr char TAG[]`). STL allowed only for constexpr usage or allocations performed once at init; **zero heap allocation on hot paths** (init/deinit are the only malloc sites; stack/static buffers elsewhere) | User decision; keeps the firmware auditable and RAM-bounded — important for a device that lives in the field. |

## 5. Project layout

```
soulcloud_client_demo/
├── partitions.csv               (new; custom partition table, see below)
├── sdkconfig.defaults           (new; target esp32s3, custom partition table)
├── components/
│   ├── on9log/                  (git submodule → github.com/huming2207/on9log @ 8c11225)
│   └── soulcloud/               (this component; submodule → esp_soulcloud_client)
│       ├── CMakeLists.txt       idf_component_register(SRCS ...)
│       │                        REQUIRES: on9log, mqtt, app_update, esp_http_client,
│       │                                  nvs_flash, log, esp_event, esp_netif
│       │                        (PRIV_REQUIRES: mbedtls)
│       │                        SRCS include external/mpack (common/expect/platform/
│       │                        reader/writer — no node)
│       ├── idf_component.yml    dependency: espressif/mqtt ^1.1.0 (managed) — DONE
│       ├── external/mpack/      (git submodule → ludocode/mpack @ 2789ea3) — DONE
│       ├── mpack-config.h       (component-local: reader/expect/writer on, node off)
│       ├── Kconfig              all tunables (§7)
│       ├── include/
│       │   └── soulcloud.hpp        public lifecycle/event/command/OTA API
│       └── src/
│           ├── soulcloud.cpp        lifecycle, esp-mqtt event wiring, connect/stat orchestration
│           ├── mqtt_bridge.cpp/.hpp esp-mqtt init/config, subscribe-on-connect, publish helpers
│           ├── protocol.cpp/.hpp    topic constants, stat/cmd/ota payload builders + strict parsers
│           ├── commands.cpp/.hpp    command registry, dispatch, duplicate cache, result encoding
│           ├── ota.cpp              OTA state machine + HTTP download + flash + hash verify
│           ├── logs.cpp/.hpp        on9log MQTT sink, TX queue, throttling, DROPPED accounting
│           └── config.cpp/.hpp      Kconfig + NVS storage, credential rotation
└── main/                       demo app (§6)
```

**Partition table (`partitions.csv`, 8 MB flash / 4 MB usable by apps):**

```
# Name,   Type, SubType, Offset,  Size,    Flags
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xf000,  0x2000,
phy_init, data, phy,     0x11000, 0x1000,
ota_0,    app,  ota_0,   0x20000, 0x380000,
ota_1,    app,  ota_1,   0x3A0000,0x380000,
```

- Two 3.5 MB app slots (ample for ESP-IDF apps with on9log + mqtt + OTA);
  NVS 24 KB (config + rotation space). `sdkconfig.defaults` sets
  `CONFIG_PARTITION_TABLE_CUSTOM=y` +
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`.
- The factory app slot is omitted: flashing the first image goes to `ota_0`
  via `idf.py -p PORT flash` (bootloader picks the ota slot; standard ESP-IDF
  OTA flow).

Public API sketch:

```c
typedef struct {
    // filled from Kconfig/NVS by soulcloud_config_load(); fields documented in §7
    char device_uid[128]; char password[128]; char broker_uri[256];
    char api_base_url[256]; uint32_t stat_interval_s; /* ... */
} soulcloud_config_t;

esp_err_t soulcloud_config_load(soulcloud_config_t *cfg);       // NVS over Kconfig
esp_err_t soulcloud_init(const soulcloud_config_t *cfg);        // NVS, esp-mqtt client, registry, log sink
esp_err_t soulcloud_start(void);                                // esp_mqtt_client_start (auto-reconnect)
esp_err_t soulcloud_stop(void);
esp_err_t soulcloud_register_command(const char *name,
        esp_err_t (*handler)(const soulcloud_command_t *cmd, soulcloud_result_t *out));
bool     soulcloud_is_connected(void);
void     soulcloud_set_connection_cb(void (*cb)(bool connected, void *ctx));
esp_err_t soulcloud_report_stat(void);                          // force immediate stat
```

Notes:
- on9log macros (`ON9_LOGI`, `ON9_LOGW`, ...) are used directly by the demo;
  the component only installs the MQTT sink at init.
- The component never touches WiFi; `main/` owns WiFi and calls
  `soulcloud_mqtt_notify_wifi_connected()` (or similar) so the client can
  trigger an immediate reconnect attempt.

## 6. Demo application (`main/`)

- `main.c`: NVS init → `soulcloud_config_load` → WiFi STA (Kconfig
  `SOULCLOUD_DEMO_WIFI_SSID/PASS`) with event handling → `soulcloud_init` +
  `soulcloud_start` → periodic demo loop.
- **Demo device provisioning (user decision)**: a demo account is hardcoded
  for now (device UID/password provisioned once in the backend database by
  SQL + the credentials endpoint); NVS overrides are still honored so
  rotation works without reflashing.
- `commands.c`: demo command handlers registered with the component:
  - `getInfo`    → `{device_uid, fw_sha256, uptime_s, reset_reason, wifi_rssi, heap_free}` (payload as single-key-map array)
  - `reboot`     → confirms, then `esp_restart()` after a short delay
  - `setLogging` → args `{enabled: bool}` (the canonical backend e2e example)
  - `setConfig`  → runtime-update UID/password/broker/API config in NVS + reconnect (credential rotation path)
  - `echo`       → echoes args back (protocol payload round-trip demo)
- Demo log stream: periodic `ON9_LOGI/ON9_LOGW/ON9_LOGE` messages (uptime/heap
  samples) demonstrating tag/fmt dictionary decoding; DROPPED packets on
  overflow; optional SNTP TIME_SYNC packet.
- Optional: status LED (blink pattern = connection state), `esp_console`
  status/trigger commands for local debugging.

## 7. Configuration items (Kconfig → NVS override)

| key | default | purpose |
|---|---|---|
| `device_uid` | `esp32-demo-0001` | MQTT username + clientId |
| `device_password` | (demo hardcoded default) | MQTT credential (issued by `POST /v1/devices/:id/credentials`) |
| `serial` | MAC hex | `stat.sn` |
| `broker_uri` | `ws://192.168.1.100:1883/mqtt` | MQTT-over-WS endpoint |
| `api_base_url` | `http://192.168.1.100:8080` | prefix for OTA `download.url` |
| `stat_interval_s` | 120 | periodic stat |
| `log_rate_per_s` | 10 | uplink log throttle (server allows 20) |
| `log_queue_len` | 32 | on9log TX queue |
| `mqtt_buffer_in/out` | 4096 / 8192 | esp-mqtt packet buffers (must fit largest on9log packet) |
| `ota_max_bytes` | 4 MiB | refuse absurd `bin_size` |
| `ota_timeout_s` | 120 | per-download watchdog |
| `mqtt_keepalive_s` | 60 | MQTT keepalive (PINGREQ) |
| `mqtt_reconnect_timeout_ms` | 10000 | esp-mqtt auto-reconnect interval |

## 8. Verification plan (against local soulcloud.js)

Staging: `docker compose up postgres`, migrations, `bun run dev` (API :8080 +
broker :1883/mqtt, **unchanged**). Demo device UID/password provisioned via
SQL insert + the credentials endpoint.

1. **Linker validation (must pass before anything else)**: build demo, inspect
   ELF — `.noload_keep_in_elf.*` sections present with stable VMA addresses,
   `--gc-sections` does not drop them; upload ELF via `POST /v1/firmware-artifacts`
   and verify dictionary extraction succeeds.
2. **Connect/auth**: wrong password → CONNACK refused and retry backoff; correct
   credentials → connected; ACL violations (publish/subscribe to foreign topics)
   → connection killed by broker.
3. **Commands**: enqueue `getInfo` via `POST /v1/command-batches` → device
   executes → `cmd/result` recorded → command state `device_completed`.
   Duplicate delivery (QoS1 replay) → cached result, no re-execution.
4. **stat**: `fw` from `esp_app_get_elf_sha256()` equals uploaded artifact
   `buildId`; backend `/devices/:id/firmware-state` returns the artifact.
5. **Logs**: device `on9log` packets ingested; `GET /devices/:id/logs` decodes
   tag/fmt/args correctly against the artifact dictionary; DROPPED accounting.
6. **OTA**: build v2 → upload release (bin+elf) → deploy to device →
   download/verify/flash/restart → new firmware reports new `stat.fw` → target
   reaches `completed` in `GET /ota-jobs/:id`. Failure path: corrupt bin hash →
   `failed` with `-2`.
7. **Resilience**: broker restart, WiFi drop, credential rotation via
   `setConfig` + server-side revoke → reconnect and command delivery resume.
8. Clean build with `idf.py build`; the project sets `MINIMAL_BUILD ON`, so
   all component dependencies must be declared explicitly (see §5).

## 9. Open items / risks

- **R2 (resolved — backend TODO, see docs/logging.md)**: IDF 6.0's linker
  keeps on9log strings (built-in `.noload (INFO)` + `KEEP`) but names the
  merged output section `.noload`; the backend matcher only accepts
  `.noload_keep_in_elf*`. Fix = relax the matcher in `artifact.ts`
  (user-approved direction, not yet done — **remind the user**). Device
  side needs no changes.
- **R3 (accepted)**: no self-service device provisioning API on the backend
  yet; demo uses a hardcoded account provisioned via SQL + credentials
  endpoint.
- **R4 (accepted)**: MQTT password and settings stored plaintext in NVS
  (partition table provides room; flash encryption deferred to production).
- **R5 (accepted)**: demo defaults to plaintext `ws://`/`http://`; TLS paths
  supported but not exercised locally.
- **R6 (accepted)**: target board esp32-s3-devkitc-1 (8 MB flash). If a
  different flash size is used, `partitions.csv` sizes must be revisited.
- **R7 (accepted)**: on9log as git submodule pinned to `8c11225` (the commit
  used by on9log_demo). Note: `on9log` repo submodule `external/fmt` is only
  needed for plain-text mode; ESP binary mode does not build it.
- **R8 (new)**: IDF 6.0 requires the `WS_TRANSPORT` Kconfig for
  `MQTT_TRANSPORT_WEBSOCKET`; defaults are `y`/`y` in both components, but
  verify in the final `sdkconfig` (record in `sdkconfig.defaults` if needed).
- **R9 (new)**: nested submodules — `components/soulcloud` contains
  `external/mpack` and the demo repo references both. Clone/CI must use
  `--recurse-submodules`; the soulcloud component's own `.gitmodules`
  already lists mpack.
- **R10 (new)**: commit timestamps. Every commit in this project (demo repo
  and the soulcloud submodule) is stamped manually with
  `GIT_AUTHOR_DATE`/`GIT_COMMITTER_DATE` — any time at/after 19:03:21
  (+1000), monotonically increasing across commits. No git aliases or hooks
  are installed.

## 10. E2E testing on QEMU (see docs/e2e-qemu.md)

Espressif's QEMU fork emulates the ESP32-S3 (CPU, flash/PSRAM MMU, crypto
HW, OpenETH Ethernet; **no Wi-Fi**). The demo now has an Ethernet path
(`sdkconfig.defaults.eth`) and the `.noload` backend fix is landed, so the
full flow — connect/auth, commands, stat, on9log ingestion, OTA — can run
against the local soulcloud.js backend in QEMU (slirp gateway `10.0.2.2`).
Quick start + hardware/CI notes: docs/SETUP.md, docs/e2e-qemu.md.

## 11. Suggested build order

1. ~~Project scaffolding~~ — done (partitions.csv, sdkconfig.defaults,
   on9log + mpack submodules, MPack build).
2. ~~protocol layer on MPack~~ — done, host tests (91 checks) green.
3. ~~mqtt_bridge + stat/cmd/ota + command loop~~ — done in code; end-to-end
   run pending hardware/QEMU (docs/SETUP.md).
4. ~~logs.cpp on9log MQTT sink~~ — done; `.noload` backend fix landed
   (soulcloud.js 4352fa2).
5. ~~ota.cpp~~ — done in code; OTA E2E pending QEMU/hardware.
6. Polish: rate limiting, duplicate suppression, failure reporting — done
   (throttle, QoS1 dup cache, failure codes); remaining: field validation
   passes, E2E runs, CI.

Commit timestamps: any time ≥ 19:03:21 (+1000) today, monotonically
increasing, set via `GIT_AUTHOR_DATE`/`GIT_COMMITTER_DATE` (see R10).
