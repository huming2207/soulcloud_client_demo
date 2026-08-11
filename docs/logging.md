# Logging: on9log integration

## Architecture

```
on9log core (components/on9log, submodule @ 8c11225)
  ├─ encodes packets (18-byte header + typed args, .noload string addresses)
  ├─ filters (level / tag), ISR ringbuffer path
  └─ dispatch to registered sinks (start / payload / end callbacks)
       ├─ on9log_esp_vfs (UART console + SLIP)  — local debugging / host decoder
       └─ soulcloud log_sender                  — MQTT `log` topic, QoS 0,
                                                 throttled, optional batching
```

Both sinks run in parallel in every build: the VFS sink emits the
SLIP-encoded stream on the console UART (decode it with the on9log host
tool, github.com/huming2207/on9log_host), and `log_sender` forwards the
same packets to the `log` topic.

- `log_sender` (see `components/soulcloud/src/logs.cpp`) is a
  **producer/consumer pair over a FreeRTOS ring buffer**
  (`log_rb_size`, default 16 KiB):
  - producer: the on9log sink callbacks (running on the log source's
    task) reassemble each packet under a static mutex and enqueue it with
    a zero-tick wait — a full ring buffer drops the packet (logs are
    lossy telemetry). No MQTT work happens on the producer side.
  - consumer: a successful enqueue notifies the event-driven soulcloud
    core task; it drains with zero-tick receives and publishes to
    `soulcloud/v1/devices/{uid}/log` at QoS 0, throttled to
    `log_rate_per_s` (default 10 packet tokens/s; the server guard
    allows 20/s with burst 100). Packets wait in the ring buffer until
    rate credit is available. While disconnected the consumer does not
    drain the ring buffer, so nothing is lost to a reconnect blip.
- One MQTT `log` message is either one raw on9log packet (first byte
  `0x9a`) or, in batching mode, an aggregated container (first byte
  `0x01`, a MessagePack array of on9log packets — see
  `soulcloudjs/docs/PROTOCOL.log-packaging.md`). No SLIP on the MQTT
  path. `payload_len` must equal the actual payload length (backend
  enforces `packet.length == 18 + payload_len`).
- on9log core emits its own DROPPED packets (ISR ringbuffer overflow) through
  the same sink; the MQTT-side queue and publish failures are tracked by the
  transport-level WARN below.
- **Drop visibility**: packets dropped device-side (ring buffer full or
  publish failure) accumulate in a counter; while connected,
  a WARN packet is emitted through on9log at most once per second so the
  backend can tell "device logged nothing" from "device dropped logs".

## Batching mode (log aggregation)

By default (`log_batch_count` = 1) every packet is published immediately
as a raw `0x9a` message — lowest latency, matching the platform's
realtime log stream. Set `log_batch_count` > 1 to accumulate packets and
publish them as one `0x01` container:

| Config | NVS key | Range | Default | Meaning |
| --- | --- | --- | --- | --- |
| `log_batch_count` | `batch_cnt` | 1..4096 | 1 | Packets per publish; 1 = disabled (raw single-packet mode) |
| `log_batch_timeout_ms` | `batch_to` | 0..60000 | 0 | Force-flush a non-empty batch this long after it started; 0 = no timer |
| `log_rb_size` | `rb_size` | 1024..262144 | 16384 | Log ring buffer size in bytes |
| `log_rb_internal` | `rb_int` | 0/1 | 1 | 1 = internal SRAM, 0 = PSRAM (internal RAM is always safe during OTA flash writes, which suspend the cache) |
| `log_rb_flush_at` | `rb_flush` | 256..rb_size | 8192 | Flush the pending batch when free space drops below this (bytes) |

The batch (4 KiB static buffer, internal RAM) flushes on **any** of:

1. `log_batch_count` packets accumulated,
2. 80 elements (the backend charges one rate token per ELEMENT with a
   shared 100-token burst; 20 tokens remain for control uplinks) or
   the 4 KiB byte budget,
3. `log_batch_timeout_ms` elapsed since the first packet of the batch
   (no upload happens if the batch is empty),
4. the ring buffer's free space drops below `log_rb_flush_at`
   (backpressure).

The device mirrors the backend token accounting: a raw packet costs one
token and a container costs one token per element. Its log bucket has an
80-token burst, leaving 20 tokens in the backend's shared 100-token bucket
for stat/command/OTA results. Device refill is `log_rate_per_s` (default
10), below the backend default of 20. The hard container cap is 80. QoS stays
0: log uplink is best-effort telemetry (drops are
counted and surfaced via the WARN), and QoS 1 with a persistent session
was observed to queue unacknowledged messages and destabilise the
connection on slow links. The packaging doc's "QoS 1" line is
internally inconsistent (it also says "exactly like a raw packet",
which is QoS 0) and is treated as a doc bug.

### QEMU / E2E: console decoded by the host tool

In the QEMU harness the emulated console carries ESP_LOG text only (the
eth build skips the on9log VFS sink; real devices keep the dual
output). `scripts/qemu-on9log-wrap.sh` pipes QEMU's stdout through the
on9log host decoder, which passes non-SLIP bytes through verbatim, so
the harness matches plain text only.

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
