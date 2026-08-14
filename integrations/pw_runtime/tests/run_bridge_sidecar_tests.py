"""Bridge-level fake-sidecar tests for the PW runtime DLL (WP2).

Drives PWRuntimeBridge.dll through ctypes against pw_runtime_fake_server.exe
running scenario fixtures. Covers: v1 compatibility, v2 start with credential
spans (presence/length proof only, never values), pw.role_list, pw.role_enter
pending/done, fail-closed stale revision, request timeout, mid-flow crash, and
sidecar restart. No live server and no secrets are involved.

Env overrides (never required to be set; defaults are repo-relative):
  PW_RUNTIME_BRIDGE_DLL  - path to PWRuntimeBridge.dll
  PW_FAKE_SIDECAR_EXE    - path to pw_runtime_fake_server.exe
  PW_FAKE_FIXTURES_DIR   - path to the fixtures directory
"""

import ctypes
import json
import os
import shutil
import sys
import tempfile
from ctypes import (CFUNCTYPE, POINTER, Structure, c_char, c_char_p, c_int32,
                    c_size_t, c_uint8, c_uint32, c_void_p, c_wchar_p)

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(TESTS_DIR, "..", "..", ".."))

BRIDGE_DLL = os.environ.get(
    "PW_RUNTIME_BRIDGE_DLL",
    os.path.join(REPO_ROOT, "build", "bin", "RelWithDebInfo", "PWRuntimeBridge.dll"))
FAKE_EXE = os.environ.get(
    "PW_FAKE_SIDECAR_EXE",
    os.path.join(REPO_ROOT, "build", "bin", "RelWithDebInfo", "pw_runtime_fake_server.exe"))
FIXTURES_DIR = os.environ.get("PW_FAKE_FIXTURES_DIR", os.path.join(TESTS_DIR, "fixtures"))
SCENARIOS_DIR = os.path.join(FIXTURES_DIR, "scenarios")

RESULT_OK = 0
RESULT_PROCESS_ERROR = 4
RESULT_IO_ERROR = 5
RESULT_TIMEOUT = 6

RESPONSE_CB = CFUNCTYPE(None, POINTER(c_char), c_uint32, c_void_p)


class StartInfoV1(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("executable_path", c_wchar_p),
        ("working_directory", c_wchar_p),
        ("startup_timeout_ms", c_uint32),
    ]


class StartInfoV2(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("executable_path", c_wchar_p),
        ("working_directory", c_wchar_p),
        ("startup_timeout_ms", c_uint32),
        ("role_admin", c_uint32),
        ("account_utf8", POINTER(c_uint8)),
        ("account_utf8_size", c_uint32),
        ("password_utf8", POINTER(c_uint8)),
        ("password_utf8_size", c_uint32),
        ("server", c_wchar_p),
        ("exact_role_id", c_int32),
    ]


class StatusV1(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("process_state", c_uint32),
        ("process_id", c_uint32),
        ("exit_code", c_int32),
    ]


class ApiV2(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("abi_version", c_uint32),
        ("create", c_void_p),
        ("destroy", c_void_p),
        ("start", c_void_p),
        ("stop", c_void_p),
        ("get_status", c_void_p),
        ("request_json", c_void_p),
        ("copy_last_error", c_void_p),
        ("start_v2", c_void_p),
    ]


class Bridge:
    def __init__(self, dll_path, abi_version):
        self._dll = ctypes.CDLL(dll_path)
        self.api = ApiV2()
        # v1 callers pass the v1 table size; the DLL fills only the v1 prefix.
        table_size = (ctypes.sizeof(ApiV2) if abi_version == 2
                      else 2 * 4 + 7 * ctypes.sizeof(c_void_p))
        result = self._dll.pw_runtime_get_api(abi_version, table_size, ctypes.byref(self.api))
        if result != RESULT_OK:
            raise RuntimeError(f"pw_runtime_get_api(v{abi_version}) -> {result}")
        self.abi_version = abi_version
        self.handle = c_void_p()
        create = ctypes.cast(self.api.create, CFUNCTYPE(c_int32, POINTER(c_void_p)))
        if create(ctypes.byref(self.handle)) != RESULT_OK or not self.handle:
            raise RuntimeError("create failed")
        self._next_id = 1
        self._keepalive = []

    def destroy(self):
        if self.handle:
            destroy = ctypes.cast(self.api.destroy, CFUNCTYPE(None, c_void_p))
            destroy(self.handle)
            self.handle = None

    def start_v1(self, exe, workdir, timeout_ms=10000):
        info = StartInfoV1(ctypes.sizeof(StartInfoV1), exe, workdir, timeout_ms)
        start = ctypes.cast(self.api.start, CFUNCTYPE(c_int32, c_void_p, POINTER(StartInfoV1)))
        return start(self.handle, ctypes.byref(info))

    def start_v2(self, exe, workdir, account=b"", password=b"", server="",
                 role_admin=1, timeout_ms=10000):
        account_span = (c_uint8 * max(1, len(account)))()
        if account:
            ctypes.memmove(account_span, account, len(account))
        password_span = (c_uint8 * max(1, len(password)))()
        if password:
            ctypes.memmove(password_span, password, len(password))
        info = StartInfoV2()
        info.struct_size = ctypes.sizeof(StartInfoV2)
        info.executable_path = exe
        info.working_directory = workdir
        info.startup_timeout_ms = timeout_ms
        info.role_admin = role_admin
        info.account_utf8 = account_span if account else None
        info.account_utf8_size = len(account)
        info.password_utf8 = password_span if password else None
        info.password_utf8_size = len(password)
        info.server = server
        info.exact_role_id = 0
        start_v2 = ctypes.cast(self.api.start_v2, CFUNCTYPE(c_int32, c_void_p, POINTER(StartInfoV2)))
        result = start_v2(self.handle, ctypes.byref(info))
        # The local span copies die here; the bridge already scrubbed its own.
        return result

    def stop(self, timeout_ms=2000):
        stop = ctypes.cast(self.api.stop, CFUNCTYPE(c_int32, c_void_p, c_uint32))
        return stop(self.handle, timeout_ms)

    def status(self):
        out = StatusV1()
        out.struct_size = ctypes.sizeof(StatusV1)
        get_status = ctypes.cast(self.api.get_status, CFUNCTYPE(c_int32, c_void_p, POINTER(StatusV1)))
        if get_status(self.handle, ctypes.byref(out)) != RESULT_OK:
            raise RuntimeError("get_status failed")
        return out

    def request(self, payload, timeout_ms=5000):
        collected = []

        def on_response(data, size, _user):
            collected.append(ctypes.string_at(data, size))

        callback = RESPONSE_CB(on_response)
        self._keepalive.append(callback)
        request_json = payload if isinstance(payload, bytes) else payload.encode("utf-8")
        request_json_fn = ctypes.cast(
            self.api.request_json,
            CFUNCTYPE(c_int32, c_void_p, c_char_p, c_uint32, c_uint32, RESPONSE_CB, c_void_p))
        result = request_json_fn(self.handle, request_json, len(request_json),
                                 timeout_ms, callback, None)
        if result != RESULT_OK:
            return result, None
        return RESULT_OK, json.loads(collected[0].decode("utf-8"))

    def call_tool(self, name, arguments, timeout_ms=5000):
        self._next_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": self._next_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        result, response = self.request(json.dumps(request), timeout_ms)
        if result != RESULT_OK:
            return result, None
        return RESULT_OK, response["result"]["structuredContent"]


def make_workdir(scenario_name):
    workdir = tempfile.mkdtemp(prefix="pw_bridge_test_")
    if scenario_name is not None:
        shutil.copyfile(os.path.join(SCENARIOS_DIR, scenario_name),
                        os.path.join(workdir, "pw_fake_scenario.json"))
    return workdir


def cleanup(workdir):
    shutil.rmtree(workdir, ignore_errors=True)


class Failure(Exception):
    pass


def expect(condition, message):
    if not condition:
        raise Failure(message)


def test_v1_compat():
    workdir = make_workdir(None)  # static mode: no scenario file
    bridge = Bridge(BRIDGE_DLL, 1)
    try:
        expect(bridge.start_v1(FAKE_EXE, workdir) == RESULT_OK, "v1 start failed")
        result, content = bridge.call_tool("pw.status", {})
        expect(result == RESULT_OK and content is not None, "v1 pw.status transport failed")
        expect(content["connected"] is False, "static fake must report connected=false")
        expect(bridge.stop() == RESULT_OK, "v1 stop failed")
    finally:
        bridge.destroy()
        cleanup(workdir)


def test_start_list_enter():
    workdir = make_workdir("success.json")
    bridge = Bridge(BRIDGE_DLL, 2)
    account = b"fake_account"
    password = b"fake_password_123"
    try:
        result = bridge.start_v2(FAKE_EXE, workdir, account=account, password=password,
                                 server="127.0.0.1:29000", role_admin=1)
        expect(result == RESULT_OK, f"start_v2 -> {result}")
        with open(os.path.join(workdir, "pw_fake_env_probe.json"), encoding="utf-8") as stream:
            probe = json.load(stream)
        expect(probe["account_set"] and probe["account_len"] == len(account),
               "account span did not reach the child environment")
        expect(probe["password_set"] and probe["password_len"] == len(password),
               "password span did not reach the child environment")
        expect(probe["role_admin"] == "1", "role_admin flag did not reach the child environment")
        expect(probe["server_set"], "server did not reach the child environment")
        probe_text = json.dumps(probe)
        expect("fake_password_123" not in probe_text and "fake_account" not in probe_text,
               "env probe leaked a credential value")

        result, content = bridge.call_tool("pw.role_list", {})
        expect(result == RESULT_OK and content["ok"], "pw.role_list failed")
        expect(content["payload"]["revision"] == 1, "role_list revision must be 1")
        expect(len(content["payload"]["roles"]) == 2, "role_list must carry 2 roles")

        args = {"role_id": 1025, "role_list_revision": 1, "operation_id": 7}
        result, content = bridge.call_tool("pw.role_enter", args)
        expect(result == RESULT_OK and content["ok"], "pw.role_enter accept failed")
        expect(content["payload"]["result"] == "pending", "first role_enter must be pending")
        result, content = bridge.call_tool("pw.role_enter", args)
        expect(result == RESULT_OK and content["ok"], "pw.role_enter poll failed")
        payload = content["payload"]
        expect(payload.get("status") == "entered", "role_enter poll must complete")
        expect(payload["attestedRoleId"] == 1025, "attested role must equal the requested one")
        expect(payload["instanceId"] == 1, "attested instance must equal the role worldtag")
        expect(bridge.stop() == RESULT_OK, "stop failed")
    finally:
        bridge.destroy()
        cleanup(workdir)


def test_stale_revision_fails_closed():
    workdir = make_workdir("stale_list.json")
    bridge = Bridge(BRIDGE_DLL, 2)
    try:
        expect(bridge.start_v2(FAKE_EXE, workdir, account=b"fake_account",
                               password=b"fake_password_123") == RESULT_OK, "start_v2 failed")
        result, content = bridge.call_tool("pw.role_list", {})
        expect(result == RESULT_OK and content["payload"]["revision"] == 1, "first list revision")
        result, content = bridge.call_tool("pw.role_list", {})
        expect(result == RESULT_OK and content["payload"]["revision"] == 2, "relist must bump revision")
        result, content = bridge.call_tool(
            "pw.role_enter", {"role_id": 1024, "role_list_revision": 1, "operation_id": 9})
        expect(result == RESULT_OK, "transport error on stale role_enter")
        expect(content["ok"] is False, "stale revision must fail")
        expect(content["errorCode"] == "role_list_revision_stale",
               f"unexpected terminal code {content['errorCode']}")
        expect(bridge.stop() == RESULT_OK, "stop failed")
    finally:
        bridge.destroy()
        cleanup(workdir)


def test_request_timeout():
    workdir = tempfile.mkdtemp(prefix="pw_bridge_test_")
    scenario = {
        "scenario": "hang",
        "hang_after_calls": 1,
        "tools": {
            "pw.status": {
                "responses": [{"ok": True, "errorCode": "", "message": "x", "state": {}}]
            }
        },
    }
    with open(os.path.join(workdir, "pw_fake_scenario.json"), "w", encoding="utf-8") as stream:
        json.dump(scenario, stream)
    bridge = Bridge(BRIDGE_DLL, 2)
    try:
        expect(bridge.start_v2(FAKE_EXE, workdir) == RESULT_OK, "start_v2 failed")
        result, _ = bridge.call_tool("pw.status", {}, timeout_ms=1500)
        expect(result == RESULT_TIMEOUT, f"hung sidecar must surface TIMEOUT, got {result}")
        expect(bridge.stop() == RESULT_OK, "stop of hung sidecar failed")
    finally:
        bridge.destroy()
        cleanup(workdir)


def test_crash_mid_flow():
    workdir = make_workdir("disconnect.json")  # answers 5 calls, then exits
    bridge = Bridge(BRIDGE_DLL, 2)
    try:
        expect(bridge.start_v2(FAKE_EXE, workdir) == RESULT_OK, "start_v2 failed")
        for index in range(5):
            result, content = bridge.call_tool("pw.status", {})
            expect(result == RESULT_OK, f"call {index + 1} before crash must succeed")
        result, _ = bridge.call_tool("pw.status", {})
        expect(result in (RESULT_PROCESS_ERROR, RESULT_IO_ERROR),
               f"crashed sidecar must surface PROCESS/IO error, got {result}")
    finally:
        bridge.destroy()
        cleanup(workdir)


def test_restart():
    workdir = make_workdir("success.json")
    bridge = Bridge(BRIDGE_DLL, 2)
    try:
        expect(bridge.start_v2(FAKE_EXE, workdir) == RESULT_OK, "first start failed")
        first_pid = bridge.status().process_id
        expect(first_pid != 0, "first child pid missing")
        expect(bridge.stop() == RESULT_OK, "first stop failed")
        # The fake re-reads the same scenario file; responses restart from the top.
        expect(bridge.start_v2(FAKE_EXE, workdir) == RESULT_OK, "restart failed")
        second_pid = bridge.status().process_id
        expect(second_pid != 0 and second_pid != first_pid, "restart must produce a new pid")
        result, content = bridge.call_tool("pw.role_list", {})
        expect(result == RESULT_OK and content["ok"], "role_list after restart failed")
        expect(bridge.stop() == RESULT_OK, "second stop failed")
    finally:
        bridge.destroy()
        cleanup(workdir)


TESTS = [
    ("v1_compat", test_v1_compat),
    ("start_list_enter", test_start_list_enter),
    ("stale_revision_fails_closed", test_stale_revision_fails_closed),
    ("request_timeout", test_request_timeout),
    ("crash_mid_flow", test_crash_mid_flow),
    ("restart", test_restart),
]


def main():
    for path, label in ((BRIDGE_DLL, "PW_RUNTIME_BRIDGE_DLL"), (FAKE_EXE, "PW_FAKE_SIDECAR_EXE")):
        if not os.path.isfile(path):
            print(f"missing {label}: {path}")
            return 1
    failures = 0
    for name, test in TESTS:
        try:
            test()
            print(f"PASS {name}")
        except Exception as error:  # noqa: BLE001 - report any failure as a test failure
            failures += 1
            print(f"FAIL {name} - {error}")
    print(f"{len(TESTS) - failures}/{len(TESTS)} bridge tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
