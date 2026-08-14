# Upstream Update - 2026-08-13

## Current Status

- The in-progress merge now targets official `upstream/main` at
  `207b3ee6ca5fff1880a01cd647f83ed84b13136d` (`Reverted a deleted line`).
- The upstream delta from the previous merge target was 59 commits and 1,224 files.
- The existing Perfect World runtime bridge remains an unstaged local integration.
- The merge is intentionally not committed, staged further, pushed, or opened as a PR.

## Completed

- Fetched `origin` and `upstream` and applied the latest upstream delta.
- Resolved `asset_compiler.cpp` by retaining KTX2 handling and adding upstream
  equirectangular texture handling.
- Retained the PW runtime CMake dependency while adding upstream `PLAYER_NAME`
  definitions.
- Synchronized and initialized all submodules, including the new `cpp-httplib`
  submodule.

## Verification

- `cmake -S . -B build` - PASS with the existing Visual Studio generator and
  `UNRAVEL_BUILD_PW_RUNTIME_BRIDGE=ON`.
- `cmake --build build --config RelWithDebInfo --target engine editor game engine_data editor_data pw_runtime_bridge pw_runtime_bridge_smoke -- /m` - PASS.
- `cmake --build build --config RelWithDebInfo --target pw_runtime_fake_server -- /m` - PASS.
- `pw_runtime_bridge_smoke.exe PWRuntimeBridge.dll pw_runtime_fake_server.exe` - PASS.
- `UnravelEditor.exe` launch from `build/bin` runtime - PASS; process remained
  alive for 25 seconds and was stopped by its exact PID.

## Open Gates

- Real PW headless/server login was not repeated in this update; only the
  deterministic fake-sidecar bridge acceptance was run.
- A merge commit is still required before this branch has durable ancestry to
  `207b3ee6`; creating it requires explicit user authorization.

## Next Steps

- Repeat the real headless login/status smoke against the test server if live
  integration acceptance is required.
- Review and commit the merged upstream update and PW bridge as separate commits.
