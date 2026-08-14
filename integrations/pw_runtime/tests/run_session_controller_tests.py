"""Session-controller state-machine tests against the fake PW sidecar (WP3 gate).

Drives pw_session_controller_test.exe (controller -> pw_runtime_client ->
PWRuntimeBridge.dll -> pw_runtime_fake_server.exe) through scripted scenarios.
No live server and no secrets are involved; credentials below are dummy values
consumed only by the fake sidecar.

Env overrides (never required; defaults are repo-relative):
  PW_SESSION_TEST_EXE   - path to pw_session_controller_test.exe
  PW_RUNTIME_BRIDGE_DLL - path to PWRuntimeBridge.dll
  PW_FAKE_SIDECAR_EXE   - path to pw_runtime_fake_server.exe
  PW_FAKE_FIXTURES_DIR  - path to the fixtures directory
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(TESTS_DIR, "..", "..", ".."))
BIN_DIR = os.path.join(REPO_ROOT, "build", "bin", "RelWithDebInfo")

TEST_EXE = os.environ.get("PW_SESSION_TEST_EXE", os.path.join(BIN_DIR, "pw_session_controller_test.exe"))
BRIDGE_DLL = os.environ.get("PW_RUNTIME_BRIDGE_DLL", os.path.join(BIN_DIR, "PWRuntimeBridge.dll"))
FAKE_EXE = os.environ.get("PW_FAKE_SIDECAR_EXE", os.path.join(BIN_DIR, "pw_runtime_fake_server.exe"))
FIXTURES_DIR = os.environ.get("PW_FAKE_FIXTURES_DIR", os.path.join(TESTS_DIR, "fixtures"))
SCENARIOS_DIR = os.path.join(FIXTURES_DIR, "scenarios")

CONNECT = ("connect exe={exe} workdir={workdir} server=fake.local:29000 "
           "account=test_account password=test_password connect_ms=30000 enter_ms=30000")


def patch_hang_after_calls(scenario, count):
    scenario["hang_after_calls"] = count


def patch_status_prefix(count):
    """Keep only the first `count` pw.status responses; the last one repeats."""
    def apply(scenario):
        responses = scenario["tools"]["pw.status"]["responses"]
        scenario["tools"]["pw.status"]["responses"] = responses[:count]
    return apply


def patch_status_drop(indices):
    """Drop pw.status responses by index (the controller's call cadence differs
    from the WP0 drive script; intermediate phases it never polls are removed)."""
    def apply(scenario):
        responses = scenario["tools"]["pw.status"]["responses"]
        scenario["tools"]["pw.status"]["responses"] = [
            r for i, r in enumerate(responses) if i not in indices]
    return apply


def patch_single_pending_enter(scenario):
    responses = scenario["tools"]["pw.role_enter"]["responses"]
    scenario["tools"]["pw.role_enter"]["responses"] = responses[:1]


def patch_preview_error(scenario):
    """Every pw.role_preview call fails closed; enter must still succeed."""
    state = scenario["tools"]["pw.role_list"]["responses"][0]["state"]
    scenario["tools"]["pw.role_preview"]["responses"] = [{
        "ok": False,
        "errorCode": "role_not_found",
        "message": "Role is not in the current role list",
        "state": state,
    }]


TESTS = [
    {
        "name": "success_full",
        "scenario": "success",
        # The controller never polls status between pending and done; the
        # entering_world status snapshot is consumed by no one, and the
        # post-entry polls must see the in_world snapshot.
        "patches": [patch_status_drop([3])],
        "expect_sequence": ["starting", "authenticating", "character_select", "entering_world", "in_world"],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "assert_revision 1",
            "assert_roles 2",
            "assert_camera selchar",
            "select 1025",
            "assert_camera choose",
            "enter",
            "wait_state in_world 30000",
            "assert_attested 1025 1",
            "assert_capability 1",
            "disconnect",
            "wait_state idle 10000",
            "assert_capability 0",
        ],
    },
    {
        "name": "preview_fetch",
        "scenario": "preview",
        # Same status cadence as success_full; the two selects each trigger a
        # pw.role_preview fetch and must publish distinct previews.
        "patches": [patch_status_drop([3])],
        "expect_sequence": ["starting", "authenticating", "character_select", "entering_world", "in_world"],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "assert_revision 1",
            "assert_roles 2",
            "select 1024",
            "wait_preview 1024 10000",
            "assert_preview 1024 0 0",
            "assert_preview_equipment 2",
            "select 1025",
            "wait_preview 1025 10000",
            "assert_preview 1025 3 1",
            "assert_preview_equipment 1",
            "enter",
            "wait_state in_world 30000",
            "assert_attested 1025 1",
            # The preview survives entry (the proxy stays up into the world).
            "assert_preview 1025 3 1",
            "disconnect",
            "wait_state idle 10000",
            "assert_preview_none",
        ],
    },
    {
        "name": "preview_error_nonblocking",
        "scenario": "preview",
        "patches": [patch_status_drop([3]), patch_preview_error],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "select 1025",
            "wait_preview 1025 10000",
            "assert_preview_error role_not_found",
            "enter",
            "wait_state in_world 30000",
            "assert_attested 1025 1",
            "disconnect",
            "wait_state idle 10000",
        ],
    },
    {
        "name": "world_stream",
        "scenario": "world",
        "patches": [patch_status_drop([3])],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "select 1025",
            "enter",
            "wait_state in_world 30000",
            "wait_world 10000",
            "wait_world_entities 5 10000",
            "assert_world_self 1025 1",
            "assert_world_entities 5",
            "assert_world_kind monster",
            "assert_world_kind npc",
            "assert_world_kind player",
            "assert_world_kind matter",
            "disconnect",
            "wait_state idle 10000",
            "assert_world_none",
        ],
    },
    {
        "name": "world_actions",
        "scenario": "world",
        "patches": [patch_status_drop([3])],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            # Mutations stay closed before the attested entry.
            "world_action jump expect_reject",
            "select 1025",
            "enter",
            "wait_state in_world 30000",
            "wait_world_entities 5 10000",
            "world_action move_to x=130.0 y=962.0 z=21.0",
            "sleep 1500",
            "assert_world_no_error",
            "world_action jump",
            "world_action select_target id=4001",
            "sleep 1500",
            "assert_world_no_error",
            "world_action normal_attack",
            "sleep 1500",
            "assert_world_no_error",
            "world_action stop_move",
            "sleep 1500",
            "assert_world_no_error",
            "disconnect",
            "wait_state idle 10000",
            "assert_world_none",
        ],
    },
    {
        "name": "no_timer_jump",
        "scenario": "success",
        "patches": [patch_status_prefix(3)],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "sleep 3000",
            "assert_not_state in_world",
            "assert_state character_select",
            "disconnect",
            "wait_state idle 10000",
        ],
    },
    {
        "name": "wrong_password",
        "scenario": "wrong_password",
        "script": [
            CONNECT,
            "wait_state error 30000",
            "assert_error account_login_failed",
            "assert_capability 0",
            "disconnect",
            "wait_state idle 10000",
        ],
    },
    {
        "name": "empty_roles",
        "scenario": "empty_roles",
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "assert_roles 0",
            "select 1024 expect_reject",
            "enter expect_reject",
            "disconnect",
            "wait_state idle 10000",
        ],
    },
    {
        "name": "stale_list",
        "scenario": "stale_list",
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "assert_revision 1",
            "select 1025",
            "sleep 1500",
            "assert_selection 0",
            "assert_revision 2",
            "select 1025",
            "enter",
            "wait_state error 30000",
            "assert_error role_list_revision_stale",
            "assert_capability 0",
        ],
    },
    {
        "name": "disconnect_mid_enter",
        "scenario": "disconnect",
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "select 1025",
            "enter",
            "wait_state entering_world 10000",
            "wait_state error 60000",
            "assert_error connection_lost",
            "assert_not_state in_world",
            "assert_capability 0",
        ],
    },
    {
        "name": "role_mismatch",
        "scenario": "role_mismatch",
        # Only the character_select status is polled before enter; the trailing
        # in_world status snapshot of the fixture must not leak into idle polls.
        "patches": [patch_status_prefix(1)],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "select 1025",
            "enter",
            "wait_state error 30000",
            "assert_error role_mismatch",
            "assert_not_state in_world",
            "assert_capability 0",
        ],
    },
    {
        "name": "world_mismatch",
        "scenario": "world_mismatch",
        "patches": [patch_status_prefix(1)],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "select 1025",
            "enter",
            "wait_state error 30000",
            "assert_error world_mismatch",
            "assert_not_state in_world",
            "assert_capability 0",
        ],
    },
    {
        "name": "user_disconnect_char_select",
        "scenario": "success",
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "disconnect",
            "wait_state idle 10000",
            "assert_capability 0",
        ],
    },
    {
        "name": "user_disconnect_entering_world",
        "scenario": "success",
        "patches": [patch_status_prefix(3), patch_single_pending_enter],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "select 1025",
            "enter",
            "wait_state entering_world 10000",
            "disconnect",
            "wait_state idle 10000",
            "assert_capability 0",
        ],
    },
    {
        "name": "retry_after_disconnect",
        "scenario": "success",
        "patches": [patch_status_prefix(3)],
        "script": [
            CONNECT,
            "wait_state character_select 30000",
            "assert_revision 1",
            "disconnect",
            "wait_state idle 10000",
            CONNECT,
            "wait_state character_select 30000",
            "assert_revision 1",
            "assert_roles 2",
            "assert_capability 0",
            "disconnect",
            "wait_state idle 10000",
        ],
    },
    {
        "name": "hang_timeout",
        "scenario": "success",
        "patches": [patch_status_prefix(3), lambda s: patch_hang_after_calls(s, 6)],
        "script": [
            "connect exe={exe} workdir={workdir} server=fake.local:29000 "
            "account=test_account password=test_password connect_ms=30000 enter_ms=60000",
            "wait_state character_select 30000",
            "select 1025",
            "enter",
            "wait_state entering_world 10000",
            "wait_state error 90000",
            "assert_error connection_lost",
            "assert_not_state in_world",
        ],
        "wall_clock_s": 150,
    },
]


def run_test(test):
    tmp = tempfile.mkdtemp(prefix="pw_session_test_")
    try:
        with open(os.path.join(SCENARIOS_DIR, test["scenario"] + ".json"), encoding="utf-8") as f:
            scenario = json.load(f)
        if "patches" in test:
            for patch in test["patches"]:
                patch(scenario)
        with open(os.path.join(tmp, "pw_fake_scenario.json"), "w", encoding="utf-8") as f:
            json.dump(scenario, f)

        script_text = "\n".join(
            line.format(exe=FAKE_EXE, workdir=tmp) for line in test["script"]
        ) + "\n"
        script_path = os.path.join(tmp, "script.txt")
        with open(script_path, "w", encoding="utf-8") as f:
            f.write(script_text)

        env = dict(os.environ)
        env["PW_RUNTIME_LIBRARY"] = BRIDGE_DLL
        wall_clock = test.get("wall_clock_s", 120)
        try:
            proc = subprocess.run(
                [TEST_EXE, "--script", script_path],
                env=env, capture_output=True, text=True, timeout=wall_clock,
            )
        except subprocess.TimeoutExpired:
            return False, "wall-clock timeout ({}s) - hung future?".format(wall_clock), ""

        trace_states = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    trace_states.append(json.loads(line).get("state"))
                except json.JSONDecodeError:
                    pass

        if proc.returncode != 0:
            detail = proc.stderr.strip() or proc.stdout.strip()
            return False, "exit code {}: {}".format(proc.returncode, detail), proc.stdout

        expected = test.get("expect_sequence")
        if expected:
            pos = 0
            for state in trace_states:
                if pos < len(expected) and state == expected[pos]:
                    pos += 1
            if pos != len(expected):
                return False, "trace misses sequence {} (got {})".format(expected, trace_states), proc.stdout
        return True, "", proc.stdout
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    for label, path in [("test exe", TEST_EXE), ("bridge dll", BRIDGE_DLL), ("fake sidecar", FAKE_EXE)]:
        if not os.path.exists(path):
            print("missing {}: {}".format(label, path))
            return 1
    print("test exe:     {}".format(TEST_EXE))
    print("bridge dll:   {}".format(BRIDGE_DLL))
    print("fake sidecar: {}".format(FAKE_EXE))
    failures = 0
    for test in TESTS:
        ok, detail, output = run_test(test)
        if ok:
            print("PASS {}".format(test["name"]))
        else:
            failures += 1
            print("FAIL {}: {}".format(test["name"], detail))
            if os.environ.get("PW_SESSION_TEST_VERBOSE"):
                print(output)
    total = len(TESTS)
    print("{}/{} session tests passed".format(total - failures, total))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
