# Logging: on9log integration

## Architecture

```
on9log core (components/on9log, submodule @ 8c11225)
  ├─ encodes packets (18-byte header + typed args, .noload string addresses)
  ├─ filters (level / tag), ISR ringbuffer path
  └─ dispatch to registered sinks (start / payload / end callbacks)
       ├─ on9log_esp_vfs (UART console + SLIP)  — local debugging / host decoder
       └─ soulcloud log_sender                  — MQTT `log` topic, QoS 0, throttled
```

Both sinks run in parallel in every build: the VFS sink emits the
SLIP-encoded stream on the console UART (decode it with the on9log host
tool, github.com/huming2207/on9log_host), and `log_sender` forwards the
same packets to the `log` topic.

- `log_sender` (see `components/soulcloud/src/logs.cpp`) is a
  **producer/consumer pair over a FreeRTOS ring buffer**
  (`SOULCLOUD_LOG_RB_SIZE`, default 16 KiB, PSRAM):
  - producer: the on9log sink callbacks (running on the log source's
    task) reassemble each packet under a static mutex and enqueue it with
    a zero-tick wait — a full ring buffer drops the packet (logs are
    lossy telemetry). No MQTT work happens on the producer side.
  - consumer: the soulcloud core task polls the ring buffer with
    zero-tick receives on a 1-tick interval (empty == NULL return, no
    signalling needed) and publishes to
    `soulcloud/v1/devices/{uid}/log` at QoS 0, throttled to
    `SOULCLOUD_LOG_RATE_PER_S` (default 10 msg/s; the server guard allows
    20/s). Packets over the limit are dropped silently.
- One MQTT `log` message == one complete on9log packet (no SLIP on the MQTT
  path). `payload_len` must equal the actual payload length (backend
  enforces `packet.length == 18 + payload_len`).
- on9log core emits its own DROPPED packets (ISR ringbuffer overflow) through
  the same sink; the MQTT-side throttle drops are transport policy and are
  not reported as DROPPED packets.

### QEMU / E2E: console decoded by the host tool

In the QEMU harness the emulated console carries ESP_LOG text interleaved
with the SLIP stream. `scripts/qemu-on9log-wrap.sh` pipes QEMU's stdout
through `on9log --log-stdin --elf app.elf` (the on9log host decoder):
SLIP frames are decoded to readable log lines against the firmware ELF
and non-frame bytes (ESP_LOG text) pass through verbatim, so the harness
matches plain text only.
- One MQTT `log` message == one complete on9log packet (no SLIP on the MQTT
  path). `payload_len` must equal the actual payload length (backend
  enforces `packet.length == 18 + payload_len`).
- on9log core emits its own DROPPED packets (ISR ringbuffer overflow) through
  the same sink; the MQTT-side throttle drops are transport policy and are
  not reported as DROPPED packets.

## String dictionary (server side)

The backend extracts tag/format strings from the uploaded firmware ELF
(`packages/core/src/logging/artifact.ts`):

- strings in sections named `.noload_keep_in_elf*` → format (contains `%`/`{`)
  or tag
- strings in allocated read-only sections matching
  `^\.(rodata|data\.rel\.ro|rodata\.str|srodata)` → tags only

on9log macros place format strings in `.noload_keep_in_elf.<N>` input
sections (`__attribute__((section(...)))`); the device only ever transmits
the *addresses* (ELF VMAs), never the strings.

## ✅ Resolved (2026-08-05, soulcloud.js 4352fa2)

**Problem:** ESP-IDF 6.0's linker script already supports on9log sections
(`sections.ld.in` has `.noload 0 (INFO)` with
`KEEP(*(.noload_keep_in_elf .noload_keep_in_elf.*))` — verified in
`build/esp-idf/esp_system/ld/sections.ld`), but it **merges the input
sections into an output section named `.noload`**.

- Verified: `readelf -S hello_world.elf` shows `[.noload] PROGBITS addr 0,
  size 0x45` containing the demo format strings.
- The backend matcher only accepted `sec.name.startsWith(".noload_keep_in_elf")`,
  so `POST /v1/firmware-artifacts` extracted no formats from IDF 6.0
  builds → on9log log decoding could not resolve format strings.

**Fix (landed):** `packages/core/src/logging/artifact.ts` now also accepts
`sec.name === ".noload"` as a no-load string section; `buildNoloadElf()`
gained an optional section-name parameter; `logging.test.ts` covers the
merged `.noload` layout. Device side needed no changes (addresses are
identical; the section is kept by `KEEP`).

**Why not fix on the device side:** a custom `-T` linker script fragment
cannot pre-empt the built-in `.noload` output section (later scripts cannot
re-claim input sections already collected by an earlier output section
definition), and renaming the on9log macros would mean forking the on9log
submodule.
