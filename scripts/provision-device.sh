#!/usr/bin/env bash
#
# Provisions a demo device on the soulcloud backend:
#   1. registers a human user (personal project is created automatically)
#   2. creates the device row directly in PostgreSQL (no device-create API yet)
#   3. issues MQTT credentials through POST /v1/devices/:id/credentials
#      (the password is returned once and replaces the placeholder hash)
#
# Usage: scripts/provision-device.sh <device_uid> [username]
#
# Env:
#   DATABASE_URL  default postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud
#   API_URL       default http://127.0.0.1:8080
#   PSQL_CMD      psql invocation (default: docker exec into the compose
#                 container; set to plain `psql` for a local server)
set -euo pipefail

cd "$(dirname "$0")/.."

DEVICE_UID="${1:?usage: provision-device.sh <device_uid> [username]}"
USERNAME="${2:-provision-$(date +%s)}"
PASSWORD="provision-password-$(date +%s)"
EMAIL="${USERNAME}@example.com"

DATABASE_URL="${DATABASE_URL:-postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud}"
API_URL="${API_URL:-http://127.0.0.1:8080}"
PSQL_CMD="${PSQL_CMD:-docker exec -i soulcloudjs-postgres-1 psql -U soulcloud -d soulcloud}"

run_psql() {
  # shellcheck disable=SC2086
  bash -c "$PSQL_CMD" "$@"
}

json_field() {
  # json_field <json> <field> -> prints the string value
  python3 -c "import json,sys; print(json.load(sys.argv[1])[sys.argv[2]])" "$1" "$2"
}

echo "== registering user $USERNAME =="
REG=$(curl -fsS -X POST "$API_URL/v1/auth/register" \
  -H 'content-type: application/json' \
  -d "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD\",\"email\":\"$EMAIL\"}")
USER_ID=$(json_field "$REG" user_id)
TOKEN=$(json_field "$REG" access_token)
echo "user $USER_ID registered"

echo "== locating personal project =="
PROJECT_ID=$(echo "SELECT project_id FROM user_projects WHERE user_id = '$USER_ID';" \
  | run_psql -t -A | head -1)
[ -n "$PROJECT_ID" ] || { echo "no personal project found" >&2; exit 1; }
echo "project $PROJECT_ID"

echo "== creating device $DEVICE_UID =="
DEVICE_ID=$(echo "INSERT INTO devices (id, device_uid, assigned_id, password_hash, project_id)
  VALUES (gen_random_uuid(), '$DEVICE_UID', '$DEVICE_UID', 'pending-rotation', '$PROJECT_ID')
  RETURNING id;" | run_psql -t -A | head -1)
[ -n "$DEVICE_ID" ] || { echo "device insert failed (uid may already exist?)" >&2; exit 1; }
echo "device $DEVICE_ID"

echo "== issuing MQTT credentials =="
CRED=$(curl -fsS -X POST "$API_URL/v1/devices/$DEVICE_ID/credentials" \
  -H "authorization: Bearer $TOKEN")
MQTT_USER=$(json_field "$CRED" mqtt_username)
MQTT_PASS=$(json_field "$CRED" mqtt_password)

echo
echo "==================== device provisioning complete ===================="
echo "  device_uid     (MQTT username + client id): $MQTT_USER"
echo "  mqtt password  (shown once, store it):      $MQTT_PASS"
echo "  device_id (DB):                             $DEVICE_ID"
echo "  project_id:                                 $PROJECT_ID"
echo "  user:                                       $USERNAME"
echo
echo "Fill these into sdkconfig (or NVS via the setConfig command):"
echo "  CONFIG_SOULCLOUD_DEVICE_UID=\"$MQTT_USER\""
echo "  CONFIG_SOULCLOUD_DEVICE_PASSWORD=\"$MQTT_PASS\""
echo "======================================================================"
