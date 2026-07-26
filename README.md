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
moose-hit-editor-plugin/
  CMakeLists.txt              <- top level: find_package(ParaView) + plugin scan/build
  ParaMoose/
    paraview.plugin           <- plugin descriptor, discovered by paraview_plugin_scan()
    CMakeLists.txt             <- the plugin's own build file (paraview_add_plugin)
    HitParser.h / HitParser.cxx
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
- `MooseHitEditorPanel.h` / `MooseHitEditorPanel.cxx` - the `QDockWidget`
  subclass: toolbar (Open/Save/Save As/Run), a tree of blocks on the left,
  a parameter table on the right.
- `ParaMoose/paraview.plugin` - the plugin descriptor
  (name/description) that `paraview_plugin_scan()` reads.
- `ParaMoose/CMakeLists.txt` - the actual `paraview_add_plugin()`
  / `paraview_plugin_add_dock_window()` calls, included only via the
  top-level scan/build.
- `CMakeLists.txt` (top level) - `find_package(ParaView)` + scan/build.

## Known limitations (read before relying on this)

1. **The HIT parser is simplified.** It does not implement the full hit
   grammar - no `!include`, no multi-line strings, limited escaping, and it
   doesn't preserve comments or original formatting on save. MOOSE itself
   ships a real hit parsing library at `framework/contrib/hit` in the MOOSE
   repository (with Python bindings too). For anything beyond a demo,
   link against that instead of `HitParser.cxx` so anything MOOSE can parse,
   you can parse identically.
2. **No schema validation.** This editor lets you type any block/parameter
   name - it has no idea what parameters `[Kernels]` or a `Diffusion`
   kernel actually accept. The natural next step (mentioned in-chat
   earlier) is to also parse the `--json` syntax dump from your compiled
   MOOSE app and use it to validate block/parameter names, populate
   dropdowns for `MooseEnum` parameters, and show parameter docstrings as
   tooltips. That's a separate chunk of work (JSON parsing + a schema
   lookup keyed by block path) layered on top of this tree/table UI.
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

1. Click **Open...**, pick a `.i` file. The block tree populates on the
   left.
2. Click a block in the tree to see/edit its parameters in the table on the
   right. Right-click the tree for **Add Child Block** / **Remove Block**.
3. Use **Add Parameter** / **Remove Selected** under the table to edit
   parameters for the selected block.
4. **Save** / **Save As...** writes the tree back out in hit format.
5. Optionally set **App executable** to your compiled MOOSE app, then
   **Run + Load Result** — this shells out to `<exe> -i <file>`, and on
   success loads `<stem>_out.e` into the active ParaView pipeline using
   ParaView's own Exodus reader, so you immediately see the mesh/fields in
   the render view.

## Extending toward the "schema-aware" version

The natural next iteration, per the architecture discussed earlier:

- At startup (or on demand), run `your-app-opt --json` and parse the
  resulting syntax tree.
- Key that schema by block path (e.g. `Kernels/*/type` -> valid kernel
  types) and use it to:
  - populate a combo box instead of a free-text field for `type =`,
  - show/hide parameters based on the selected `type`'s declared
    parameters,
  - render `MooseEnum` parameters as dropdowns and vector parameters as a
    small repeated-field editor instead of a single text cell,
  - show each parameter's docstring as a tooltip on hover.
- This turns the generic tree/table editor here into something much closer
  to Peacock's input-file tab, while still living inside ParaView and
  reusing ParaView's own Exodus reading/rendering for the results side.
