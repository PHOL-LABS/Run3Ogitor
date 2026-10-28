# Run3Ogitor MCP

An MCP server for authoring **native Run3 levels** in the running Ogitor editor.
Python 3.10+ is the only server requirement; no pip packages or API keys are needed.
The editor plugin needs Qt Network in addition to the existing Ogre/Qt dependencies.
`build.bat` builds and deploys it with the native Run3 serializer.

## Connect Codex

1. Build Ogitor and launch `run.bat`.
2. In the **Run3 MCP** toolbar, choose a port (default **9877**) and click
   **Connect**. The status changes to `Listening on 127.0.0.1:9877`.
3. Register the stdio server with Codex, using absolute paths:

   ```powershell
   codex mcp add run3ogitor -- python C:/Run3-Game-Engine/Run3Ogitor/tools/run3ogitor_mcp/server.py --port 9877
   ```

4. Restart the Codex session/extension, then inspect `/mcp` or the MCP servers UI.
   Ask it to call `editor_status` before editing. The server can initialize and
   list its tools while Ogitor is disconnected; editor calls return instructions
   to connect instead of failing server startup.

The same command works with other MCP clients supporting stdio. A TOML example:

```toml
[mcp_servers.run3ogitor]
command = "python"
args = ["C:/Run3-Game-Engine/Run3Ogitor/tools/run3ogitor_mcp/server.py", "--port", "9877"]
tool_timeout_sec = 120
```

Use an absolute Python executable if `python` is unavailable in the client's PATH.
Codex supports stdio servers, CLI registration and `[mcp_servers.name]` configuration;
see [official OpenAI MCP documentation](https://developers.openai.com/codex/mcp/).
This server implements newline-delimited JSON-RPC MCP, including initialize/version
negotiation, notifications, ping, tool discovery and text/image tool results.
See the [MCP stdio specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).

## Authoring workflow

Call `editor_status`, then open a map or create a new one. For this checkout:

```json
{
  "name": "courtyard",
  "content_root": "C:/Run3-Game-Engine/Games/The Long Way/TheLongWay",
  "output_root": "C:/Run3-Game-Engine/Run3Ogitor/artifacts/my-levels"
}
```

Pass this to `level_create`. Create `output_root` first. It writes a new level
under `run3/maps/high/courtyard` in that overlay, with `scene.cfg`, `scene.xml`
and `sequence.xml`. The base game's assets are reused. The level must not already
exist in any selected content root. Opening/creating refuses unsaved current edits.

| Tools | Purpose |
| --- | --- |
| `editor_status`, `scene_list`, `object_get` | Inspect native records, IDs, transforms and XML fields |
| `level_create`, `level_open`, `level_save` | Create/open/save real Run3 maps with overlay resolution |
| `asset_list` | Search mesh filenames, texture filenames and material names |
| `object_create` | Place existing meshes, dimensioned boxes, point/spot/directional lights |
| `record_create`, `object_fields` | Add or edit native gameplay records such as triggers/NPCs using their authored XML schema |
| `object_transform`, `object_delete` | Move, rotate, scale or remove records/subtrees |
| `material_create`, `object_material` | Write colored/textured materials and assign them to objects |
| `environment_set` | Set ambient lighting and player spawn |
| `history` | Undo/redo scene edits as editor operations |
| `camera_set`, `viewport_capture` | Aim the camera and inspect the resulting PNG image |
| `scene_validate`, `render_restore` | Check scene structure and retry recoverable renderer failures |

Coordinates use Run3 world units, **Y up**. Local transforms are relative to the
native parent; camera coordinates are world-space. Rotation is `[pitch,yaw,roll]`
in degrees, applied `Y*X*Z`. A box is a unit cube centered on its origin, so scale
`[1000,40,1000]` creates a floor 1000 units wide/deep and 40 units thick. Set
`collision:true` to emit the runtime's static collision entity; `false` emits
`nocollide`. Mesh records are wrapped in transform nodes, matching Run3's format.
Use `scene_list` to find the `nodes` section (default placement parent) and
`adents` for sequence/gameplay records. IDs are session-local; refresh after
reopening or undo/redo that recreates records.

To texture an object, use `asset_list(kind="texture")`, pass a returned filename
to `material_create`, then pass the returned material name to `object_material`.
New materials and the shared unit-box `.mesh` are **real map-local asset files**,
written immediately without overwriting existing files. Generated meshes use the
Ogre 1.8 format for runtime compatibility. Scene saves preserve native material
references, with the serializer's backups and external-change checks.

For advanced entities, inspect an authored `object_get.xml` and its exact
`fields` first. `object_fields` values are strings. Existing paths such as
`colourDiffuse[0]/@r` are returned by `object_get`; new attributes can use `@name`.
Use `record_create` for new child records, not property paths. Lua is never run
inside the editor. Runtime-specific entity schemas remain the game's schemas.

## Visual evaluation and corrections

Keep Ogitor visible, unminimized, on its Render tab. Use `camera_set` with
`position` and `look_at`, then `viewport_capture`. The MCP result contains an
**image**, not just a filename. A vision-capable Codex client can inspect it,
move/resize/retexture objects, change lights, and capture again. Inspect multiple
angles and close-ups before saving. Capture defaults to a maximum dimension of
1280 pixels; `max_size` accepts 128–2048.

`scene_validate` reports missing mesh files, duplicate names and player spawn
problems. It cannot judge aesthetics, path traversability or gameplay. The
preview does not simulate scripts, physics, NPC AI, audio or game compositors.
Verify the saved map in Run3 with the base and overlay roots before shipping it.

Undo/redo covers scene edits. It does **not** erase new level directories or
material/mesh asset files. A disconnected/timed-out request may already have
executed; inspect the scene before retrying a mutation. Tool calls are synchronous
and serialized; active modal dialogs return a busy error. There is no arbitrary
Python/Lua/shell execution tool.

## Local connection and shutdown

The editor listens only on IPv4 loopback while **Connect** is active. It creates
a random per-session authentication token in
`%LOCALAPPDATA%/Run3Ogitor/mcp-9877.json` on Windows or
`$XDG_CONFIG_HOME/Run3Ogitor/mcp-9877.json` (`~/.config` by default) elsewhere.
The Python server discovers this automatically. Don't commit this file or its
token. The port is remembered; connection is off on normal startup. **Disconnect**
closes client sockets, removes discovery data and immediately stops accepting
commands. Each editor instance needs a different port. `--connection-file` can
override discovery; `--timeout` controls the editor response deadline.

For bounded automated editor tests, `RUN3_MCP_AUTOCONNECT=<port>` opts in at
startup; it is not persisted as a preference. Unset it for normal use.

## Verification

```powershell
python -m unittest discover -s tools/run3ogitor_mcp -p test_server.py -v
python tools/run3ogitor_mcp/smoke_editor.py `
  --editor C:/Run3-Game-Engine/build/run3ogitor-windows-msvc142/RunPath/bin/qtOgitor.exe `
  --content-root "C:/Run3-Game-Engine/Games/The Long Way/TheLongWay" `
  --output-root C:/Run3-Game-Engine/Run3Ogitor/artifacts/mcp-test
```

The integration test starts only its own editor/MCP processes, creates a uniquely
named map in the supplied overlay, tests authentication, geometry, lighting,
materials, undo/redo, capture, save and reopen. It preserves the resulting map
and `overview.png` for review. No base game files are modified. A failed test may
leave its new map for diagnosis. Do not point `output_root` at a source asset tree.
