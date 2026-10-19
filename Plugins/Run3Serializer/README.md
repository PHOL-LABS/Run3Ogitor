# Native Run3 map editing

Build with `Run3Ogitor/build.bat`, then start `Run3Ogitor/run.bat`.
Use **File > Open** and select a map's `scene.cfg`, or use the
**Run3 native map (scene.cfg)** importer. A `scene.cfg` path can also be passed
to `run.bat` (quote paths containing spaces).

Select the base game directory (the directory containing `run3`) and, optionally,
derived-content overlay directories. Later overlays have higher priority.
Resources and each Scene/Scene2/Sequence document resolve through the same roots.
Paths are remembered per map. Only open trusted local game assets: the original
game uses legacy binary mesh readers, which Ogre explicitly does not consider
safe for untrusted files.

## Editing and saving

- Scene and sequence XML records appear in the scene tree, including unknown
  records. Their original attributes and parameter children are editable in
  **Custom Properties**. `@name` is the game identifier; the tree label is an
  independent, unique editor identifier.
- Models, NPCs, lights and particles have previews. Non-model records have
  colored helpers; triggers display their volumes. Cutscene frames and train
  keypoints use their native world-space positions. Logical records remain
  accessible through the scene tree. Lua/gameplay scripts are never executed.
- Use position/rotation/scale gizmos for spatial records. Do not move XML
  containers such as `scene`, `nodes`, `sequence` or `adents`: they are document
  structure, not game objects.
- Right-click a native record/section and choose **Add Run3 XML child...** to add
  a new record or subtree using native XML. Duplicate/delete and undo use the
  editor's normal commands. When duplicating named game objects, change their
  `@name` and any references intentionally; references are not automatically
  renamed. Use native records rather than ordinary Ogitor object factories.
- **Save** writes changed documents into the last overlay, or into the base
  game if no overlay was specified. No `.ogscene` conversion is performed.
  Saving without changes does not rewrite XML. Changed documents retain unknown
  fields, comments and record order, but XML whitespace/quoting is normalized.
- Overwritten files get `.ogitor.bak` recovery copies. External source/target
  changes are detected before overwriting. Reopen if another tool changed them.
  Save As is intentionally not an `.ogscene` export: reopen the map with a
  different writable overlay to create a variant.

## Preview limitations

The editor is not the Run3 runtime renderer. Unsupported authored game shaders
use an **editor-only textured preview**; original material names/files are not
changed by saving. Game compositor effects, physics, NPC AI, sequence playback
and audio playback do not run in the editor. Validate final lighting and gameplay
in `run3_shell`.

Meshes rejected by the SDK's stricter legacy reader can be canonicalized by the
official Ogre 14.5 `OgreMeshUpgrader` already used by the port. CMake discovers
the tool in the port's Windows vcpkg build, or it can be provided through
`RUN3_LEGACY_MESH_UPGRADER` at configure time / `RUN3_EDITOR_MESH_UPGRADER` at
runtime. Converted files go to the Qt user cache, keyed by source bytes and tool
identity. Original meshes are never overwritten. Missing/invalid resources are
logged and use markers instead of opening a modal dialog for each mesh.

The resource browser lists game mesh filenames without eagerly rendering
thousands of thumbnails. Set the appropriate `@meshFile` property to assign one.
The editor uses a temporary OFS workspace only for its ancillary UI, never as
the native map's storage format.

SemiInfiniteWorlds (`WorldSeed`/`BlockManifest`) are explicitly rejected before
constructing the scene. The document/resource resolver and serializer plugin
are separate from preview entities so a future world editor can reuse them.

## Verification

`run3_document_tests.exe` is built alongside the plugin. It tests overlay
precedence, preservation, backups, external-edit protection and native reopening.
An optional game-root argument validates all high-detail map documents read-only.

`build-support/smoke-native.ps1 -Map <scene.cfg> -ContentRoot <game> -Overlay <overlay>`
runs a bounded graphical test, exercises a gizmo move and property edit, restores
them, checks an unchanged editor/XML round trip and the no-op Save path, renders
a frame, tests actual object selection, move/rotate/scale gizmos and highlighted
handles, all three hover/selection box materials and a full camera rotation,
captures it under the engine's `build` directory and exits. It does not
write map files. The script terminates only its own process on timeout.

Unattended launchers can supply `RUN3_EDITOR_CONTENT_ROOT` and
`RUN3_EDITOR_OVERLAYS` (semicolon-separated), bypassing the content-path dialog.
