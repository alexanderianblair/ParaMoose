# MOOSE HIT Editor - ParaView Plugin (example)

A minimal ParaView dock-widget plugin that lets you open, edit, and save a
MOOSE `.i` (hit format) input file from inside ParaView, and optionally run
the associated MOOSE app and load the resulting Exodus result straight into
the active pipeline.

This is a **teaching example**, not a production-ready tool. It's meant to
show the *shape* of a real plugin (tree/table UI backed by a parsed HIT
tree, wired into ParaView's dock-window and pipeline APIs) so you have a
concrete skeleton to extend.

## Files

```
ParaMoose/                    <- top level project
  CMakeLists.txt              <- find_package(ParaView) + plugin scan/build
  ParaMoose/                  <- the plugin itself (name matches its own dir)
    paraview.plugin           <- plugin descriptor, discovered by paraview_plugin_scan()
    CMakeLists.txt             <- the plugin's own build file (paraview_add_plugin)
    HitParser.h / HitParser.cxx
    MooseSchema.h / MooseSchema.cxx
    MooseHitEditorPanel.h / MooseHitEditorPanel.cxx
```

ParaView 5.7+ (including 6.0) builds plugins through a **scan/build**
pattern rather than a single flat `CMakeLists.txt`: each plugin lives in its
own subdirectory containing a `paraview.plugin` descriptor and its own
`CMakeLists.txt`, and the top-level project calls `paraview_plugin_scan()`
to discover it and `paraview_plugin_build()` to build it. Calling
`paraview_add_plugin()` directly from a flat top-level `CMakeLists.txt`
(an earlier version of this example did exactly that) produces a
`"<...>'s CMakeLists.txt may not add the <name> plugin"` configure error.

- `HitParser.h` / `HitParser.cxx` - a small, self-contained parser and
  serializer for the hit format (nested `[Block] ... []`, `key = value`,
  quoted values, `#` comments).
- `MooseSchema.h` / `MooseSchema.cxx` - loads MOOSE's `--json` syntax dump
  and exposes valid `type = ` values per top-level block plus
  description/options for individual parameters. See "Schema awareness"
  below.
- `MooseHitEditorPanel.h` / `MooseHitEditorPanel.cxx` - the `QDockWidget`
  subclass: toolbar (Open/Save/Save As/Run/Load Schema), a tree of blocks
  on the left, a parameter table on the right.
- `ParaMoose/paraview.plugin` - the plugin descriptor (name/description)
  that `paraview_plugin_scan()` reads.
- `ParaMoose/CMakeLists.txt` - the actual `paraview_add_plugin()` /
  `paraview_plugin_add_dock_window()` calls, included only via the
  top-level scan/build.
- `CMakeLists.txt` (top level) - `find_package(ParaView)`, `BUILD_SHARED_LIBS`,
  and scan/build.

## Schema awareness

Click **Load App Schema...** (after setting **App executable**) to run
`<exe> --json` and parse the result. Once loaded:

- Any parameter named `type` gets a dropdown of valid values scoped to the
  top-level block it lives under (e.g. selecting a block under `[Kernels]`
  and editing its `type` offers `Diffusion`, `TimeDerivative`, etc., pulled
  from that app's actual registered kernels).
- Any parameter the schema describes as a fixed set of options
  (MooseEnum-style) also gets a dropdown instead of a free-text cell.
- Parameter names get a tooltip with their description and C++ type, where
  the schema has one.
- All dropdowns stay editable, so you can still type a value the schema
  doesn't know about.

This is a best-effort reader, not a strict validator - see the caveat in
`MooseSchema.h` about how it maps the JSON tree to block names. It doesn't
stop you from typing an invalid block/parameter name; it only helps you
pick a valid one when it can.

## Known limitations (read before relying on this)

1. **The HIT parser is simplified.** It does not implement the full hit
   grammar - no `!include`, no multi-line strings, limited escaping, and it
   doesn't preserve comments or original formatting on save. MOOSE itself
   ships a real hit parsing library at `framework/contrib/hit` in the MOOSE
   repository (with Python bindings too). For anything beyond a demo,
   link against that instead of `HitParser.cxx` so anything MOOSE can parse,
   you can parse identically.
2. **Schema awareness is best-effort, not validation.** The `--json`-backed
   dropdowns/tooltips (see "Schema awareness" above) help you pick valid
   values but don't stop you from typing something the schema doesn't know
   about, and parameter docs/options are looked up by name globally rather
   than strictly scoped to the exact block path - see the note at the top
   of `MooseSchema.h`.
3. **`onRunSolve()` runs synchronously** (`waitForFinished(-1)`), which
   blocks the ParaView UI thread for the duration of the solve. Fine for a
   quick demo; for real use, run the `QProcess` asynchronously and stream
   output into a log widget (ParaView has `pqOutputWidget` for exactly
   this) instead.
4. **CMake macro names shift between ParaView versions.** This example
   uses `paraview_plugin_add_dock_window()` / `paraview_add_plugin()`,
   matching the `Examples/Plugins/DockWidget` example shipped with recent
   (5.9+) ParaView releases. If your ParaView is older, check that example
   in your own ParaView source tree for the exact macro signatures - older
   releases used `ADD_PARAVIEW_DOCK_WINDOW()` / `ADD_PARAVIEW_PLUGIN()`
   instead.

## Building

You need ParaView development headers - either build ParaView from source,
or use a binary release that includes the plugin SDK.

```bash
mkdir build && cd build
cmake -DParaView_DIR=/path/to/paraview_build/lib/cmake/paraview-6.0 ..
cmake --build . --config Release
```

(Point `ParaView_DIR` at wherever your build/install placed
`ParaViewConfig.cmake` - `lib/cmake/paraview-<version>` under your build or
install prefix.)

This produces `ParaMoose.so` (Linux/macOS) or
`ParaMoose.dll` (Windows), typically under
`build/ParaMoose/` or `build/bin/` depending on platform.

## Loading it in ParaView

1. Launch ParaView.
2. **Tools > Manage Plugins > Load New...**
3. Select the built `ParaMoose.{so,dll,dylib}`.
4. Check "Auto Load" if you want it loaded automatically next time.
5. A new dock panel titled **MOOSE Input Editor** appears (default docked
   left). If it's hidden, it'll show up under **View > MOOSE Input
   Editor**.

## Using it

1. Click **Open...**, pick a `.i` file, or click **New** to start from a
   blank input (the panel actually starts with a blank one already, so you
   can begin adding blocks immediately without opening or saving anything
   first).
2. Right-click empty space in the tree for **Add Top-Level Block** — this
   always adds a new `[BlockName]` at the top level, regardless of what's
   currently selected. Right-click an existing block for **Add Child
   Block** (adds under that block) or **Remove Block**.
3. Click a block in the tree to see/edit its parameters in the table on the
   right.
3. Use **Add Parameter** / **Remove Selected** under the table to edit
   parameters for the selected block.
4. **Save** / **Save As...** writes the tree back out in hit format.
5. Optionally set **App executable** to your compiled MOOSE app, then
   **Run + Load Result** — this shells out to `<exe> -i <file>`, and on
   success loads `<stem>_out.e` into the active ParaView pipeline using
   ParaView's own Exodus reader, so you immediately see the mesh/fields in
   the render view.
6. Optionally click **Load App Schema...** (same executable) to enable the
   type/enum dropdowns and tooltips described in "Schema awareness" above.
7. Click **View Mesh** to load the mesh referenced by the `[Mesh]` block
   directly, without running a solve. See "Viewing the input mesh" below.

## Viewing the input mesh

**View Mesh** searches the `[Mesh]` block (and any sub-blocks, e.g. nested
MeshGenerators) for a `file = ...` parameter, resolves it relative to the
saved input file's location, and loads it with ParaView's own Exodus
reader. This covers both:

```
[Mesh]
  type = FileMesh
  file = my_mesh.e
[]
```

and the newer MeshGenerator-nested style:

```
[Mesh]
  [fmg]
    type = FileMeshGenerator
    file = my_mesh.e
  []
[]
```

Notes/limitations:

- The input file must be saved first (paths are resolved relative to it).
- If `[Mesh]` has no `file` parameter at all - e.g. a generated mesh like
  `type = GeneratedMesh` - there's nothing to find, since MOOSE builds that
  mesh at runtime rather than reading it from disk. Use **Run + Load
  Result** instead to see the mesh MOOSE actually produces.
- If multiple `file` parameters exist (e.g. a chain of mesh generators
  that each reference a file), only the first one found is used.
- Only Exodus (`.e`/`.exo`) is wired up, via ParaView's own Exodus reader.
  Other formats MOOSE can read (Gmsh `.msh`, Abaqus `.inp`, etc.) would
  need the reader proxy picked based on file extension - not done here to
  keep the example focused.
