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

1. **Connect/auth**: device appears in the broker; wrong password → refused
   + backoff.
2. **Commands**: `POST /v1/command-batches` `getInfo` → device answers →
   command state `device_completed` (query via DB or API).
3. **stat**: `/devices/:id/firmware-state` shows `fw` == uploaded artifact
   `buildId` (elf sha256).
4. **Logs**: device `ON9_LOGI` packets ingested; `GET /devices/:id/logs`
   decodes tag/fmt/args (blocked until the `.noload` matcher fix — see
   docs/logging.md).
5. **OTA**: build v2 → upload release (bin+elf) → `POST .../deploy` → device
   downloads, verifies SHA-256, flashes ota_1, restarts → new `stat.fw` →
   target `completed` in `GET /ota-jobs/:id`.
6. **Resilience**: kill/restart broker while the device runs → reconnect +
   command delivery resumes.

## CI sketch (GitHub Actions)

- Job: `ubuntu-latest`, services: postgres; steps:
  - checkout with `--recurse-submodules`
  - setup `espressif/idf` docker image (or `idf-build-apps` + manual QEMU
    install: `idf_tools.py install qemu-xtensa`)
  - install + migrate soulcloud.js backend, start api + broker
  - build demo (ethernet config), `merge-bin`
  - start QEMU with `-serial tcp::5555,server,nowait` (or pipe) and run the
    assertion script (bash or pytest).
- Alternative structured approach: `pytest-embedded` with the `@pytest.mark.qemu`
  marker (Espressif's own pattern) driving both QEMU and the backend.

## TODOs

- [x] demo_app: Ethernet (OpenETH) network path + Kconfig switch
- [x] backend `.noload` matcher fix (soulcloud.js 4352fa2, docs/logging.md)
- [x] device provisioning helper: `scripts/provision-device.sh`
- [ ] E2E runner script (start backend → qemu → assert) — see
      docs/SETUP.md for the pieces; wire-up next
- [ ] CI workflow (optional, after local E2E is green)
