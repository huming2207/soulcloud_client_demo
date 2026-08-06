# E2E testing on QEMU (esp32s3)

Proposal: run the full device flow — MQTT connect, commands, on9log
ingestion, OTA — against the local soulcloud.js backend inside Espressif's
QEMU fork, without physical hardware; later reuse the same flow in CI.

## QEMU support status (espressif/qemu fork, esp-develop)

Source: https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md

| Feature | ESP32-S3 | Notes |
|---|---|---|
| Dual-core CPU, UART, NOR flash (+MMU), PSRAM OPI/MMU, eFuse, RNG, GDMA, AES, SHA, RSA, HMAC, DS, SysTimer, Timer Groups, TWAI | ✅ | crypto HW works — OTA SHA-256 verify runs natively |
| Ethernet (OpenCores, `open_eth`) | ✅ | **not real hardware**; the only way to get networking in QEMU |
| Wi-Fi / Bluetooth / USB / I2C / I2S / RMT / GPIO matrix | ❌ | not emulated |
| flash size | 2 / 4 / 8 / 16 MB | our 8 MB partition table is fine |

`qemu-system-xtensa -machine esp32s3`; prebuilt binaries for macos-arm64
etc. via `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`.

## Network design (the key piece)

- No Wi-Fi in QEMU → the demo app needs an **Ethernet path**: enable
  `CONFIG_ETH_USE_OPENETH` and init `esp_eth_mac_new_openeth` (pattern:
  `examples/common_components/protocol_examples_common/connect.c`).
  Add a Kconfig switch to `demo_app` (wifi | ethernet).
- QEMU user-mode networking (slirp): `-nic user,model=open_eth`.
- The emulated device reaches the **host** (where postgres/api/broker run)
  at the slirp gateway `10.0.2.2` → configure:
  - `broker_uri = ws://10.0.2.2:1883/mqtt`
  - `api_base_url = http://10.0.2.2:8080`
- No hostfwd needed (the device is a client; hostfwd is only for guest
  listeners).

## Local run flow

1. Start the backend: `docker compose up -d --wait postgres`, migrations,
   `bun run dev` (API :8080 + broker :1883).
2. Provision the demo device (SQL insert + `POST /v1/devices/:id/credentials`
   to issue the password).
3. Bake the device config into the flash image:
   - simplest: set the demo values via `sdkconfig.defaults`
     (uid/password/broker/api) for the QEMU build, or
   - pre-provision the NVS partition (`nvs_partition_gen.py` + `merge-bin`),
     which also tests the real NVS-override path.
4. Build + merge the flash image:
   ```sh
   idf.py build
   idf.py merge-bin -o build/qemu/flash_image.bin --fill-flash-size 8MB @build/flash_args
   ```
   (the app already flashes into `ota_0` — the OTA slot — so both first boot
   and the OTA update path boot the same way)
5. Run:
   ```sh
   qemu-system-xtensa -nographic -machine esp32s3 \
     -drive file=build/qemu/flash_image.bin,if=mtd,format=raw \
     -nic user,model=open_eth \
     -serial stdio
   ```
   (or `idf.py qemu monitor` once the qemu tool is installed)

## E2E assertions (scripted)

**Primary runner: pytest-embedded suite in `tests/e2e/`** (see CI below) —
session fixtures bring up the backend (postgres + api + broker), provision
a fresh device, build the Ethernet firmware into `build/e2e-pytest/`
(private SDKCONFIG; the normal build tree is untouched), and each test
boots a fresh QEMU and asserts one flow:

```sh
python3 -m venv /tmp/e2e-venv
/tmp/e2e-venv/bin/pip install -r tests/e2e/requirements.txt
/tmp/e2e-venv/bin/pytest tests/e2e -v
```

Options/env: `SOULCLOUD_BACKEND_DIR`, `QEMU_BIN`, `ON9LOG_BIN`,
`ON9LOG_ELF`, `E2E_UID`, `E2E_REUSE_BACKEND=1` (reuse an externally
managed api/broker).

The device console carries both ESP_LOG text and the on9log SLIP stream;
`scripts/qemu-on9log-wrap.sh` pipes QEMU stdout through the on9log host
decoder (`on9log --log-stdin --elf app.elf`), so the harness reads plain
text (SLIP frames decoded, ESP_LOG passed through).

(provisions, builds v1/v2, boots QEMU, asserts everything in one go;
`--skip-ota`, `--skip-resilience`, `--keep-running`).

1. **Connect/auth**: device appears in the broker; wrong password → refused
   + backoff.
2. **Commands**: `POST /v1/command-batches` `getInfo` → device answers →
   command state `device_completed` (query via DB or API).
3. **stat**: `/devices/:id/firmware-state` shows `fw` == uploaded artifact
   `buildId` (elf sha256).
4. **Logs**: device `ON9_LOGI` packets ingested; `GET /devices/:id/logs`
   decodes tag/fmt/args against the artifact dictionary.
5. **OTA**: build v2 → upload release (bin+elf) → `POST .../deploy` → device
   downloads, verifies SHA-256, flashes ota_1, restarts → new `stat.fw` →
   target `completed` in `GET /ota-jobs/:id`.
6. **Resilience**: kill/restart broker while the device runs → reconnect +
   command delivery resumes.

## CI (GitHub Actions)

`.github/workflows/e2e-qemu.yml` runs the pytest suite on `ubuntu-latest`:

- checkout (recursive submodules) → `oven-sh/setup-bun` →
  `espressif/install-esp-idf-action` (IDF 6.0.2) →
  `idf_tools.py install qemu-xtensa`
- clones `soulcloud.js` (needs the `.flash.rodata` tag-extraction fix,
  commit ≥ ccc7bce — fail-fast check in the workflow), `bun install`,
  `docker compose up -d --wait postgres`, `db:generate` + `db:deploy`
- `python3 -m venv` + `pip install -r tests/e2e/requirements.txt`
- `SOULCLOUD_BACKEND_DIR` points at the clone and
  `pytest tests/e2e -v` runs the suite; on failure the backend logs and
  `/tmp/pytest-embedded/**/*.log` (device serial) are uploaded as the
  `e2e-logs` artifact.

Known quirks (all handled by the script/workflow):

- QEMU's emulated PSRAM is **Quad SPI** (APS6404) → the Ethernet build
  forces `CONFIG_SPIRAM_MODE_QUAD`; the devkit's Octal PSRAM stays in the
  WiFi (hardware) build.
- QEMU's openeth model only answers MII for **PHY address 1**.
- `CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64` (IDF's `(len+1)/2` hex conversion
  halves len=32, so 64 is required for the full 32-byte `stat.fw`).
- OpenETH RX pool is raised to 32 buffers (`ETH_OPENETH_DMA_RX_BUFFER_NUM`)
  or large OTA downloads drop frames.

## TODOs

- [x] demo_app: Ethernet (OpenETH) network path + Kconfig switch
- [x] backend `.noload` matcher fix (soulcloud.js 4352fa2, docs/logging.md)
- [x] device provisioning helper: `scripts/provision-device.sh`
- [x] pytest-embedded suite (`tests/e2e/`) with per-flow test cases:
      connect/stat, command, log decode, OTA, resilience — local and
      GitHub Actions green (2026-08-06); a bash-only prototype runner
      was removed once the suite landed
- [x] CI workflow: `.github/workflows/e2e-qemu.yml` (needs soulcloud.js
      ≥ ccc7bce).
