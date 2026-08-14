# pw.status state structure (baseline, pre-role_enter)

Captured from the native headless client `EC_MCPServer.cpp`
(`MCP_BuildStateJson` / `MCP_BuildToolResult`). This documents the `state`
object as the native sidecar serves it today, plus the post-WP1 `login`
addition that the scenario fixtures already exercise.

## Envelope

Every `tools/call` response has the shape:

```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"<escaped structuredContent string>"}],"structuredContent":{...},"isError":false}}
```

`structuredContent` is `{"ok":bool,"errorCode":string,"message":string,"state":<state object>,"payload":{...}?}`:

- `ok` / `errorCode` / `message`: tool outcome. Tool errors have `ok=false`,
  a machine-readable `errorCode`, and `isError=true` at the envelope level.
- `state`: the snapshot below; always present, also on errors.
- `payload`: optional per-tool object (`pw.role_list` role array, deferred
  operation progress, ...). Absent when the tool produced no payload. `pw.role_list`
  role entries carry `role_id,name,profession,gender,race,level,level2,status,
  deleting,create_time,delete_time,worldtag` (`worldtag` added post-WP1 WP3a: the
  role's stored world/instance tag, needed for editor-side enter attestation).

## state top-level keys

| Key | Type | Meaning |
|-----|------|---------|
| `connected` | bool | network link to the game/login server is up |
| `inWorld` | bool | client is in game state (`GS_GAME`) with a live role |
| `gameState` | int | stock `CECGameRun` ordinal: 0 `GS_NONE`, 1 `GS_LOGIN`, 2 `GS_GAME` |
| `roleId` | int | live in-world role id; 0 when not in world |
| `roleName` | string | live in-world role name; "" when not in world |
| `selectedRole` | object | character-select highlight: `{id,worldtag,status,level,position{x,y,z}}`; all zeros when nothing is selected |
| `roleManagement` | object | role-admin op progress: `{state,complete,createdRoleId,targetStatuses,lastError}` |
| `instanceId` | int | live world/instance tag; 0 when not in world |
| `position` | object | live host position `{x,y,z}` |
| `direction` | object | live facing vector `{x,y,z}` |
| `lastSentPosition` | object | last position pushed to the server `{x,y,z}` |
| `lastServerCorrection` | object | `{valid,x,y,z,ageMs,count}` server correction telemetry |
| `moving`/`grounded`/`jumping`/`flying`/`falling`/`swimming` | bool | movement flags |
| `moveEnv` / `moveEnvName` | int/string | 0 ground, 1 water, 2 air |
| `moveMode` / `moveModeName` | int/string | 0 stand, 1 move, 2 jump, 3 freefall, 4 slide |
| `work` | object | current work/action `{id,name}` |
| `terrain` | object | `{available,height,heightAbove}` |
| `collision` | object | `{available,providers,brushes,blockedCount,aheadBlocked,fraction,normal{x,y,z},probeDistanceCm,probes[[dirX,dirZ,fraction]permille]}` |
| `navigation` | object | `{state,stateName,usage,usageName,destination,waypoint,nodesRemaining,distance}`; stateName idle/moving/path_finished, usageName none/move/trace |
| `targetId` | int | current selected target id; 0 = none |
| `serviceNpcId` | int | active NPC service object id; 0 = none |
| `safeLock` | object | `{active,end,remaining,total}` stock safe-lock timers |
| `dead` | bool | character dead flag |
| `hp`/`maxHp`/`mp`/`maxMp` | int | live combat stats; 0 when not in world |
| `level` / `exp` | int | live level/exp; 0 when not in world |

## state by login situation

- **offline**: `connected=false`, `inWorld=false`, `gameState=0`,
  `roleId=0`, `roleName=""`, `selectedRole` all zeros, `instanceId=0`,
  positions zero, `hp/mp/level/exp` zero.
- **authenticating**: link up (`connected=true`) but still `inWorld=false`;
  no role data yet.
- **character select**: `connected=true`, `inWorld=false`, `gameState=1`,
  `selectedRole` populated (`id`, `worldtag`, `status`, `level`, `position`)
  once a role is highlighted; `roleManagement` reflects pending create /
  delete / undelete operations.
- **in world**: `connected=true`, `inWorld=true`, `gameState=2`; `roleId`,
  `roleName`, `instanceId` (== entered worldtag), `position`, `direction`,
  `hp/mp/level/exp` are live.

## login object (post-WP1)

WP1 adds a `login` object to `state`:

```json
"login":{"phase":"offline|authenticating|character_select|entering_world|in_world","roleListReady":false,"roleListRevision":0,"expectedRoleId":0,"expectedWorldTag":0,"lastError":""}
```

- `phase` is the factual lifecycle phase only; it never encodes failure.
- `roleListReady` / `roleListRevision`: authoritative role-list state; the
  revision is monotonic per process and bumps when a full list is accepted
  and after create/delete/undelete completion (mirrors the `revision` field
  WP1 adds to the `pw.role_list` payload).
- `expectedRoleId` / `expectedWorldTag`: the role/worldtag a deferred
  `pw.role_enter` is attesting against; 0 = none. Cleared after attested
  entry or terminal failure.
- `lastError`: terminal login failure code (e.g. `account_login_failed`,
  `login_server_rejected`); "" = none. Phase stays factual while
  `lastError` carries the failure.
