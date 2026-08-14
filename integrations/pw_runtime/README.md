# Perfect World runtime bridge

This optional Windows integration gives Unravel a stable C ABI for controlling
the existing headless `ElementClient` without linking legacy PW or Angelica
types into the editor.

## Current backend

`PWRuntimeBridge.dll` owns one headless client process and transports MCP
JSON-RPC over inherited stdin/stdout pipes. It performs the MCP initialize
handshake before reporting the runtime as started. The ABI is versioned and
uses only fixed-width integers, opaque handles, UTF-8 byte spans, callbacks,
and caller-owned storage.

The bridge passes a least-privilege environment to the child. Credentials are
never accepted as MCP tool arguments and unrelated controller, cloud, source
control, and LLM environment variables are not forwarded.

Unravel exposes these tools:

- `pw_runtime_start` starts the executable configured by
  `PW_RUNTIME_CLIENT_EXE` and optionally `PW_RUNTIME_WORKING_DIRECTORY`.
- `pw_runtime_stop` stops the child owned by the editor.
- `pw_runtime_status` returns DLL/process state and the native `pw.status`
  snapshot when the child is running.
- `pw_runtime_call` proxies the same read-only allowlist enforced by the
  existing headless agent (now including `pw.role_list`).
- `pw_runtime_role_enter` is the single typed mutation path: it drives native
  `pw.role_enter` for one exact role of a current role-list revision and only
  reports success after exact role/worldtag attestation.

Mutating `pw.*` calls other than the typed role-enter path are denied. This is
a deliberate fail-closed boundary.

## ABI v2 and role-admin start (WP2)

`pw_runtime_get_api` negotiates `PW_RUNTIME_ABI_VERSION_V1` (legacy table) or
`PW_RUNTIME_ABI_VERSION_V2`. The v2 table extends v1 with `start_v2`, which
accepts `pw_runtime_start_info_v2`:

- `role_admin=1` starts the child in character-select mode
  (`PW_HEADLESS_ROLE_ADMIN=1`); with `role_admin=0` an optional
  `exact_role_id` maps to `PW_HEADLESS_ROLE_ID`.
- Account/password are in-memory UTF-8 byte spans. The bridge converts them
  into the child environment block only; they never pass through MCP JSON, the
  process command line, or logs, and every temporary copy is securely erased
  after `CreateProcess`. `server` optionally sets `PW_SERVER`.

The editor client (`pw_runtime_client`) exposes `start_role_admin(...)` and
typed `enter_role(role_id, role_list_revision, expected_worldtag,
operation_id)`. A completed enter grants a mutation capability bound to the
child PID, exact role id and world instance; it is revoked by stop/start,
process restart, or any live role/world mismatch (`has_mutation_capability()`
revalidates against a fresh `pw.status`).

`tests/run_bridge_sidecar_tests.py` drives the DLL through ctypes against the
fake sidecar and covers v1 compatibility, the v2 credential span plumbing
(presence/length probe only - never values), list, enter pending/done,
fail-closed stale revision, request timeout, mid-flow crash, and restart:

```powershell
py -3 integrations/pw_runtime/tests/run_bridge_sidecar_tests.py
```

## Session controller and editor UI (WP3/WP4)

`editor/editor/mcp/pw_session_controller.{h,cpp}` owns the interactive login
flow for the editor: one worker thread performs every bridge call; the UI reads
a mutex-protected snapshot and posts intents (`connect`, `select_role`,
`enter_selected_role`, `disconnect`, `cancel`). States follow the authoritative
`pw.status` login object and typed operation results only
(`idle -> starting -> authenticating -> role_list_loading -> character_select ->
selecting_role -> entering_world -> in_world`; `error` is terminal with a
machine-readable code). Timers can only produce errors, never success.
`in_world` is published atomically with the exact role/instance attestation and
the mutation capability. A role-list revision change clears the selection.

The RmlUi overlay (`editor_data/data/ui/pw_login.rhtml` + `.rcss`, hosted by
`editor/editor/pwlogin/pw_login_ui.{h,cpp}` and the `pw_login_panel` viewport)
only renders the public snapshot and forwards user intents. The password field
is read on Connect, handed to the controller in-process, and cleared
immediately; it is never persisted. The mode toggles from the editor menu
`Perfect World -> Login / Character Select`.

The same session surface is available to automation as MCP tools / control
methods: `pw_session_ui`, `pw_session_status`, `pw_session_connect`,
`pw_session_select`, `pw_session_enter`, `pw_session_disconnect`. Connect takes
no arguments: credentials come only from the editor process environment.

`tests/run_editor_acceptance.py` is the live acceptance driver (WP6): it
launches the editor with the converted login-scene project, walks the full
route with screenshots and a redacted transcript, and sweeps artifacts for
credential leaks. All configuration is via environment; see the script header.

It uses `PW_RUNTIME_BRIDGE_DLL` (defaults to `build/bin/RelWithDebInfo`) and
the same fake-sidecar env configuration as the scenario runner. Bridge-level
scenario selection uses `pw_fake_scenario.json` in the child working
directory because the scrubbed child environment does not forward
`PW_FAKE_SCENARIO`.

`tests/run_session_controller_tests.py` covers the editor session controller
(`editor/editor/mcp/pw_session_controller.{h,cpp}`) end to end: the test
executable links the real controller and `pw_runtime_client`, loads the real
bridge DLL, and drives scripted intent/assertion scripts against the fake
sidecar (scenario selected via `pw_fake_scenario.json` in a per-test
temporary working directory). The 14 tests cover the full login/select/enter
route, role preview fetch with distinct per-role previews, non-blocking
preview failure, no-timer-jump, wrong password, empty roles, stale revision
with selection reset, mid-enter disconnect, role/world mismatch fail-closed,
user disconnect from character select and mid-enter, retry after disconnect,
and the hung-call timeout path:

```powershell
cmake --build build --config RelWithDebInfo --target pw_session_controller_test
py -3 integrations/pw_runtime/tests/run_session_controller_tests.py
```

Configuration comes from the environment: `PW_SESSION_TEST_EXE`,
`PW_RUNTIME_BRIDGE_DLL`, `PW_FAKE_SIDECAR_EXE`, `PW_FAKE_FIXTURES_DIR`
(all with repo-relative defaults).

## Role preview (WP5)

Selecting a role posts a read-only `pw.role_preview` fetch
(`pw_session_controller::fetch_preview`): the native client decodes the role's
customization scalars (profession/gender/race, body/hair colors, face ids) and
its equipment list, and the controller publishes them as `snapshot.preview`.
The preview `serial` is a monotonic publish counter (0 = none); a selection,
a role-list revision change, or a disconnect clears the preview. Failures
surface only as `preview.error` — they never block select/enter.
`pw_session_status` exposes the preview as normalized read-only scalars; raw
`custom_data`/equipment blobs never leave the native process.

The login UI renders the preview two ways:

- The detail panel shows the equipment list (`pw-detail-equip` in
  `editor_data/data/ui/pw_login.rhtml`), synced on the preview serial.
- A 3D proxy entity ("PW Role Preview") spawns at the `[NewChar] Pos0` from
  scenectrl.ini, grounded on the login terrain, facing the create camera,
  tinted with the role's `colorBody`.

Mesh mapping (`pw_login_ui::proxy_mesh_ref_for`): every (profession, gender)
pair currently resolves to the single staged bare-body export
`characters/player/model_57f7b4eb.gltf` under the login content root (武侠男
body exported from the client data with ECModelViewer). Per-class exports slot
into that function without touching the flow. A missing or uncompiled mesh
waits up to 600 frames for the asset watcher, then keeps the previous proxy
and logs `pw preview: mesh ... unavailable` — the flow is never blocked.

Tests: the `preview` fake scenario (WP0 runner) pins the tool contract; the
`preview_fetch` / `preview_error_nonblocking` session tests prove two roles
publish distinct previews and that a preview failure cannot block entry; the
live `preview` acceptance mode (`run_editor_acceptance.py preview`) selects
two roles on the test server, asserts their previews differ (serial bump plus
content), screenshots both, and enters the world.

## World replication (WP7)

While the session is `in_world`, the controller streams a world snapshot
(`pw_session_world`): `seq` is a monotonic publish counter, `epoch` bumps on
every world entry (reconnect/resync marker — consumers drop all replicated
entities of the old epoch). `self` rides the authoritative `pw.status` poll
(position, direction, hp/mp, level, dead/moving, target); `entities` is the
complete nearest-first set from a read-only `pw.nearby` poll (~1 Hz, range
60). Stream failures only record `world.error`; they never break the session.
`pw_session_world` (MCP/control) exposes the full snapshot;
`pw_session_status` carries a summary.

The editor replicates the snapshot into the scene
(`pw_login_ui::service_world_view`): one flat-color capsule marker per nearby
entity (monster red / npc green / player blue / matter yellow, dead monsters
gray), a white self marker, and the MCP camera follows the self marker.
Markers are keyed by (epoch, id); an epoch bump wipes and re-creates them.

World actions use a typed gated path, never the generic read-only channel:
`pw_runtime_client::call_mutation` forwards only `pw.move_to`, `pw.stop_move`,
`pw.jump`, `pw.select_target`, `pw.normal_attack`, and only while the
attested mutation capability (child PID + exact role/instance) revalidates
live. The controller exposes them as the `world_action` intent (in_world
only; outcome in `world.error`), the automation surface as
`pw_session_world_action`, and the in-world UI screen as
Forward/Stop/Jump/Target/Attack buttons.

Tests: the `world` fake scenario pins the `pw.nearby`/action contracts; the
`world_stream` and `world_actions` session tests cover the replication
snapshot (self, entity set, epoch clear on disconnect) and the gated actions
(rejected before attestation, accepted in_world); the live `world` acceptance
mode enters the world on the test server, verifies the stream, screenshots
the markers, and moves the character 5 m forward via the gated action.

## Next backend gate

## Build and smoke

Configure with `UNRAVEL_BUILD_PW_RUNTIME_BRIDGE=ON`, then build the editor. The
DLL is emitted next to `UnravelEditor`.

The standalone smoke target verifies ABI discovery, child lifecycle, MCP
initialize, `pw.status`, response framing, and shutdown:

```powershell
cmake --build build --config Release --target pw_runtime_bridge pw_runtime_fake_server pw_runtime_bridge_smoke
build/bin/Release/pw_runtime_bridge_smoke.exe `
  build/bin/Release/PWRuntimeBridge.dll `
  build/bin/Release/pw_runtime_fake_server.exe
```

The same smoke accepts a real headless executable and a separate element root
as its optional third argument. Do not place credentials or server addresses
on the command line.

## Next backend gate

The ABI intentionally does not expose the sidecar implementation. A future
native x64 `PWRuntimeCore.dll` can implement the same contract after every
legacy dependency has reproducible x64 provenance. Before enabling mutations,
the host must also prove entry into the exact requested positive role and
revoke authority on role, world, or session mismatch.

## Contract fixtures and scenario tests (WP0)

`tests/fixtures/` captures the native headless-client MCP contract so the
bridge login flow can be tested without a live PW server:

- `baseline-pw.status-schema.json` / `baseline-pw.role_list-schema.json` -
  current `inputSchema` objects captured from the native
  `EC_MCPServer.cpp MCP_BuildToolsList`.
- `baseline-tools-list.json` - the full 55-tool `tools/list` baseline
  (pre-`role_enter`), served by the fake sidecar in scenario mode.
- `status-structure.md` - field-level documentation of the `pw.status`
  `state` object per login situation, including the post-WP1 `login` object.
- `scenarios/*.json` - scripted per-tool response queues plus a `drive` step
  list and the expected terminal error code (`null` on success): `success`,
  `wrong_password`, `empty_roles`, `stale_list`, `disconnect`,
  `role_mismatch`, `world_mismatch`.

`tests/pw_runtime_fake_server.cpp` runs in two modes. With no configuration
it keeps the original static behavior used by `pw_runtime_bridge_smoke`. With
`PW_FAKE_SCENARIO` (or argv[1]) pointing at a scenario file - or a
`pw_fake_scenario.json` in its working directory, which is how bridge-level
tests select scenarios - it answers `initialize` / `tools/list`, replays
scripted `tools/call` responses in order per tool (the last response repeats
for polling), and can close the pipe after a fixed number of answered calls
to simulate a crash, which the runner detects as `connection_lost`, or accept
a call and never answer it (`hang_after_calls`) to exercise timeout paths.
Scenario mode also writes `pw_fake_env_probe.json` with presence/length flags
for the credential-bearing variables so tests can prove the credential
plumbing without recording any secret value.

Run all scenarios:

```powershell
py -3 integrations/pw_runtime/tests/run_fake_sidecar_scenarios.py
```

The runner is Python 3 stdlib-only. All test configuration is portable and
comes from the environment:

- `PW_FAKE_SIDECAR_EXE` - fake server executable; defaults to the first
  existing `build/bin/<Config>/pw_runtime_fake_server.exe`.
- `PW_FAKE_FIXTURES_DIR` - fixtures root; defaults to the runner's own
  `fixtures/` directory.
- `PW_FAKE_SCENARIO` - consumed by the fake server itself; the runner sets
  it per scenario.

Fixtures carry only fictitious role ids/names and never real account data.
Live acceptance configuration (server address, account credentials) comes
only from the caller's environment at run time and is never committed.
