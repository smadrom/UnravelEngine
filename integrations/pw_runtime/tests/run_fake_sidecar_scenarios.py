#!/usr/bin/env python3
"""WP0 scenario runner for the fake PW MCP sidecar.

Spawns pw_runtime_fake_server in scenario mode for every
fixtures/scenarios/*.json, performs the MCP initialize handshake with
Content-Length framing, executes the scenario drive steps, subset-matches the
declared expectations against each response result, and checks the expected
terminal error code. Prints one PASS/FAIL line per scenario and exits nonzero
on any failure.

Portable configuration via environment only (no machine-local defaults):

- PW_FAKE_SIDECAR_EXE: fake server executable. Default: the first existing
  build/bin/<Config>/pw_runtime_fake_server.exe under the repo; the runner
  tries `cmake --build` once when the build tree is configured but the target
  has not been built yet.
- PW_FAKE_FIXTURES_DIR: fixtures root. Default: the fixtures/ directory next
  to this script.

The fake server itself reads PW_FAKE_SCENARIO (set by this runner per
scenario) or argv[1].
"""

import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

PROTOCOL_VERSION = "2025-11-25"
TERMINAL_CONNECTION_LOST = "connection_lost"
READ_TIMEOUT_SEC = 10.0
PROCESS_EXIT_TIMEOUT_SEC = 5.0
EXE_NAME = "pw_runtime_fake_server.exe" if os.name == "nt" else "pw_runtime_fake_server"
BUILD_CONFIGS = ("Release", "RelWithDebInfo", "Debug")


class SidecarEof(Exception):
    """The sidecar closed its stdout (or the stdin pipe broke) mid flow."""


class McpPipe:
    """Content-Length framed JSON-RPC exchange over the child stdio pipes."""

    def __init__(self, proc):
        self._proc = proc
        self._buffer = bytearray()
        self._eof = False
        self._condition = threading.Condition()
        self._reader = threading.Thread(target=self._drain_stdout, daemon=True)
        self._reader.start()

    def _drain_stdout(self):
        while True:
            # read1 issues at most one raw read and returns available bytes;
            # read(4096) would block until the buffer fills or EOF.
            chunk = self._proc.stdout.read1(4096)
            if not chunk:
                break
            with self._condition:
                self._buffer.extend(chunk)
                self._condition.notify_all()
        with self._condition:
            self._eof = True
            self._condition.notify_all()

    def send(self, message):
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        frame = b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n" + body
        try:
            self._proc.stdin.write(frame)
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise SidecarEof() from exc

    def receive(self, timeout=READ_TIMEOUT_SEC):
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                message = self._try_extract_frame()
                if message is not None:
                    return message
                if self._eof:
                    raise SidecarEof()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("timed out waiting for sidecar response")
                self._condition.wait(remaining)

    def _try_extract_frame(self):
        header_end = self._buffer.find(b"\r\n\r\n")
        if header_end < 0:
            return None
        header = bytes(self._buffer[:header_end]).decode("ascii", "replace")
        content_length = None
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                content_length = int(line.split(":", 1)[1].strip())
        if content_length is None:
            del self._buffer[:header_end + 4]
            return None
        body_begin = header_end + 4
        if len(self._buffer) < body_begin + content_length:
            return None
        body = bytes(self._buffer[body_begin:body_begin + content_length])
        del self._buffer[:body_begin + content_length]
        return json.loads(body.decode("utf-8"))


def resolve_path(value, path):
    """Resolves a dotted path; numeric segments index into lists."""
    node = value
    for segment in path.split("."):
        if isinstance(node, list):
            if not segment.isdigit() or int(segment) >= len(node):
                raise KeyError(path)
            node = node[int(segment)]
        elif isinstance(node, dict):
            if segment not in node:
                raise KeyError(path)
            node = node[segment]
        else:
            raise KeyError(path)
    return node


def check_expectations(expect, result, errors):
    for path, expected in expect.items():
        try:
            actual = resolve_path(result, path)
        except KeyError:
            errors.append(f"missing expected path result.{path}")
            continue
        if actual != expected:
            errors.append(f"result.{path}: expected {expected!r}, got {actual!r}")


def terminal_from_result(result):
    """Observed terminal code: tool errorCode first, then login.lastError."""
    structured = result.get("structuredContent", {})
    error_code = structured.get("errorCode") or ""
    if error_code:
        return error_code
    login = structured.get("state", {}).get("login", {})
    return login.get("lastError") or None


def run_scenario(exe_path, scenario_path):
    spec = json.loads(scenario_path.read_text(encoding="utf-8"))
    name = spec.get("scenario", scenario_path.stem)
    env = dict(os.environ)
    env["PW_FAKE_SCENARIO"] = str(scenario_path)
    proc = subprocess.Popen(
        [str(exe_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    errors = []
    observed_terminal = None
    try:
        pipe = McpPipe(proc)
        next_id = 0

        def request(method, params=None):
            nonlocal next_id
            next_id += 1
            message = {"jsonrpc": "2.0", "id": next_id, "method": method}
            if params is not None:
                message["params"] = params
            pipe.send(message)
            return pipe.receive()

        response = request("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "wp0-scenario-runner", "version": "1"},
        })
        protocol = response.get("result", {}).get("protocolVersion")
        if protocol != PROTOCOL_VERSION:
            errors.append(f"initialize protocolVersion: expected {PROTOCOL_VERSION!r}, got {protocol!r}")
        pipe.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

        last_result = None
        for index, step in enumerate(spec.get("drive", [])):
            method = step.get("method")
            if method is None:
                method = "tools/call"
                params = {"name": step["call"], "arguments": step.get("args", {})}
                label = step["call"]
            else:
                params = step.get("params")
                label = method
            try:
                response = request(method, params)
            except SidecarEof:
                if step.get("expect_eof"):
                    observed_terminal = TERMINAL_CONNECTION_LOST
                else:
                    errors.append(f"step {index + 1} ({label}): unexpected EOF from sidecar")
                break
            if step.get("expect_eof"):
                errors.append(f"step {index + 1} ({label}): expected disconnect but sidecar answered")
                break
            if "error" in response:
                errors.append(f"step {index + 1} ({label}): rpc error {response['error']}")
                break
            last_result = response.get("result", {})
            check_expectations(step.get("expect", {}), last_result, errors)
            if errors:
                errors[-1] = f"step {index + 1} ({label}): " + errors[-1]
                break

        expected_terminal = spec.get("expected_terminal_error_code")
        if not errors:
            if observed_terminal is None and last_result is not None:
                observed_terminal = terminal_from_result(last_result)
            if observed_terminal != expected_terminal:
                errors.append(
                    f"terminal error code: expected {expected_terminal!r}, observed {observed_terminal!r}")
    except (TimeoutError, SidecarEof) as exc:
        errors.append(f"transport failure: {exc}")
    finally:
        try:
            if proc.stdin and not proc.stdin.closed:
                proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=PROCESS_EXIT_TIMEOUT_SEC)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            errors.append("sidecar did not exit after stdin close")
        stderr_text = proc.stderr.read().decode("utf-8", "replace").strip() if proc.stderr else ""
        if stderr_text and errors:
            errors.append("sidecar stderr: " + stderr_text)
    return name, errors


def locate_fake_server(repo_root):
    override = os.environ.get("PW_FAKE_SIDECAR_EXE")
    if override:
        path = Path(override)
        if not path.is_file():
            sys.exit(f"PW_FAKE_SIDECAR_EXE does not name a file: {override}")
        return path
    candidates = [repo_root / "build" / "bin" / config / EXE_NAME for config in BUILD_CONFIGS]
    existing = [candidate for candidate in candidates if candidate.is_file()]
    if existing:
        # Prefer the freshest build so a stale config cannot shadow a new one.
        return max(existing, key=lambda candidate: candidate.stat().st_mtime)
    if (repo_root / "build" / "CMakeCache.txt").is_file():
        print("fake server exe not found; building target pw_runtime_fake_server (Release)")
        subprocess.run(
            ["cmake", "--build", str(repo_root / "build"), "--config", "Release",
             "--target", "pw_runtime_fake_server"],
            check=False,
        )
        for candidate in candidates:
            if candidate.is_file():
                return candidate
    sys.exit(
        "fake server executable not found; build target pw_runtime_fake_server "
        "or point PW_FAKE_SIDECAR_EXE at it")


def main():
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parents[2]
    fixtures_dir = Path(os.environ.get("PW_FAKE_FIXTURES_DIR", script_dir / "fixtures"))
    exe_path = locate_fake_server(repo_root)
    scenario_paths = sorted((fixtures_dir / "scenarios").glob("*.json"))
    if not scenario_paths:
        print(f"no scenarios found under {fixtures_dir / 'scenarios'}")
        return 1
    print(f"fake sidecar: {exe_path}")
    print(f"fixtures:     {fixtures_dir}")
    failures = 0
    for scenario_path in scenario_paths:
        name, errors = run_scenario(exe_path, scenario_path)
        if errors:
            failures += 1
            print(f"FAIL {name}")
            for error in errors:
                print(f"  - {error}")
        else:
            print(f"PASS {name}")
    print(f"{len(scenario_paths) - failures}/{len(scenario_paths)} scenarios passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
