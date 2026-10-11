# SoulCloud client demo

ESP32-S3 demo of the `soulcloud` component (`components/soulcloud`, its own
repository) with `on9log` for binary logging.

## Build and flash (ESP32-S3 DevKitC-1)

```sh
. ~/esp/esp-idf/export.sh
idf.py menuconfig   # SoulCloud demo: Wi-Fi, pairing token; SoulCloud client: platform URL
idf.py build flash
```

Get a pairing token from the web console (Devices, Add device). The device uses
it once, then keeps its credentials in NVS. The console output uses on9log's
framed UART transport; read it with the on9log host CLI and the firmware ELF.

Upload `build/soulcloud_demo.bin` together with `build/soulcloud_demo.elf` on the
Firmware page, so binary logs decode and the image can be rolled out by OTA.

## Run in QEMU

QEMU has no Wi-Fi or octal PSRAM; `sdkconfig.defaults.qemu` switches to the
emulated Ethernet and internal RAM, and points at the host (`10.0.2.2`). Start
the platform's dev stack with `PUBLIC_HOST=10.0.2.2` so enrollment hands out an
address QEMU can reach, then:

```sh
echo 'CONFIG_SOULCLOUD_DEMO_PAIRING_TOKEN="XXXX-XXXX-XXXX-XXXX"' > /tmp/token.defaults
idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.qemu;/tmp/token.defaults" build
idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig merge-bin -o qemu_flash.bin --fill-flash-size 8MB
qemu-system-xtensa -nographic -machine esp32s3 -drive file=build-qemu/qemu_flash.bin,if=mtd,format=raw \
  -nic user,model=open_eth -serial file:qemu-serial.bin
```

## Demo extensions (`main/main.cpp`)

- Method `demo.echo` (`{"text": "..."}`), shell command `demo ticks`.
- Status extras: Wi-Fi RSSI and a tick counter.
- An on9log line every 10 s.
