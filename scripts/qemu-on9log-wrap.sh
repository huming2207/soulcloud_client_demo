#!/bin/sh
#
# QEMU wrapper for the E2E harness: the emulated console (UART0) carries
# both ESP_LOG text and the on9log SLIP byte stream. The harness expects
# plain text, so QEMU's stdout is piped through the on9log host decoder
# (github.com/huming2207/on9log_host): SLIP frames are decoded against
# the firmware ELF, non-frame bytes (ESP_LOG text) pass through verbatim.
#
# Env:
#   QEMU_BIN     qemu-system-xtensa path (auto-detected if unset)
#   ON9LOG_BIN   on9log host decoder binary (required)
#   ON9LOG_ELF   firmware ELF to resolve strings against (required)

set -e

QEMU="${QEMU_BIN:-}"
if [ -z "$QEMU" ]; then
    for d in "$HOME/.espressif/tools/qemu-xtensa" \
             "${IDF_TOOLS_PATH:-}/tools/qemu-xtensa" \
             "/opt/esp/tools/qemu-xtensa"; do
        QEMU="$(find "$d" -name qemu-system-xtensa -type f 2>/dev/null | head -1)"
        [ -n "$QEMU" ] && break
    done
fi
[ -n "$QEMU" ] || { echo "qemu-system-xtensa not found" >&2; exit 1; }
: "${ON9LOG_BIN:?ON9LOG_BIN not set}"
: "${ON9LOG_ELF:?ON9LOG_ELF not set}"

exec "$QEMU" "$@" 2>&1 | "$ON9LOG_BIN" --log-stdin --elf "$ON9LOG_ELF"
