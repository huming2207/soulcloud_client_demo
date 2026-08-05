"""
E2E fixtures: backend (postgres + api + broker), device provisioning,
firmware build, and the pytest-embedded QEMU setup.

The pytest-embedded `app`/`dut` fixtures only *parse* a prebuilt binary
directory, so this conftest builds it first (session-scoped autouse
fixture, same steps the old bash runner did):

    backend -> provision -> write sdkconfig fragment -> idf.py build

Run (from the repo root, with the venv active):

    pytest tests/e2e --embedded-services idf,qemu \
        --app-path . --build-dir build/e2e-pytest \
        --qemu-extra-args="-m 8M -nic user,model=open_eth"

Env:
    SOULCLOUD_BACKEND_DIR   path to the soulcloud.js repo
    QEMU_BIN                qemu-system-xtensa binary (auto-detected)
    E2E_UID                 device UID (default qemu-e2e-<epoch>)
    DATABASE_URL            (default postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud)
    API_URL                 (default http://127.0.0.1:8080)
"""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

import pytest

# pytest-embedded shells out to esptool.py/qemu-system-xtensa; make sure
# the venv bin dir (where pip installed esptool) is on PATH even when the
# outer shell never sourced it.
import sys
_BIN = str(Path(sys.prefix) / "bin")  # sys.executable may be a symlink; prefix is the venv
if _BIN not in os.environ.get("PATH", ""):
    os.environ["PATH"] = f"{_BIN}:{os.environ.get('PATH', '')}"

REPO_ROOT = Path(__file__).resolve().parents[2]
BACKEND_DIR = Path(os.environ.get("SOULCLOUD_BACKEND_DIR", REPO_ROOT.parent / "soulcloudjs"))
E2E_DIR = REPO_ROOT / "build" / "e2e-artifacts"   # sdkconfig fragment, product copies, logs
BUILD_DIR = REPO_ROOT / "build" / "e2e-pytest"     # CMake build dir (pytest --build-dir)
API_URL = os.environ.get("API_URL", "http://127.0.0.1:8080")
DATABASE_URL = os.environ.get(
    "DATABASE_URL", "postgres://soulcloud:soulcloud@127.0.0.1:5432/soulcloud"
)
E2E_UID = os.environ.get("E2E_UID", f"qemu-e2e-{int(time.time())}")


# ------------------------------------------------------------------ #
# tiny http/json helpers (stdlib only)
# ------------------------------------------------------------------ #

def http_json(method: str, url: str, body=None, token: str | None = None, timeout: int = 30):
    req = urllib.request.Request(url, method=method)
    req.add_header("content-type", "application/json")
    if token:
        req.add_header("authorization", f"Bearer {token}")
    data = json.dumps(body).encode() if body is not None else None
    with urllib.request.urlopen(req, data, timeout=timeout) as resp:
        return json.loads(resp.read())


def http_get(url: str, token: str | None = None, timeout: int = 30) -> str:
    req = urllib.request.Request(url)
    if token:
        req.add_header("authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode()


def wait_http(url: str, timeout_s: float = 60, interval: float = 2) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            urllib.request.urlopen(url, timeout=2)
            return
        except Exception:
            time.sleep(interval)
    raise TimeoutError(f"http wait timed out: {url}")


def wait_sql(query: str, expect_re: str, timeout_s: float = 60, interval: float = 2) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        out = subprocess.run(
            ["docker", "exec", "soulcloudjs-postgres-1", "psql", "-U", "soulcloud",
             "-d", "soulcloud", "-t", "-A", "-c", query],
            capture_output=True, text=True,
        ).stdout
        if re.search(expect_re, out):
            return
        time.sleep(interval)
    raise TimeoutError(f"sql wait timed out: {query}")


def wait_json(url: str, token: str, pred, timeout_s: float = 120, interval: float = 3):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            data = json.loads(http_get(url, token))
            if pred(data):
                return data
        except Exception:
            pass
        time.sleep(interval)
    raise TimeoutError(f"json wait timed out: {url}")


# ------------------------------------------------------------------ #
# session-scoped state
# ------------------------------------------------------------------ #

@dataclass
class Backend:
    api_pid: int | None = None
    broker_pid: int | None = None
    api_log: Path = E2E_DIR / "api.log"
    broker_log: Path = E2E_DIR / "broker.log"


@dataclass
class Device:
    uid: str
    password: str
    device_id: str
    project_id: str
    token: str
    elf: Path = E2E_DIR / "v1.elf"
    elf_sha256: str = ""


@pytest.fixture(scope="session")
def backend() -> Iterator[Backend]:
    E2E_DIR.mkdir(parents=True, exist_ok=True)

    # postgres via compose (idempotent; kept running between runs)
    subprocess.run(["docker", "compose", "up", "-d", "--wait", "postgres"],
                   cwd=BACKEND_DIR, check=True)
    env = {**os.environ, "DATABASE_URL": DATABASE_URL}
    # fresh clones have no prisma client; generate before migrate
    subprocess.run(["bun", "run", "db:generate"], cwd=BACKEND_DIR, env=env,
                   capture_output=True, check=True)
    subprocess.run(["bun", "run", "db:deploy"], cwd=BACKEND_DIR, env=env,
                   capture_output=True, check=True)

    b = Backend()
    api_up = _http_ok(f"{API_URL}/health/ready")
    broker_up = _tcp_ok("127.0.0.1", 1883)
    reuse = os.environ.get("E2E_REUSE_BACKEND") == "1"
    if reuse and api_up and broker_up:
        # explicitly opt-in to an externally managed backend
        yield b
        return
    if (api_up or broker_up) and not reuse:
        raise RuntimeError(
            "an api/broker is already listening (possibly from another test run); "
            "stop it or set E2E_REUSE_BACKEND=1 to reuse it"
        )
    # start whatever is missing (bun spawns a child; run it in its own
    # process group so teardown can kill the whole tree)
    if not api_up:
        b.api_pid = _spawn(["bun", "run", "start:api"], BACKEND_DIR, b.api_log)
        wait_http(f"{API_URL}/health/ready", 90)
    if not broker_up:
        b.broker_pid = _spawn(["bun", "run", "start:broker"], BACKEND_DIR, b.broker_log)
        # port-open is not enough: wait for the broker to actually be
        # listening (its startup log line), plus a settle buffer
        _wait_log(b.broker_log, "MQTT broker listening", 60)
        time.sleep(2)
    yield b
    for pid in (b.api_pid, b.broker_pid):
        if pid:
            _kill_tree(pid)


def _kill_tree(pid: int) -> None:
    try:
        os.killpg(pid, 15)  # SIGTERM to the process group
    except ProcessLookupError:
        pass
    except PermissionError:
        subprocess.run(["kill", str(pid)], capture_output=True)


def _spawn(cmd: list[str], cwd: Path, log: Path) -> int:
    with log.open("wb") as f:
        proc = subprocess.Popen(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                                env={**os.environ, "DATABASE_URL": DATABASE_URL,
                                     "JWT_SECRET": os.environ.get(
                                         "JWT_SECRET", "e2e-secret-" + "x" * 40)},
                                start_new_session=True)  # own process group
    return proc.pid


def _http_ok(url: str) -> bool:
    try:
        urllib.request.urlopen(url, timeout=2)
        return True
    except Exception:
        return False


def _tcp_ok(host: str, port: int) -> bool:
    import socket
    try:
        with socket.create_connection((host, port), timeout=2):
            return True
    except OSError:
        return False


def _wait_log(path: Path, needle: str, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if path.exists() and needle in path.read_text(errors="ignore"):
            return
        time.sleep(1)
    raise TimeoutError(f"{path.name} never printed {needle!r}")


@pytest.fixture(scope="session")
def device(backend: Backend) -> Device:
    # provision via the helper script (single source of truth for the
    # register/insert/credentials dance); parse its machine block
    prov = subprocess.run(
        [str(REPO_ROOT / "scripts" / "provision-device.sh"), E2E_UID],
        capture_output=True, text=True, check=True,
    ).stdout

    def kv(key: str) -> str:
        m = re.search(rf"^{key}=(.*)$", prov, re.MULTILINE)
        if not m:
            raise RuntimeError(f"provision output missing {key}:\n{prov}")
        return m.group(1)

    d = Device(uid=kv("device_uid"), password=kv("device_password"),
               device_id=kv("device_id"), project_id=kv("project_id"),
               token=kv("access_token"))
    return d


@pytest.fixture(scope="session", autouse=True)
def build_firmware(device: Device) -> Iterator[None]:
    """Builds the Ethernet/OpenETH firmware with the provisioned
    credentials baked in, into build/e2e-pytest/build (private SDKCONFIG,
    the normal build tree is untouched)."""
    E2E_DIR.mkdir(parents=True, exist_ok=True)
    (E2E_DIR / "sdkconfig.e2e").write_text(
        f'CONFIG_SOULCLOUD_DEVICE_UID="{device.uid}"\n'
        f'CONFIG_SOULCLOUD_DEVICE_PASSWORD="{device.password}"\n'
    )
    defaults = (f"sdkconfig.defaults;sdkconfig.defaults.eth;{E2E_DIR / 'sdkconfig.e2e'}")
    _idf(["-B", str(BUILD_DIR), f"-DSDKCONFIG={E2E_DIR / 'sdkconfig'}",
          f"-DSDKCONFIG_DEFAULTS={defaults}", "set-target", "esp32s3"], "set-target")
    _idf(["-B", str(BUILD_DIR), f"-DSDKCONFIG={E2E_DIR / 'sdkconfig'}",
          f"-DSDKCONFIG_DEFAULTS={defaults}", "build"], "build")
    elf = BUILD_DIR / "hello_world.elf"
    shutil.copyfile(elf, device.elf)
    device.elf_sha256 = _sha256(elf)
    yield


def _idf(args: list[str], what: str) -> None:
    idf_path = os.environ.get("IDF_PATH") or str(Path.home() / "esp" / "esp-idf")
    quoted = " ".join(shlex.quote(a) for a in args)
    proc = subprocess.run(
        ["bash", "-c", f'source "{idf_path}/export.sh" >/dev/null 2>&1 && idf.py {quoted}'],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"idf.py {what} failed:\n{proc.stdout[-3000:]}\n{proc.stderr[-3000:]}")


def _sha256(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ------------------------------------------------------------------ #
# pytest-embedded overrides
# ------------------------------------------------------------------ #

@pytest.fixture
def qemu_prog_path() -> str | None:
    """Auto-detect the Espressif QEMU binary (local ~/.espressif and CI
    /opt/esp layouts)."""
    env = os.environ.get("QEMU_BIN")
    if env:
        return env
    for base in (Path.home() / ".espressif" / "tools" / "qemu-xtensa",
                 Path(os.environ.get("IDF_TOOLS_PATH", "/opt/esp")) / "tools" / "qemu-xtensa"):
        if base.is_dir():
            for p in base.rglob("qemu-system-xtensa"):
                if p.is_file():
                    return str(p)
    raise RuntimeError("qemu-system-xtensa not found; install via idf_tools.py install qemu-xtensa")


# ------------------------------------------------------------------ #
# shared API helpers for the tests
# ------------------------------------------------------------------ #

@pytest.fixture
def api(device: Device):
    class Api:
        def get(self, path: str) -> str:
            return http_get(f"{API_URL}{path}", device.token)

        def post(self, path: str, body=None, form: dict | None = None) -> dict:
            if form is not None:
                return self._post_form(path, form)
            return http_json("POST", f"{API_URL}{path}", body, device.token)

        @staticmethod
        def _post_form(path: str, form: dict) -> dict:
            # minimal multipart/form-data (files + fields)
            boundary = "----e2e" + str(int(time.time() * 1000))
            parts = []
            for key, val in form.items():
                if isinstance(val, Path):
                    parts.append(
                        f"--{boundary}\r\n"
                        f'Content-Disposition: form-data; name="{key}"; filename="{val.name}"\r\n'
                        f"Content-Type: application/octet-stream\r\n\r\n"
                    )
                    parts.append(val.read_bytes())
                    parts.append(b"\r\n")
                else:
                    parts.append(
                        f"--{boundary}\r\n"
                        f'Content-Disposition: form-data; name="{key}"\r\n\r\n'
                        f"{val}\r\n"
                    )
            parts.append(f"--{boundary}--\r\n")
            body = b"".join(p if isinstance(p, bytes) else p.encode() for p in parts)
            req = urllib.request.Request(f"{API_URL}{path}", data=body, method="POST")
            req.add_header("content-type", f"multipart/form-data; boundary={boundary}")
            req.add_header("authorization", f"Bearer {device.token}")
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read())

        def firmware_state(self) -> dict:
            return json.loads(self.get(f"/v1/devices/{device.device_id}/firmware-state"))

        def send_command(self, cmd: str, args: list | None = None) -> str:
            return self.post("/v1/command-batches", {
                "device_ids": [device.device_id],
                "command": {"cmd": cmd, "args": args or []},
            })["batch_id"]

        def wait_command(self, batch_id: str, state: str = "device_completed",
                         timeout_s: float = 90) -> None:
            wait_sql(
                f"SELECT state FROM device_commands WHERE batch_id='{batch_id}';",
                state, timeout_s,
            )

    return Api()
