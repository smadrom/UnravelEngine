"""WP6 live acceptance driver: Unravel editor + PW headless sidecar + spw server.

Launches UnravelEditor with the converted login-scene project, activates the PW
login UI, and drives the full route (connect -> character select -> select role
-> enter world) through the session-controller MCP tools over the editor TCP
control channel. Captures screenshots of the UI states, records a redacted MCP
transcript, and sweeps for credential leaks.

Configuration is environment-only (no secrets in this file or in the report):
  PW_ACCOUNT / PW_PASSWORD      - test account credentials (required)
  PW_SERVER                     - host:port of the test server (required)
  PW_RUNTIME_CLIENT_EXE         - headless client exe (required)
  PW_RUNTIME_WORKING_DIRECTORY  - element root matching the server data (required)
  UNRAVEL_EDITOR_EXE            - default build/bin/RelWithDebInfo/UnravelEditor.exe
  UNRAVEL_PW_PROJECT            - Unravel project containing PW content (required)
  UNRAVEL_MCP_PORT              - default 17890
  UNRAVEL_MCP_CALL_TIMEOUT      - per-call seconds; raise for a cold world asset import
  PW_ACCEPTANCE_OUT             - output dir for transcript/screenshots/SUMMARY
  PW_ACCEPTANCE_ROLE_ID         - live worldmap role id; default 3057
  PW_ACCEPTANCE_ACCOUNT_EMPTY   - account expected to have zero roles (empty-roles run)

Usage: py -3 run_editor_acceptance.py [happy|preview|preview-offline|world|worldmap|worldmap-offline|wrong-password|empty-roles|all]
"""

import datetime
import json
import os
import socket
import struct
import subprocess
import sys
import time

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(TESTS_DIR, "..", "..", ".."))

EDITOR_EXE = os.environ.get(
    "UNRAVEL_EDITOR_EXE", os.path.join(REPO_ROOT, "build", "bin", "RelWithDebInfo", "UnravelEditor.exe"))
PROJECT_DIR = os.environ.get("UNRAVEL_PW_PROJECT", "")
MCP_PORT = int(os.environ.get("UNRAVEL_MCP_PORT", "17890"))
MCP_CALL_TIMEOUT_S = float(os.environ.get("UNRAVEL_MCP_CALL_TIMEOUT", "30"))
OUT_DIR = os.environ.get(
    "PW_ACCEPTANCE_OUT",
    os.path.join(REPO_ROOT, "artifacts", "pw-acceptance-" + datetime.date.today().isoformat()))
CLIENT_EXE = os.environ.get("PW_RUNTIME_CLIENT_EXE", "")

WORLD_MAP_INSTANCE_ID = 161
WORLD_MAP_SLUG = "a61"
WORLD_MAP_LOAD_TIMEOUT_S = 600
WORLD_MAP_TERRAIN_TOLERANCE_M = 0.5
WORLD_MAP_MOVE_DISTANCE_M = 5.0
WORLD_MAP_MIN_DISPLACEMENT_M = 0.1
WORLD_MAP_ROLE_ID = int(os.environ.get("PW_ACCEPTANCE_ROLE_ID", "3057"))
# Genuine role-3057 ground positions captured before/after a successful move.
# These are deliberately not assembled from nearby entity coordinates.
WORLD_MAP_OFFLINE_GROUND_POINTS = (
    {"x": 227.10800170898438, "y": 36.10300064086914, "z": 164.4810028076172},
    {"x": 223.24200439453125, "y": 36.16400146484375, "z": 167.65199279785156},
)
OFFLINE_MODES = ("preview-offline", "worldmap-offline")
SUPPORTED_MODES = (
    "happy", "preview", "preview-offline", "world", "worldmap", "worldmap-offline",
    "wrong-password", "empty-roles", "all",
)

SECRET_VALUES = []  # filled from env; swept out of every artifact


class EditorMcp:
    """Length-prefixed JSON frames over the editor TCP control channel.

    One short-lived connection per call: the server closes any client that is
    idle for more than ~5s (SO_RCVTIMEO), so persistent connections break
    whenever the driver pauses between polls.
    """

    def __init__(self, port, timeout=30.0):
        self.port = port
        self.timeout = timeout
        self.seq = 0
        self.transcript = []

    def call(self, method, params=None):
        self.seq += 1
        request = {"seq": self.seq, "method": method, "params": params or {}}
        body = json.dumps(request).encode()
        with socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout) as sock:
            sock.sendall(struct.pack("<I", len(body)) + body)
            header = self._recv_exact(sock, 4)
            (length,) = struct.unpack("<I", header)
            response = json.loads(self._recv_exact(sock, length).decode("utf-8", "replace"))
        self.transcript.append({"method": method, "params": params or {}, "response": response})
        return response

    def call_result(self, method, params=None):
        response = self.call(method, params)
        if not response.get("ok"):
            raise RuntimeError("{} failed: {}".format(method, response.get("error")))
        result = response.get("result", {})
        if isinstance(result, str):
            try:
                return json.loads(result)
            except json.JSONDecodeError:
                return {"text": result}
        return result

    def _recv_exact(self, sock, count):
        data = b""
        while len(data) < count:
            chunk = sock.recv(count - len(data))
            if not chunk:
                raise RuntimeError("control channel closed")
            data += chunk
        return data

    def close(self):
        pass


def wait_for_port(port, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2):
                return True
        except OSError:
            time.sleep(2)
    return False


def wait_session_state(mcp, wanted, timeout_s, forbidden=()):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        snap = mcp.call_result("pw_session_status")
        last = snap
        if snap.get("state") in forbidden:
            raise RuntimeError("entered forbidden state {} ({})".format(
                snap.get("state"), snap.get("errorCode")))
        if snap.get("state") == wanted:
            return snap
        time.sleep(1)
    raise RuntimeError("timed out waiting for {}; last={}".format(wanted, last))


def wait_login_scene(mcp, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        status = mcp.call_result("login_status")
        if status.get("status") == "done":
            return status
        if status.get("status") == "error":
            raise RuntimeError("login scene load failed: {}".format(status.get("error")))
        time.sleep(2)
    raise RuntimeError("login scene load timed out")


def wait_map_load(mcp, slug, timeout_s):
    """Wait until the requested map owns the active, completed load state."""
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        status = mcp.call_result("login_status")
        last = status
        if status.get("status") == "done" and status.get("map") == slug:
            return status
        attempted_map = status.get("attempted_map")
        start_error = status.get("start_error")
        if attempted_map == slug and start_error:
            raise RuntimeError("map {} load failed to start: {}".format(slug, start_error))
        if status.get("status") == "error":
            raise RuntimeError("map {} load failed: {}".format(
                slug, status.get("error") or start_error or status))
        time.sleep(2)
    raise RuntimeError("map {} load timed out; last={}".format(slug, last))


def screenshot(mcp, name, ui=True):
    path = os.path.join(OUT_DIR, name)
    mcp.call_result("screenshot", {"path": path, "w": 1600, "h": 900, "ui": ui})
    deadline = time.time() + 60
    while time.time() < deadline:
        status = mcp.call_result("screenshot_status")
        if status.get("completed") and status.get("path") == path:
            return path
        if status.get("status") == "error":
            raise RuntimeError("screenshot failed: {}".format(status.get("error")))
        time.sleep(1)
    raise RuntimeError("screenshot timed out")


def launch_editor(account, password, client_exe=None, workdir=None):
    env = dict(os.environ)
    env["PW_MCP_PORT"] = str(MCP_PORT)
    env["PW_RUNTIME_CLIENT_EXE"] = client_exe or CLIENT_EXE
    if workdir:
        env["PW_RUNTIME_WORKING_DIRECTORY"] = workdir
    env["PW_SERVER"] = os.environ["PW_SERVER"]
    env["PW_ACCOUNT"] = account
    env["PW_PASSWORD"] = password
    proc = subprocess.Popen(
        [EDITOR_EXE, "--project", PROJECT_DIR],
        cwd=os.path.dirname(os.path.abspath(EDITOR_EXE)),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return proc


def launch_editor_headless():
    """Launch the editor for asset-only MCP checks without a PW session environment."""
    env = dict(os.environ)
    for key in ("PW_ACCOUNT", "PW_PASSWORD", "PW_SERVER", "PW_RUNTIME_CLIENT_EXE",
                "PW_RUNTIME_WORKING_DIRECTORY", "PW_ACCEPTANCE_ACCOUNT_EMPTY",
                "PW_ACCEPTANCE_PASSWORD_EMPTY"):
        env.pop(key, None)
    env["PW_MCP_PORT"] = str(MCP_PORT)
    return subprocess.Popen(
        [EDITOR_EXE, "--project", PROJECT_DIR],
        cwd=os.path.dirname(os.path.abspath(EDITOR_EXE)),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def redact(text):
    # Longest-first prevents a shorter overlapping secret from exposing the
    # suffix of a longer one (account=alice, password=alice123).
    for secret in sorted(set(SECRET_VALUES), key=len, reverse=True):
        if secret:
            text = text.replace(secret, "***")
    return text


def redact_value(value):
    if isinstance(value, str):
        return redact(value)
    if isinstance(value, list):
        return [redact_value(item) for item in value]
    if isinstance(value, dict):
        return {key: redact_value(item) for key, item in value.items()}
    return value


def save_transcript(mcp, name):
    path = os.path.join(OUT_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(redact_value(mcp.transcript), f, ensure_ascii=False, indent=2)
    return path


def sweep_leaks():
    leaks = []
    for root, _dirs, files in os.walk(OUT_DIR):
        for name in files:
            path = os.path.join(root, name)
            with open(path, "rb") as f:
                content = f.read()
            for secret in SECRET_VALUES:
                if not secret:
                    continue
                raw = secret.encode("utf-8")
                json_escaped = json.dumps(secret, ensure_ascii=False)[1:-1].encode("utf-8")
                if raw in content or json_escaped in content:
                    leaks.append(path)
                    break
    return leaks


def run_happy(mcp):
    mcp.call_result("pw_session_ui", {"active": True})
    wait_login_scene(mcp, 600)
    time.sleep(5)  # let the RmlUi document load and show
    screenshot(mcp, "01-login.png")

    mcp.call_result("pw_session_connect")
    snap = wait_session_state(mcp, "character_select", 120)
    roles = snap.get("roles", [])
    if not roles:
        raise RuntimeError("role list is empty on the happy path")
    revision = snap.get("roleListRevision")
    screenshot(mcp, "02-character-select.png")

    first = roles[0]
    other = roles[-1]
    # Select and enter the NOT-first role to prove exact-role routing.
    target = other if other["role_id"] != first["role_id"] else first
    mcp.call_result("pw_session_select", {"role_id": target["role_id"]})
    snap = mcp.call_result("pw_session_status")
    if snap.get("selectedRoleId") != target["role_id"]:
        raise RuntimeError("selection mismatch: {}".format(snap.get("selectedRoleId")))
    screenshot(mcp, "03-role-selected.png")

    mcp.call_result("pw_session_enter")
    snap = wait_session_state(mcp, "in_world", 120)
    if snap.get("attestedRoleId") != target["role_id"]:
        raise RuntimeError("attested role {} != selected {}".format(
            snap.get("attestedRoleId"), target["role_id"]))
    if snap.get("attestedInstanceId") != target["worldtag"]:
        raise RuntimeError("attested instance {} != worldtag {}".format(
            snap.get("attestedInstanceId"), target["worldtag"]))
    if not snap.get("mutationCapable"):
        raise RuntimeError("mutation capability not bound after attested entry")
    screenshot(mcp, "04-in-world.png")

    mcp.call_result("pw_session_disconnect")
    wait_session_state(mcp, "idle", 30)
    return {"role": target["role_id"], "instance": target["worldtag"], "revision": revision,
            "roles": len(roles)}


def run_wrong_password(mcp):
    mcp.call_result("pw_session_ui", {"active": True})
    mcp.call_result("pw_session_connect")
    snap = wait_session_state(mcp, "error", 120)
    if not snap.get("errorCode"):
        raise RuntimeError("error state without a code")
    return {"errorCode": snap.get("errorCode")}


def run_empty_roles(mcp):
    mcp.call_result("pw_session_ui", {"active": True})
    wait_login_scene(mcp, 600)
    mcp.call_result("pw_session_connect")
    snap = wait_session_state(mcp, "character_select", 120)
    if snap.get("roles"):
        raise RuntimeError("expected an empty role list, got {}".format(len(snap["roles"])))
    screenshot(mcp, "05-empty-roles.png")
    mcp.call_result("pw_session_disconnect")
    wait_session_state(mcp, "idle", 30)
    return {"roles": 0}


def wait_preview(mcp, role_id, timeout_s):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        snap = mcp.call_result("pw_session_status")
        preview = snap.get("preview", {})
        last = preview
        if preview.get("serial", 0) > 0 and preview.get("roleId") == role_id:
            return preview
        time.sleep(1)
    raise RuntimeError("timed out waiting for preview of {}; last={}".format(role_id, last))


def preview_signature(preview):
    return {
        "profession": preview.get("profession"),
        "gender": preview.get("gender"),
        "race": preview.get("race"),
        "customPresent": preview.get("customPresent"),
        "colorBody": preview.get("colorBody"),
        "colorHair": preview.get("colorHair"),
        "equipment": preview.get("equipment", []),
        "error": preview.get("error", ""),
    }


def run_preview(mcp, shot_prefix="preview"):
    """WP5 gate: selecting two different roles publishes two different previews."""
    mcp.call_result("pw_session_ui", {"active": True})
    wait_login_scene(mcp, 600)
    time.sleep(5)  # let the RmlUi document load and show

    mcp.call_result("pw_session_connect")
    snap = wait_session_state(mcp, "character_select", 120)
    roles = snap.get("roles", [])
    if len(roles) < 2:
        raise RuntimeError("preview run needs >= 2 roles, got {}".format(len(roles)))
    role_a, role_b = roles[0], roles[-1]

    mcp.call_result("pw_session_select", {"role_id": role_a["role_id"]})
    preview_a = wait_preview(mcp, role_a["role_id"], 60)
    time.sleep(2)  # let the proxy/tint apply and the detail panel sync
    screenshot(mcp, shot_prefix + "-01-role-a.png")

    mcp.call_result("pw_session_select", {"role_id": role_b["role_id"]})
    preview_b = wait_preview(mcp, role_b["role_id"], 60)
    if preview_b.get("serial", 0) <= preview_a.get("serial", 0):
        raise RuntimeError("preview serial did not bump between selections: {} -> {}".format(
            preview_a.get("serial"), preview_b.get("serial")))
    time.sleep(2)
    screenshot(mcp, shot_prefix + "-02-role-b.png")

    sig_a, sig_b = preview_signature(preview_a), preview_signature(preview_b)
    if sig_a == sig_b:
        raise RuntimeError("previews are identical for two roles: {}".format(sig_a))

    # Preview data must never block entry.
    mcp.call_result("pw_session_enter")
    snap = wait_session_state(mcp, "in_world", 120)
    if snap.get("attestedRoleId") != role_b["role_id"]:
        raise RuntimeError("attested role {} != selected {}".format(
            snap.get("attestedRoleId"), role_b["role_id"]))
    screenshot(mcp, shot_prefix + "-03-in-world.png")

    mcp.call_result("pw_session_disconnect")
    wait_session_state(mcp, "idle", 30)
    return {"roleA": role_a["role_id"], "roleB": role_b["role_id"],
            "previewA": sig_a, "previewB": sig_b}


def wait_world_stream(mcp, timeout_s):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        world = mcp.call_result("pw_session_world")
        last = world
        if world.get("seq", 0) > 0:
            return world
        time.sleep(1)
    raise RuntimeError("timed out waiting for the world stream; last={}".format(last))


def run_world(mcp):
    """WP7 gate: world replication live — self stream, nearby markers, gated actions."""
    mcp.call_result("pw_session_ui", {"active": True})
    wait_login_scene(mcp, 600)
    time.sleep(5)

    mcp.call_result("pw_session_connect")
    snap = wait_session_state(mcp, "character_select", 120)
    roles = snap.get("roles", [])
    if not roles:
        raise RuntimeError("world run needs >= 1 role")
    target = roles[-1]
    mcp.call_result("pw_session_select", {"role_id": target["role_id"]})
    mcp.call_result("pw_session_enter")
    snap = wait_session_state(mcp, "in_world", 120)
    if snap.get("attestedRoleId") != target["role_id"]:
        raise RuntimeError("attested role {} != selected {}".format(
            snap.get("attestedRoleId"), target["role_id"]))

    world = wait_world_stream(mcp, 30)
    if world.get("epoch", 0) < 1:
        raise RuntimeError("world epoch never published")
    self0 = world["self"]["position"]
    direction = world["self"]["direction"]
    entities0 = world.get("entities", [])
    time.sleep(3)  # let markers spawn and the follow camera settle
    screenshot(mcp, "world-01-in-world.png")

    # Gated action: move 5m forward; must be accepted and actually displace.
    mcp.call_result("pw_session_world_action", {
        "action": "move_to",
        "x": self0["x"] + direction["x"] * 5.0,
        "y": self0["y"] + direction["y"] * 5.0,
        "z": self0["z"] + direction["z"] * 5.0,
    })
    time.sleep(5)
    world2 = mcp.call_result("pw_session_world")
    if world2.get("error"):
        raise RuntimeError("world action error: {}".format(world2["error"]))
    pos1 = world2["self"]["position"]
    moved = ((pos1["x"] - self0["x"]) ** 2 +
             (pos1["y"] - self0["y"]) ** 2 +
             (pos1["z"] - self0["z"]) ** 2) ** 0.5
    if moved < 0.1:
        raise RuntimeError("move_to accepted but never displaced the character")
    screenshot(mcp, "world-02-moved.png")

    mcp.call_result("pw_session_world_action", {"action": "jump"})
    monsters = [e for e in world2.get("entities", [])
                if e.get("kind") == "monster" and not e.get("dead")]
    targeted = 0
    if monsters:
        nearest = min(monsters, key=lambda e: e.get("dist", 1e9))
        mcp.call_result("pw_session_world_action", {"action": "select_target", "id": nearest["id"]})
        targeted = nearest["id"]
    mcp.call_result("pw_session_disconnect")
    wait_session_state(mcp, "idle", 30)
    return {"role": target["role_id"], "instance": snap.get("attestedInstanceId"),
            "entities": len(entities0), "moved": round(moved, 2), "targeted": targeted}


def probe_terrain_position(mcp, position, label):
    probe = mcp.call_result("terrain_probe", {
        "points": [{"x": position["x"], "z": position["z"], "y": position["y"]}],
    })
    samples = probe.get("samples", [])
    if len(samples) != 1:
        raise RuntimeError("{} terrain_probe returned {} samples, want 1".format(
            label, len(samples)))
    sample = samples[0]
    if not sample.get("ok"):
        raise RuntimeError("{} is outside the loaded terrain region".format(label))
    delta_y = sample.get("delta_y")
    if not isinstance(delta_y, (int, float)):
        raise RuntimeError("{} terrain_probe did not return delta_y: {}".format(label, sample))
    if abs(delta_y) > WORLD_MAP_TERRAIN_TOLERANCE_M:
        raise RuntimeError("{} terrain height mismatch: delta_y={}".format(label, delta_y))
    return sample


def verify_world_map_manifest():
    """Fail closed if the staged slug belongs to a different PW instance."""
    path = os.path.join(PROJECT_DIR, "data", WORLD_MAP_SLUG, "_meta", "manifest.eds.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        raise RuntimeError("cannot read staged instance manifest {}: {}".format(path, exc))
    source_map = str(manifest.get("sourceMap", "")).replace("\\", "/").lower()
    expected_suffix = "/{0}/{0}.ecwld".format(WORLD_MAP_SLUG)
    if manifest.get("instanceId") != WORLD_MAP_INSTANCE_ID:
        raise RuntimeError("staged map instanceId {} != {}".format(
            manifest.get("instanceId"), WORLD_MAP_INSTANCE_ID))
    if not source_map.endswith(expected_suffix):
        raise RuntimeError("staged sourceMap {!r} is not {}".format(
            manifest.get("sourceMap"), expected_suffix))
    return {"instance": manifest["instanceId"], "source_map": source_map}


def run_worldmap_offline(mcp):
    """Load the converted instance-161 region and prove its absolute terrain placement."""
    manifest = verify_world_map_manifest()
    mcp.call_result("pw_session_ui", {"active": True})
    wait_login_scene(mcp, WORLD_MAP_LOAD_TIMEOUT_S)

    mcp.call_result("load_login", {
        "content_root": WORLD_MAP_SLUG + ":",
        "map": WORLD_MAP_SLUG,
    })
    status = wait_map_load(mcp, WORLD_MAP_SLUG, WORLD_MAP_LOAD_TIMEOUT_S)
    if not status.get("terrain"):
        raise RuntimeError("instance map load completed without terrain")

    ground_points = list(WORLD_MAP_OFFLINE_GROUND_POINTS)
    probe = mcp.call_result("terrain_probe", {
        "points": ground_points + [{"x": 2000.0, "z": 2000.0}],
    })
    samples = probe.get("samples", [])
    if len(samples) != len(ground_points) + 1:
        raise RuntimeError("terrain_probe returned {} samples, want {}".format(
            len(samples), len(ground_points) + 1))
    ground_samples = samples[:-1]
    for index, sample in enumerate(ground_samples):
        if not sample.get("ok"):
            raise RuntimeError("known instance-161 ground point {} is outside terrain".format(index))
        delta_y = sample.get("delta_y")
        if not isinstance(delta_y, (int, float)):
            raise RuntimeError("ground terrain_probe did not return delta_y: {}".format(sample))
        if abs(delta_y) > WORLD_MAP_TERRAIN_TOLERANCE_M:
            raise RuntimeError("ground point {} height mismatch: delta_y={}".format(index, delta_y))
    if samples[-1].get("ok"):
        raise RuntimeError("terrain probe at (2000, 2000) must be outside the region")

    focus = ground_points[0]
    mcp.call_result("camera_set", {
        "pos": [focus["x"], focus["y"] + 28.0, focus["z"] - 40.0],
        "target": [focus["x"], focus["y"], focus["z"]],
    })
    time.sleep(2)
    # ui=False: the active PW login document's screen-space composite currently
    # blacks out the whole frame (post-merge regression); worldmap evidence is
    # the world itself, not the login overlay.
    screenshot(mcp, "worldmap-offline-01-spawn.png", ui=False)
    mcp.call_result("camera_set", {
        "pos": [211.0, 420.0, -200.0],
        "target": [211.0, 40.0, 200.0],
    })
    time.sleep(2)
    screenshot(mcp, "worldmap-offline-02-overview.png", ui=False)
    return {
        "map": status.get("map"),
        "manifest": manifest,
        "probe_delta_y": [round(sample["delta_y"], 3) for sample in ground_samples],
        "buildings": status.get("created", 0),
        "foliage": status.get("foliage_created", 0),
        "water": status.get("water_created", 0),
    }


def wait_world_self(mcp, role_id, instance_id, timeout_s):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        world = mcp.call_result("pw_session_world")
        last = world
        if world.get("error"):
            raise RuntimeError("world stream error: {}".format(world["error"]))
        self_state = world.get("self", {})
        if (world.get("seq", 0) > 0 and self_state.get("roleId") == role_id and
                self_state.get("instanceId") == instance_id):
            return world
        time.sleep(1)
    raise RuntimeError("timed out waiting for attested world self; last={}".format(last))


def wait_for_displacement(mcp, origin, direction_x, direction_z,
                          minimum_distance, timeout_s):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        world = mcp.call_result("pw_session_world")
        last = world
        if world.get("error"):
            raise RuntimeError("world action error: {}".format(world["error"]))
        position = world.get("self", {}).get("position", {})
        if all(isinstance(position.get(axis), (int, float)) for axis in ("x", "y", "z")):
            delta_x = position["x"] - origin["x"]
            delta_z = position["z"] - origin["z"]
            moved = (delta_x ** 2 + delta_z ** 2) ** 0.5
            progress = delta_x * direction_x + delta_z * direction_z
            if moved >= minimum_distance and progress > 0.0:
                return world, moved
        time.sleep(1)
    raise RuntimeError("move_to never displaced the character by {:.2f}m; last={}".format(
        minimum_distance, last))


def run_worldmap(mcp):
    """Enter a live instance-161 role and prove terrain-aligned movement."""
    manifest = verify_world_map_manifest()
    mcp.call_result("pw_session_ui", {"active": True})
    wait_login_scene(mcp, WORLD_MAP_LOAD_TIMEOUT_S)
    time.sleep(5)

    mcp.call_result("pw_session_connect")
    snap = wait_session_state(mcp, "character_select", 120)
    candidates = [
        role for role in snap.get("roles", [])
        if role.get("role_id") == WORLD_MAP_ROLE_ID
        and role.get("worldtag") == WORLD_MAP_INSTANCE_ID
        and isinstance(role.get("role_id"), int)
        and role.get("role_id") > 0
        and not role.get("deleting", False)
    ]
    if not candidates:
        available = [
            {"role_id": role.get("role_id"), "worldtag": role.get("worldtag"),
             "deleting": role.get("deleting", False)}
            for role in snap.get("roles", [])
        ]
        raise RuntimeError("no enterable role for instance {}; available={}".format(
            WORLD_MAP_INSTANCE_ID, available))
    target = candidates[0]
    role_id = target["role_id"]
    mcp.call_result("pw_session_select", {"role_id": role_id})
    selected = mcp.call_result("pw_session_status")
    if selected.get("selectedRoleId") != role_id:
        raise RuntimeError("selection mismatch: {} != {}".format(
            selected.get("selectedRoleId"), role_id))
    mcp.call_result("pw_session_enter")
    snap = wait_session_state(mcp, "in_world", 120)
    if snap.get("attestedRoleId") != role_id:
        raise RuntimeError("attested role {} != selected {}".format(
            snap.get("attestedRoleId"), role_id))
    if snap.get("attestedInstanceId") != WORLD_MAP_INSTANCE_ID:
        raise RuntimeError("attested instance {} != {}".format(
            snap.get("attestedInstanceId"), WORLD_MAP_INSTANCE_ID))

    status = wait_map_load(mcp, WORLD_MAP_SLUG, WORLD_MAP_LOAD_TIMEOUT_S)
    if not status.get("terrain"):
        raise RuntimeError("instance map load completed without terrain")

    mcp.call_result("pw_session_world_action", {"action": "stop_move"})
    time.sleep(2)
    world = wait_world_self(mcp, role_id, WORLD_MAP_INSTANCE_ID, 60)
    self0 = world["self"]["position"]
    if not all(isinstance(self0.get(axis), (int, float)) for axis in ("x", "y", "z")):
        raise RuntimeError("world self position is incomplete: {}".format(self0))
    sample0 = probe_terrain_position(mcp, self0, "world self")
    time.sleep(3)
    screenshot(mcp, "worldmap-01-in-world.png", ui=False)

    direction = world["self"].get("direction", {})
    direction_x = direction.get("x", 0.0)
    direction_z = direction.get("z", 0.0)
    horizontal_length = (direction_x ** 2 + direction_z ** 2) ** 0.5
    if horizontal_length < 0.001:
        direction_x, direction_z, horizontal_length = 1.0, 0.0, 1.0
    direction_x /= horizontal_length
    direction_z /= horizontal_length
    target_x = self0["x"] + direction_x * WORLD_MAP_MOVE_DISTANCE_M
    target_z = self0["z"] + direction_z * WORLD_MAP_MOVE_DISTANCE_M
    mcp.call_result("pw_session_world_action", {
        "action": "move_to",
        "x": target_x,
        "y": self0["y"],
        "z": target_z,
    })
    world2, moved = wait_for_displacement(
        mcp, self0, direction_x, direction_z, WORLD_MAP_MIN_DISPLACEMENT_M, 20)
    pos1 = world2["self"]["position"]
    sample1 = probe_terrain_position(mcp, pos1, "moved world self")
    time.sleep(2)
    screenshot(mcp, "worldmap-02-moved.png", ui=False)

    mcp.call_result("pw_session_disconnect")
    wait_session_state(mcp, "idle", 30)
    return {
        "role": role_id,
        "eligible_roles": len(candidates),
        "instance": WORLD_MAP_INSTANCE_ID,
        "map": status.get("map"),
        "manifest": manifest,
        "moved": round(moved, 2),
        "probe_delta_y": round(sample0["delta_y"], 3),
        "probe_delta_y_after_move": round(sample1["delta_y"], 3),
        "entities": len(world2.get("entities", [])),
    }


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    if mode not in SUPPORTED_MODES:
        print("unknown mode: {}; choose one of {}".format(mode, "|".join(SUPPORTED_MODES)))
        return 1
    if mode not in OFFLINE_MODES:
        for required in ("PW_ACCOUNT", "PW_PASSWORD", "PW_SERVER",
                         "PW_RUNTIME_CLIENT_EXE", "PW_RUNTIME_WORKING_DIRECTORY",
                         "UNRAVEL_PW_PROJECT"):
            if not os.environ.get(required):
                print("missing env: {}".format(required))
                return 1
        SECRET_VALUES.extend((os.environ["PW_ACCOUNT"], os.environ["PW_PASSWORD"],
                              os.environ["PW_SERVER"]))
    else:
        # Offline modes need only converted project assets; preview-offline adds
        # its fake sidecar below, while worldmap-offline starts no PW session.
        for required in ("UNRAVEL_PW_PROJECT",):
            if not os.environ.get(required):
                print("missing env: {}".format(required))
                return 1
        if mode == "preview-offline":
            os.environ.setdefault("PW_SERVER", "fake.local:29000")
        SECRET_VALUES.extend(
            os.environ[key] for key in ("PW_ACCOUNT", "PW_PASSWORD", "PW_SERVER")
            if os.environ.get(key)
        )
    os.makedirs(OUT_DIR, exist_ok=True)

    results = {}
    failures = 0

    def one(name, account, password, fn, client_exe=None, workdir=None,
            headless_editor=False):
        nonlocal failures
        print("=== {}".format(name))
        # A stale editor from a previous run would own the control port and its
        # environment (credentials), silently hijacking this run.
        if wait_for_port(MCP_PORT, 2):
            subprocess.run(["taskkill", "/IM", "UnravelEditor.exe", "/F"],
                           capture_output=True)
            time.sleep(3)
            if wait_for_port(MCP_PORT, 5):
                results[name] = "FAIL: control port busy with a foreign editor"
                failures += 1
                return
        # A hard-terminated editor leaves its headless client orphaned and still
        # logged in; a same-account relogin then races the stale server session.
        effective_client_exe = None if headless_editor else (client_exe or CLIENT_EXE)
        if effective_client_exe:
            subprocess.run(["taskkill", "/IM", os.path.basename(effective_client_exe), "/F"],
                           capture_output=True)
        proc = (launch_editor_headless() if headless_editor
                else launch_editor(account, password, client_exe, workdir))
        try:
            if not wait_for_port(MCP_PORT, 300):
                raise RuntimeError("editor control port did not come up")
            mcp = EditorMcp(MCP_PORT, timeout=MCP_CALL_TIMEOUT_S)
            try:
                results[name] = fn(mcp)
                print("PASS {} -> {}".format(name, results[name]))
            finally:
                save_transcript(mcp, "transcript-{}.json".format(name))
                mcp.close()
        except Exception as exc:  # noqa: BLE001 - acceptance runner reports any failure
            failures += 1
            results[name] = "FAIL: {}".format(redact(str(exc)))
            print("FAIL {}: {}".format(name, redact(str(exc))))
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
            if effective_client_exe:
                # The client (overridden sidecar or real) outlives a
                # hard-terminated editor just like the real client does.
                subprocess.run(["taskkill", "/IM", os.path.basename(effective_client_exe), "/F"],
                               capture_output=True)
            # Wait until the control port is really closed before the next run.
            deadline = time.time() + 30
            while time.time() < deadline and wait_for_port(MCP_PORT, 1):
                time.sleep(1)

    if mode in ("happy", "all"):
        one("happy", os.environ["PW_ACCOUNT"], os.environ["PW_PASSWORD"], run_happy)
    if mode in ("preview", "all"):
        one("preview", os.environ["PW_ACCOUNT"], os.environ["PW_PASSWORD"], run_preview)
    if mode in ("world", "all"):
        one("world", os.environ["PW_ACCOUNT"], os.environ["PW_PASSWORD"], run_world)
    if mode in ("worldmap",):
        one("worldmap", os.environ["PW_ACCOUNT"], os.environ["PW_PASSWORD"], run_worldmap)
    if mode in ("preview-offline",):
        # WP5 visual gate without the live server: the editor's session stack
        # drives the fake sidecar (preview scenario), so the two fixture roles
        # publish visually distinct previews (orange warrior vs green mage).
        import shutil
        import tempfile
        fake_exe = os.environ.get(
            "PW_FAKE_SIDECAR_EXE",
            os.path.join(REPO_ROOT, "build", "bin", "RelWithDebInfo", "pw_runtime_fake_server.exe"))
        tmp = tempfile.mkdtemp(prefix="pw_preview_offline_")
        with open(os.path.join(TESTS_DIR, "fixtures", "scenarios", "preview.json"),
                  encoding="utf-8") as f:
            scenario = json.load(f)
        # The controller polls pw.status every 500ms in character_select and
        # fails closed on an in_world phase without a local operation, so pin
        # the character_select snapshot for the whole selection window and let
        # only the post-enter polls observe in_world.
        responses = scenario["tools"]["pw.status"]["responses"]
        scenario["tools"]["pw.status"]["responses"] = (
            responses[:3] + [responses[2]] * 120 + [responses[-1]])
        with open(os.path.join(tmp, "pw_fake_scenario.json"), "w", encoding="utf-8") as f:
            json.dump(scenario, f)
        try:
            one("preview-offline", "offline_preview", "offline_preview_not_a_secret",
                lambda mcp: run_preview(mcp, "preview-offline"),
                client_exe=fake_exe, workdir=tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if mode in ("worldmap-offline",):
        one("worldmap-offline", None, None, run_worldmap_offline, headless_editor=True)
    if mode in ("wrong-password",):
        # NOTE: not part of "all" — the spw test authd accepts any password
        # (verified live 2026-08-14), so a wrong-password rejection can only be
        # proven on the fake sidecar (run_fake_sidecar_scenarios.py).
        one("wrong-password", os.environ["PW_ACCOUNT"], "definitely-wrong-password-1", run_wrong_password)
    if mode in ("empty-roles", "all"):
        empty_account = os.environ.get("PW_ACCEPTANCE_ACCOUNT_EMPTY")
        empty_password = os.environ.get("PW_ACCEPTANCE_PASSWORD_EMPTY")
        if not empty_account or not empty_password:
            print("SKIP empty-roles: set PW_ACCEPTANCE_ACCOUNT_EMPTY / PW_ACCEPTANCE_PASSWORD_EMPTY")
        else:
            SECRET_VALUES.extend((empty_account, empty_password))
            one("empty-roles", empty_account, empty_password, run_empty_roles)

    leaks = sweep_leaks()
    if leaks:
        failures += 1
        print("CREDENTIAL LEAK in artifacts: {}".format(redact(str(leaks))))

    summary = {"date": datetime.date.today().isoformat(), "results": results,
               "credential_leaks": len(leaks), "out": OUT_DIR}
    with open(os.path.join(OUT_DIR, "results.json"), "w", encoding="utf-8") as f:
        json.dump(redact_value(summary), f, indent=1)
    print("SUMMARY: {}".format(redact(json.dumps(summary))))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
