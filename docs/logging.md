# Logging: on9log integration

## Architecture

```
on9log core (components/on9log, submodule @ 8c11225)
  ├─ encodes packets (18-byte header + typed args, .noload string addresses)
  ├─ filters (level / tag), ISR ringbuffer path
  └─ dispatch to registered sinks (start / payload / end callbacks)
       ├─ on9log_esp_vfs (UART + SLIP)   — local debugging
       └─ soulcloud log_sender           — MQTT `log` topic, QoS 0, throttled
```

- The component installs `log_sender` (see `components/soulcloud/src/logs.cpp`)
  as an on9log sink. It reassembles each packet under a static mutex into a
  4 KiB static buffer and publishes it to
  `soulcloud/v1/devices/{uid}/log` at QoS 0, throttled to
  `SOULCLOUD_LOG_RATE_PER_S` (default 10 msg/s; the server guard allows
  20/s). Packets over the limit are dropped silently.
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

## ⚠️ TODO (blocked on backend change — remind the user)

**Problem:** ESP-IDF 6.0's linker script already supports on9log sections
(`sections.ld.in` has `.noload 0 (INFO)` with
`KEEP(*(.noload_keep_in_elf .noload_keep_in_elf.*))` — verified in
`build/esp-idf/esp_system/ld/sections.ld`), but it **merges the input
sections into an output section named `.noload`**.

- Verified: `readelf -S hello_world.elf` shows `[.noload] PROGBITS addr 0,
  size 0x45` containing the demo format strings.
- The backend matcher only accepts `sec.name.startsWith(".noload_keep_in_elf")`,
  so **`POST /v1/firmware-artifacts` extracts no formats from IDF 6.0
  builds** → on9log log decoding will not resolve format strings.

**Chosen fix (user-approved direction):** relax the backend matcher in
`packages/core/src/logging/artifact.ts`:

```js
// current:
if (sec.name.startsWith(".noload_keep_in_elf")) { ... }
// change to also accept the IDF 6.0 merged output section:
if (sec.name.startsWith(".noload_keep_in_elf") || sec.name === ".noload") { ... }
```

plus a parser test for a non-ALLOC `.noload` section. The device side needs
no changes (addresses are identical; the section is kept by `KEEP`).

**Why not fix on the device side:** a custom `-T` linker script fragment
cannot pre-empt the built-in `.noload` output section (later scripts cannot
re-claim input sections already collected by an earlier output section
definition), and renaming the on9log macros would mean forking the on9log
submodule.
