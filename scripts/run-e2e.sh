#!/usr/bin/env bash
#
# E2E test runner: runs the full Soulcloud device flow — MQTT connect/auth,
# commands, stat.fw == buildId, on9log log decode, OTA, broker-restart
# resilience — against the local soulcloud.js backend inside Espressif's
# ESP32-S3 QEMU (no hardware needed).
#
# The script owns every process it starts (backend API/broker, QEMU) and
# cleans up on exit. It builds into build/e2e/ with a private SDKCONFIG so
# your normal build tree and sdkconfig are never touched.
#
# Usage:
#   scripts/run-e2e.sh [--skip-ota] [--skip-resilience] [--keep-running]
#
# Env:
#   SOULCLOUD_BACKEND_DIR   path to the soulcloudjs repo
#                           (default: <repo-root>/../soulcloudjs)
#   QEMU_BIN                qemu-system-xtensa binary
#                           (default: auto-detect under ~/.espressif)
#   E2E_UID                 device UID (default: qemu-e2e-<epoch>)
#   E2E_VERBOSE=1           dump QEMU/backend logs on failure
#
# Exit status: 0 = all assertions passed, 1 = failed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BACKEND_DIR="${SOULCLOUD_BACKEND_DIR:-$(cd "$ROOT_DIR/.." && pwd)/soulcloudjs}"
E2E_DIR="$ROOT_DIR/build/e2e"
E2E_UID="${E2E_UID:-qemu-e2e-$(date +%s)}"

API_URL="http://127.0.0.1:8080"
DATABASE_URL="${DATABASE_URL:-postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud}"
DEVICE_ID=""          # filled after provisioning
PROJECT_ID=""         # filled after provisioning
ACCESS_TOKEN=""       # filled after provisioning
API_PID=""
BROKER_PID=""
QEMU_PID=""
OTA_MARKER_APPLIED=0  # 1 = main.cpp was patched for the v2 build

SKIP_OTA=0
SKIP_RESILIENCE=0
KEEP_RUNNING=0
for arg in "$@"; do
    case "$arg" in
        --skip-ota) SKIP_OTA=1 ;;
        --skip-resilience) SKIP_RESILIENCE=1 ;;
        --keep-running) KEEP_RUNNING=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

log()  { printf '\033[1;34m[e2e]\033[0m %s\n' "$*"; }
pass() { printf '\033[1;32m[PASS]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*"; }

die() {
    fail "$*"
    if [ "${E2E_VERBOSE:-0}" = "1" ]; then
        echo "----- qemu.serial tail -----"
        tail -50 "$E2E_DIR/qemu.serial" 2>/dev/null || true
        echo "----- backend logs tail -----"
        tail -30 "$E2E_DIR/api.log" 2>/dev/null || true
        tail -30 "$E2E_DIR/broker.log" 2>/dev/null || true
    fi
    exit 1
}

# wait_for <desc> <timeout_s> <cmd...>: polls until cmd succeeds.
wait_for() {
    local desc="$1" timeout="$2"; shift 2
    local deadline=$((SECONDS + timeout))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if "$@" >/dev/null 2>&1; then
            pass "$desc"
            return 0
        fi
        sleep 2
    done
    die "timed out after ${timeout}s: $desc"
}

# api_get <path> -> body (authorized)
api_get() {
    curl -fsS "$API_URL$1" -H "authorization: Bearer $ACCESS_TOKEN"
}

cleanup() {
    set +e
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    [ -n "$API_PID" ] && kill "$API_PID" 2>/dev/null
    [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2>/dev/null
    if [ "$OTA_MARKER_APPLIED" = "1" ]; then
        git -C "$ROOT_DIR" checkout -- main/main.cpp
    fi
    [ "$KEEP_RUNNING" = "1" ] && log "processes left running (--keep-running)"
}
trap cleanup EXIT

# ------------------------------------------------------------------ #
# 0. prerequisites
# ------------------------------------------------------------------ #
command -v bun >/dev/null || die "bun not found (backend needs it)"
command -v docker >/dev/null || die "docker not found (postgres)"
command -v python3 >/dev/null || die "python3 not found"
[ -d "$BACKEND_DIR" ] || die "backend dir not found: $BACKEND_DIR (set SOULCLOUD_BACKEND_DIR)"
grep -q 'flash\.rodata' "$BACKEND_DIR/packages/core/src/logging/artifact.ts" \
    || die "backend missing the .flash.rodata tag-extraction fix (soulcloudjs >= ccc7bce)"

if [ -z "${QEMU_BIN:-}" ]; then
    # local installs land in ~/.espressif; the CI action installs under
    # /opt/esp (IDF_TOOLS_PATH)
    for dir in "$HOME/.espressif/tools/qemu-xtensa" \
               "${IDF_TOOLS_PATH:-}/tools/qemu-xtensa" \
               "/opt/esp/tools/qemu-xtensa"; do
        QEMU_BIN="$(find "$dir" -name qemu-system-xtensa -type f 2>/dev/null | head -1 || true)"
        [ -n "$QEMU_BIN" ] && break
    done
fi
[ -n "$QEMU_BIN" ] || die "qemu-system-xtensa not found; install via: python \$IDF_PATH/tools/idf_tools.py install qemu-xtensa"
log "QEMU: $QEMU_BIN"

if [ -z "${IDF_PATH:-}" ]; then
    IDF_PATH="$HOME/esp/esp-idf"
fi
# CI runners (espressif/install-esp-idf-action) already have idf.py on
# PATH and their export.sh may fail on optional tools (esp-rom-elfs);
# only source when idf.py is missing (local dev shells).
if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1090
    source "$IDF_PATH/export.sh" >/dev/null 2>&1 \
        || log "warn: cannot source $IDF_PATH/export.sh; relying on PATH"
fi
command -v idf.py >/dev/null || die "idf.py not found (source \$IDF_PATH/export.sh first)"
command -v esptool.py >/dev/null || die "esptool.py not found (source \$IDF_PATH/export.sh first)"

git -C "$ROOT_DIR" diff --quiet -- main/main.cpp \
    || die "main/main.cpp has uncommitted changes; commit or stash them (the OTA step patches and restores it)"

mkdir -p "$E2E_DIR"

# ------------------------------------------------------------------ #
# 1. backend: postgres + migrate + api + broker
# ------------------------------------------------------------------ #
log "starting backend ($BACKEND_DIR)"
if ! curl -fsS -m 2 "$API_URL/health/ready" >/dev/null 2>&1; then
    (cd "$BACKEND_DIR" && bun install >/dev/null)
    (cd "$BACKEND_DIR" && docker compose up -d --wait postgres)
    # fresh clones have no prisma client yet; generate before migrate
    (cd "$BACKEND_DIR" && DATABASE_URL="$DATABASE_URL" bun run db:generate >/dev/null)
    (cd "$BACKEND_DIR" && DATABASE_URL="$DATABASE_URL" bun run db:deploy >/dev/null)
    JWT_SECRET="${JWT_SECRET:-$(openssl rand -base64 48)}"
    export JWT_SECRET DATABASE_URL
    cd "$BACKEND_DIR"
    nohup bun run start:api >"$E2E_DIR/api.log" 2>&1 </dev/null &
    echo $! > "$E2E_DIR/api.pid"
    nohup bun run start:broker >"$E2E_DIR/broker.log" 2>&1 </dev/null &
    echo $! > "$E2E_DIR/broker.pid"
    cd "$ROOT_DIR"
    API_PID="$(cat "$E2E_DIR/api.pid")"
    BROKER_PID="$(cat "$E2E_DIR/broker.pid")"
    wait_for "api /health/ready" 60 curl -fsS -m 2 "$API_URL/health/ready"
    wait_for "broker ws port 1883" 30 bash -c "exec 3<>/dev/tcp/127.0.0.1/1883"
    sleep 1
else
    log "backend already running; reusing it"
fi

# ------------------------------------------------------------------ #
# 2. provision the device (unique UID per run)
# ------------------------------------------------------------------ #
log "provisioning device $E2E_UID"
PROV="$("$SCRIPT_DIR/provision-device.sh" "$E2E_UID" 2>&1)"
E2E_PASSWORD="$(echo "$PROV" | sed -n 's/^device_password=//p' | head -1)"
DEVICE_ID="$(echo "$PROV" | sed -n 's/^device_id=//p' | head -1)"
PROJECT_ID="$(echo "$PROV" | sed -n 's/^project_id=//p' | head -1)"
ACCESS_TOKEN="$(echo "$PROV" | sed -n 's/^access_token=//p' | head -1)"
[ -n "$E2E_PASSWORD" ] && [ -n "$DEVICE_ID" ] && [ -n "$ACCESS_TOKEN" ] \
    || die "provisioning failed"
log "device_id=$DEVICE_ID project=$PROJECT_ID"

# ------------------------------------------------------------------ #
# 3. build v1 (Ethernet/OpenETH) + merge flash image
# ------------------------------------------------------------------ #
log "building firmware (v1)"
cat > "$E2E_DIR/sdkconfig.e2e" <<EOF
CONFIG_SOULCLOUD_DEVICE_UID="$E2E_UID"
CONFIG_SOULCLOUD_DEVICE_PASSWORD="$E2E_PASSWORD"
EOF
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.eth;$E2E_DIR/sdkconfig.e2e"
idf.py -B "$E2E_DIR/build" -DSDKCONFIG="$E2E_DIR/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" set-target esp32s3 >"$E2E_DIR/build-v1.log" 2>&1 \
    || die "set-target failed (see $E2E_DIR/build-v1.log)"
idf.py -B "$E2E_DIR/build" -DSDKCONFIG="$E2E_DIR/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" build >>"$E2E_DIR/build-v1.log" 2>&1 \
    || die "v1 build failed (see $E2E_DIR/build-v1.log)"
cp "$E2E_DIR/build/hello_world.bin" "$E2E_DIR/v1.bin"
cp "$E2E_DIR/build/hello_world.elf" "$E2E_DIR/v1.elf"
V1_SHA="$(sha256sum "$E2E_DIR/v1.elf" | cut -d' ' -f1)"
(cd "$E2E_DIR/build" && esptool.py --chip esp32s3 merge_bin \
    -o "$E2E_DIR/flash_image.bin" --pad-to-size 8MB @flash_args >/dev/null)
log "v1 elf sha256: ${V1_SHA:0:16}..."

# ------------------------------------------------------------------ #
# 4. boot QEMU
# ------------------------------------------------------------------ #
log "starting QEMU"
rm -f "$E2E_DIR/qemu.serial"
nohup "$QEMU_BIN" -m 8M -nographic -monitor none \
    -serial "file:$E2E_DIR/qemu.serial" \
    -machine esp32s3 \
    -drive "file=$E2E_DIR/flash_image.bin,if=mtd,format=raw" \
    -nic user,model=open_eth >"$E2E_DIR/qemu.log" 2>&1 </dev/null &
QEMU_PID=$!
wait_for "device boots (app_main)" 60 grep -q "Calling app_main" "$E2E_DIR/qemu.serial"

# ------------------------------------------------------------------ #
# 5. connect + stat.fw == buildId
# ------------------------------------------------------------------ #
log "asserting connect/auth + stat"
wait_for "mqtt connected" 60 grep -q "connected; subscribed" "$E2E_DIR/qemu.serial"
wait_for "stat.fw reported" 90 bash -c \
    "curl -fsS '$API_URL/v1/devices/$DEVICE_ID/firmware-state' -H 'authorization: Bearer $ACCESS_TOKEN' \
     | grep -q '$V1_SHA'"
pass "stat.fw matches v1 ELF buildId"

# ------------------------------------------------------------------ #
# 6. command round-trip
# ------------------------------------------------------------------ #
log "asserting command round-trip (getInfo)"
BATCH="$(curl -fsS -X POST "$API_URL/v1/command-batches" \
    -H "authorization: Bearer $ACCESS_TOKEN" -H 'content-type: application/json' \
    -d "{\"device_ids\":[\"$DEVICE_ID\"],\"command\":{\"cmd\":\"getInfo\",\"args\":[]}}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["batch_id"])')"
wait_for "command device_completed" 60 bash -c \
    "docker exec soulcloudjs-postgres-1 psql -U soulcloud -d soulcloud -t -A -c \
     \"SELECT state FROM device_commands WHERE batch_id='$BATCH';\" | grep -q device_completed"
pass "getInfo -> device_completed"

# ------------------------------------------------------------------ #
# 7. log ingestion + decode against the v1 artifact
# ------------------------------------------------------------------ #
log "asserting on9log ingestion + decode"
ART="$(curl -fsS -X POST "$API_URL/v1/firmware-artifacts" \
    -H "authorization: Bearer $ACCESS_TOKEN" \
    -F "project_id=$PROJECT_ID" -F "version=e2e-v1" \
    -F "file=@$E2E_DIR/v1.elf" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["artifact_id"])')"
wait_for "artifact backfills events" 60 bash -c \
    "docker exec soulcloudjs-postgres-1 psql -U soulcloud -d soulcloud -t -A -c \
     \"SELECT count(*) FROM raw_log_events WHERE artifact_id='$ART' AND decode_state != 'unknown_fw';\" | grep -qv '^0$'"
wait_for "decoded log with tag+message" 90 bash -c \
    "curl -fsS '$API_URL/v1/devices/$DEVICE_ID/logs?limit=5' -H 'authorization: Bearer $ACCESS_TOKEN' \
     | grep -q 'soulcloud_demo'"
pass "logs decoded (tag=soulcloud_demo)"

# ------------------------------------------------------------------ #
# 8. OTA: v2 build -> release -> deploy -> completed
# ------------------------------------------------------------------ #
if [ "$SKIP_OTA" = "1" ]; then
    log "skipping OTA (--skip-ota)"
else
    log "building firmware (v2, OTA target)"
    sed -i 's/ON9_LOGI(TAG, "tick=/ON9_LOGI(TAG, "e2e-v2-tick=/' "$ROOT_DIR/main/main.cpp"
    OTA_MARKER_APPLIED=1
    idf.py -B "$E2E_DIR/build" -DSDKCONFIG="$E2E_DIR/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" build >>"$E2E_DIR/build-v2.log" 2>&1 \
        || die "v2 build failed (see $E2E_DIR/build-v2.log)"
    cp "$E2E_DIR/build/hello_world.bin" "$E2E_DIR/v2.bin"
    cp "$E2E_DIR/build/hello_world.elf" "$E2E_DIR/v2.elf"

    log "uploading v2 release + deploying"
    REL="$(curl -fsS -X POST "$API_URL/v1/firmware-releases" \
        -H "authorization: Bearer $ACCESS_TOKEN" \
        -F "project_id=$PROJECT_ID" -F "version=e2e-v2" \
        -F "bin=@$E2E_DIR/v2.bin" -F "elf=@$E2E_DIR/v2.elf")"
    REL_ID="$(echo "$REL" | python3 -c 'import json,sys; print(json.load(sys.stdin)["release_id"])')"
    JOB="$(curl -fsS -X POST "$API_URL/v1/firmware-releases/$REL_ID/deploy" \
        -H "authorization: Bearer $ACCESS_TOKEN" -H 'content-type: application/json' \
        -d "{\"device_ids\":[\"$DEVICE_ID\"]}" \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["job_id"])')"

    log "waiting for OTA to complete"
    wait_for "device downloads + verifies + flashes" 180 grep -q "OTA installed" "$E2E_DIR/qemu.serial"
    pass "ota: download -> sha256 verify -> flash ota_1 -> reboot"
    wait_for "ota job completed" 120 bash -c \
        "curl -fsS '$API_URL/v1/ota-jobs/$JOB' -H 'authorization: Bearer $ACCESS_TOKEN' \
         | grep -q '\"state\":\"completed\"'"
    pass "ota job completed"
fi

# ------------------------------------------------------------------ #
# 9. resilience: broker restart -> reconnect -> commands resume
# ------------------------------------------------------------------ #
if [ "$SKIP_RESILIENCE" = "1" ]; then
    log "skipping resilience (--skip-resilience)"
else
    log "asserting broker-restart resilience"
    [ -n "$BROKER_PID" ] || die "broker not managed by this run (--keep-running?)"
    kill "$BROKER_PID"
    OLD_LINES="$(wc -l < "$E2E_DIR/qemu.serial")"
    wait_for "device disconnects after broker kill" 45 bash -c \
        "tail -n +$((OLD_LINES + 1)) '$E2E_DIR/qemu.serial' | grep -q disconnected"

    cd "$BACKEND_DIR"
    nohup bun run start:broker >"$E2E_DIR/broker.log" 2>&1 </dev/null &
    echo $! > "$E2E_DIR/broker.pid"
    cd "$ROOT_DIR"
    BROKER_PID="$(cat "$E2E_DIR/broker.pid")"
    wait_for "device reconnects" 90 bash -c \
        "tail -n +$((OLD_LINES + 1)) '$E2E_DIR/qemu.serial' | grep -q 'connected; subscribed'"

    BATCH2="$(curl -fsS -X POST "$API_URL/v1/command-batches" \
        -H "authorization: Bearer $ACCESS_TOKEN" -H 'content-type: application/json' \
        -d "{\"device_ids\":[\"$DEVICE_ID\"],\"command\":{\"cmd\":\"echo\",\"args\":[{\"msg\":\"ping\"}]}}" \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["batch_id"])')"
    wait_for "command after reconnect" 60 bash -c \
        "docker exec soulcloudjs-postgres-1 psql -U soulcloud -d soulcloud -t -A -c \
         \"SELECT state FROM device_commands WHERE batch_id='$BATCH2';\" | grep -q device_completed"
    pass "broker restart -> reconnect -> command delivered"
fi

# ------------------------------------------------------------------ #
log "E2E passed (device $E2E_UID)"
echo "----------------------------------------"
echo "  device_uid:   $E2E_UID"
echo "  device_id:    $DEVICE_ID"
[ "$SKIP_OTA" = "0" ] && echo "  ota job:      completed"
echo "  artifacts in: $E2E_DIR"
echo "----------------------------------------"
