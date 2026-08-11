"""
E2E: full Soulcloud device flow in ESP32-S3 QEMU against the local
soulcloud.js backend. Tests run in file order and share one provisioned
device + backend (session fixtures); each test boots a fresh QEMU.

    1. test_connect_and_stat   MQTT connect/auth + stat.fw == ELF buildId
    2. test_command_roundtrip  getInfo -> device_completed
    3. test_log_decode         on9log packets ingested + decoded vs artifact
    4. test_ota                v2 build -> release -> deploy -> completed
    5. test_resilience         broker restart -> reconnect -> commands resume
"""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

import pexpect
import pytest
from pytest_embedded import Dut

from conftest import wait_json  # noqa: F401  (shared helper)

REPO_ROOT = Path(__file__).resolve().parents[2]
E2E_DIR = REPO_ROOT / "build" / "e2e-artifacts"   # sdkconfig fragment, product copies (matches conftest)
BUILD_DIR = REPO_ROOT / "build" / "e2e-pytest"     # CMake build dir (pytest --build-dir)
API_URL = "http://127.0.0.1:8080"

pytestmark = pytest.mark.esp32s3


def test_connect_and_stat(dut: Dut, api, device) -> None:
    """Device boots, connects to the broker, and reports stat.fw equal to
    the built ELF's SHA-256 (the backend's artifact buildId)."""
    dut.expect(r"Calling app_main", timeout=120)
    dut.expect(r"got ip \d+\.\d+\.\d+\.\d+", timeout=60)
    dut.expect(r"downlink ready", timeout=60)

    # the stat may land a moment after the MQTT connect; poll until the
    # backend has firmware_state with our exact ELF buildId
    import json
    import time as _time
    t0 = _time.monotonic()
    state = None
    while _time.monotonic() - t0 < 180:
        try:
            raw = api.get(f"/v1/devices/{device.device_id}/firmware-state")
            state = json.loads(raw)
            print(f"[diag] fw_hash={state.get('fw_hash', '')[:16]} "
                  f"expect={device.elf_sha256[:16]} match={state.get('fw_hash') == device.elf_sha256}")
            if state.get("fw_hash") == device.elf_sha256:
                break
        except Exception as e:
            print(f"[diag] firmware-state err: {type(e).__name__} {e}")
        _time.sleep(3)
    else:
        raise AssertionError("stat.fw did not match ELF buildId within 180s")
    assert state["device_uid"] == device.uid


def test_command_roundtrip(dut: Dut, api) -> None:
    """POST /v1/command-batches getInfo -> device executes -> result 0
    recorded with state device_completed."""
    dut.expect(r"downlink ready", timeout=120)

    batch = api.send_command("getInfo")
    api.wait_command(batch, "device_completed")
    # result_code must be 0 (ok)
    import json
    rows = subprocess.run(
        ["docker", "exec", "soulcloudjs-postgres-1", "psql", "-U", "soulcloud",
         "-d", "soulcloud", "-t", "-A", "-c",
         f"SELECT result_code FROM device_commands WHERE batch_id='{batch}';"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    assert rows == "0"


def test_log_decode(dut: Dut, api, device) -> None:
    """Upload the v1 ELF as a firmware artifact; on9log log packets must
    be backfilled/associated and decode to tag + message."""
    dut.expect(r"downlink ready", timeout=120)

    art = api.post("/v1/firmware-artifacts", form={
        "project_id": device.project_id,
        "version": "e2e-v1",
        "file": device.elf,
    })["artifact_id"]

    # wait for at least one event carrying the artifact, then a decoded one
    import time
    deadline = time.monotonic() + 120
    decoded = False
    while time.monotonic() < deadline and not decoded:
        events = json.loads(api.get(f"/v1/devices/{device.device_id}/logs?limit=20"))
        for e in events["events"]:
            if e.get("decode_state") != "unknown_fw" and e.get("tag"):
                assert e["message"] is not None
                decoded = True
                break
        time.sleep(3)
    assert decoded, "no decoded log event with tag+message within 120s"


def test_ota(dut: Dut, api, device) -> None:
    """Upload the session-built v2 release (bin+elf) -> deploy -> device
    downloads, verifies SHA-256, flashes ota_1, reboots, and the job
    reaches 'completed' once the new stat.fw matches the release buildId.
    The v2 firmware is built once at session start (build_firmware), so
    no host compile runs while QEMU is active (a running compile starves
    the TCG thread on 2-core CI runners and triggers the ws reconnect
    storm)."""
    dut.expect(r"downlink ready", timeout=120)

    v2_bin = E2E_DIR / "v2.bin"
    v2_elf = E2E_DIR / "v2.elf"

    # --- upload release + deploy ---
    rel = api.post("/v1/firmware-releases", form={
        "project_id": device.project_id,
        "version": "e2e-v2",
        "bin": v2_bin,
        "elf": v2_elf,
    })
    rel_id = rel["release_id"]
    job = api.post(f"/v1/firmware-releases/{rel_id}/deploy",
                   {"device_ids": [device.device_id]})["job_id"]

    # device side: download -> verify -> flash -> reboot. The notice is
    # re-issued by the backend poller until it is acknowledged, and the
    # device may still be riding out the reconnect storm, so give it a
    # generous window.
    try:
        dut.expect(r"soulcloud_ota: OTA start", timeout=240)
    except pexpect.TIMEOUT:
        # Best-effort delivery: if the notice was lost (clean-session
        # drop during a reconnect, or a publish before the device's
        # SUBSCRIBE registered), the target stays 'delivered' with no
        # retry. Re-deploy to mint a fresh target (createOtaJob has no
        # dedupe) and wait again.
        print("OTA notice not observed; re-deploying")
        job = api.post(f"/v1/firmware-releases/{rel_id}/deploy",
                       {"device_ids": [device.device_id]})["job_id"]
        dut.expect(r"soulcloud_ota: OTA start", timeout=240)
    dut.expect(r"soulcloud_ota: sha256 verified", timeout=180)
    dut.expect(r"soulcloud_ota: OTA installed; restarting", timeout=60)
    # reboots into the new app; boot logs show the new ELF sha line
    dut.expect(r"app_init: ELF file SHA256:", timeout=120)

    # server side: job reaches completed via the new stat.fw
    import json
    deadline = 180
    import time
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline:
        data = json.loads(api.get(f"/v1/ota-jobs/{job}"))
        state = data["targets"][0]["state"]
        if state == "completed":
            break
        assert state != "failed", f"ota job failed: {data}"
        time.sleep(5)
    else:
        raise AssertionError("ota job did not reach completed in time")
    assert data["targets"][0]["result_code"] == 0


def test_resilience(dut: Dut, api, backend, device) -> None:
    """Kill the broker while the device runs: it must notice, reconnect
    after the broker comes back, and command delivery must resume."""
    if not backend.broker_pid:
        pytest.skip("broker not managed by this session (reusing an external backend)")
    dut.expect(r"downlink ready", timeout=120)

    # kill broker
    subprocess.run(["kill", str(backend.broker_pid)], check=True)
    dut.expect(r"soulcloud_demo: soulcloud disconnected", timeout=90)

    # restart broker
    import os
    env = {**os.environ, "DATABASE_URL": os.environ.get(
        "DATABASE_URL", "postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud"),
        "JWT_SECRET": os.environ.get("JWT_SECRET", "e2e-secret-" + "x" * 40)}
    with backend.broker_log.open("ab") as f:
        proc = subprocess.Popen(["bun", "run", "start:broker"],
                                cwd=Path(os.environ.get("SOULCLOUD_BACKEND_DIR",
                                                        REPO_ROOT.parent / "soulcloudjs")),
                                stdout=f, stderr=subprocess.STDOUT, env=env,
                                start_new_session=True)  # own process group for teardown
    backend.broker_pid = proc.pid

    dut.expect(r"downlink ready", timeout=120)

    # command delivery resumes
    batch = api.send_command("getInfo")
    api.wait_command(batch, "device_completed")
