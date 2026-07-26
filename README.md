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
    CMakeLists.txt             <- the plugin's own build file (paraview_add_plugin,
                                  plus building/linking MOOSE's real hit parser)
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

- `MooseSchema.h` / `MooseSchema.cxx` - loads MOOSE's `--json` syntax dump
  and exposes valid `type = ` values per top-level block plus
  description/options for individual parameters. See "Schema awareness"
  below.
- `MooseHitEditorPanel.h` / `MooseHitEditorPanel.cxx` - the `QDockWidget`
  subclass: toolbar (New/Open/Save/Save As/Run/View Mesh/Load Schema), a
  tree of blocks on the left, a parameter table on the right. Operates
  directly on `hit::Node` trees from MOOSE's own parser (see "The hit
  parser" below) rather than a custom data model.
- `ParaMoose/paraview.plugin` - the plugin descriptor (name/description)
  that `paraview_plugin_scan()` reads.
- `ParaMoose/CMakeLists.txt` - `paraview_add_plugin()` /
  `paraview_plugin_add_dock_window()`, plus compiling MOOSE's `hit` parser
  sources and linking against WASP (see "The hit parser" below). Included
  only via the top-level scan/build.
- `CMakeLists.txt` (top level) - `find_package(ParaView)`, `BUILD_SHARED_LIBS`,
  and scan/build.

## The hit parser

This plugin links directly against MOOSE's own hit parser
(`framework/contrib/hit` in your MOOSE checkout) rather than a hand-rolled
one. Concretely, the panel now edits a real `hit::Node` tree (`hit::parse()`
/ `hit::Section` / `hit::Field` / `node->render()`) instead of a custom
struct, which gets you:

- The actual MOOSE grammar - `!include`, multi-line/escaped strings, and
  all the quoting rules, not an approximation of them.
- `render()` is MOOSE's own serializer, so it applies MOOSE's real
  quoting/escaping and preserves comments and blank lines that were
  present in the parsed input and untouched by this editor - the older
  hand-rolled serializer discarded all of that on every save.

One caveat worth knowing: this editor's UI only ever surfaces `Section` and
`Field` nodes (the tree shows sections, the table shows fields) - it has no
way to view or add `Comment`/`Blank` nodes itself. So comments *survive* a
load-edit-save round trip wherever you don't touch that part of the file,
but you still can't see or author them through this UI.

hit itself depends on **WASP**, a separate compiled library MOOSE builds
alongside it - see "Building" below for what that means for the CMake
invocation. This is a real dependency shift from the plugin's earlier
versions, which had zero external dependencies beyond Qt/ParaView.

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

## Highlighting boundaries/blocks

Click a `boundary` or `block` parameter row in the table (just select it,
no need to edit it) and the mesh region(s) it names highlight in solid red
in the active render view, overlaid on whatever you're currently looking
at. Selecting any other row, or nothing, clears the highlight.

This works by spinning up a second, independent Exodus reader pointed at
whichever file you last loaded via **View Mesh** or **Run + Load Result**
(tracked separately from your main view so highlighting never disturbs its
array selection or coloring), and setting that reader's own
`SideSetArrayStatus`/`NodeSetArrayStatus`/`ElementBlocks` properties to
show only the name(s) in the selected parameter's value - the same
properties you'd toggle by hand in the reader's Properties panel to show
just one sideset. `boundary` tries both sideset and nodeset names (MOOSE
uses the same parameter name for both surface and nodal BCs); `block`
always means element blocks.

One implementation note worth knowing if you're reading the code: these
properties are NOT a flat list of "enabled" names at the raw
`vtkSMStringVectorProperty` level, even though `paraview.simple` presents
them to Python that way (confirmed against ParaView's own property XML --
`number_of_elements_per_command="2" repeat_command="1"` means the real
element list is interleaved `(name, "0"/"1")` pairs, covering *every*
array the domain knows about, not just the ones being turned on). An
earlier version of this feature wrote a flat "enabled names only" list,
which the domain silently fell back from to its default (everything
enabled) -- the visible symptom was the entire mesh highlighting instead
of just the named region. `setArraySelectionProperty()` in
`MooseHitEditorPanel.cxx` builds the full pair list correctly by
enumerating the property's domain first.

Notes/limitations:

- Nothing to highlight against until you've loaded a mesh or result at
  least once via **View Mesh** / **Run + Load Result** in the current
  session.
- If a name in the parameter's value doesn't match anything in the
  loaded mesh (typo, or the mesh doesn't actually match this input file),
  nothing shows for that name - no error, it just silently matches
  nothing, same as manually typing a bad name into the reader's Properties
  panel would.
- The highlight reader is recreated (not reused) whenever the file it
  should point at changes; it isn't torn down when the panel closes, left
  for ParaView's own session cleanup, consistent with how the main
  mesh/result sources are already handled elsewhere in this plugin.

## Known limitations (read before relying on this)

1. **`resetToNewRoot()`/"New" assumes `hit::parse(fname, "")` succeeds** and
   returns a usable empty root. That's a reasonable assumption (empty input
   is valid hit) but hasn't been verified against a live build in this
   environment (no ParaView/MOOSE SDK available here) - if "New" misbehaves
   in your build, open a minimal existing `.i` file as a workaround while
   you look into it.
2. **Schema awareness is best-effort, not validation.** The `--json`-backed
   dropdowns/tooltips (see "Schema awareness" above) help you pick valid
   values but don't stop you from typing something the schema doesn't know
   about, and parameter docs/options are looked up by name globally rather
   than strictly scoped to the exact block path - see the note at the top
   of `MooseSchema.h`.
3. **CMake macro names shift between ParaView versions.** This example
   uses `paraview_plugin_add_dock_window()` / `paraview_add_plugin()`,
   matching the `Examples/Plugins/DockWidget` example shipped with recent
   (5.9+) ParaView releases. If your ParaView is older, check that example
   in your own ParaView source tree for the exact macro signatures - older
   releases used `ADD_PARAVIEW_DOCK_WINDOW()` / `ADD_PARAVIEW_PLUGIN()`
   instead.
4. **No comment/blank-line authoring in the UI**, even though they now
   survive round-trips elsewhere in the file - see the caveat at the end of
   "The hit parser" above.
5. **The run's `finished`/`errorOccurred` handlers aren't hardened against
   every edge case** - e.g. if the panel is closed mid-solve, the
   destructor kills the process but the in-flight signal handlers still
   reference panel members briefly during teardown. Fine for normal usage
   (they're no-ops once the process is gone); a production version would
   want more defensive lifetime handling here.

## Building

You need ParaView development headers - either build ParaView from source,
or use a binary release that includes the plugin SDK.

You also now need a MOOSE checkout with **WASP built** (WASP is hit's own
dependency - the same one your MOOSE build already needed to compile `hit`
for itself). If `<moose>/framework/contrib/wasp/install` doesn't exist yet,
run `scripts/update_and_rebuild_wasp.sh` from your MOOSE checkout first.

```bash
mkdir build && cd build
cmake .. \
  -DParaView_DIR=/path/to/paraview_build/lib/cmake/paraview-6.0 \
  -DMOOSE_DIR=/path/to/your/moose
cmake --build . --config Release
```

(Point `ParaView_DIR` at wherever your build/install placed
`ParaViewConfig.cmake` - `lib/cmake/paraview-<version>` under your build or
install prefix. `MOOSE_DIR` should be the checkout root, i.e. the directory
containing `framework/`.)

If WASP lives somewhere other than
`<MOOSE_DIR>/framework/contrib/wasp/install` (some setups point it
elsewhere via the `WASP_DIR` environment variable when building MOOSE
itself), pass `-DWASP_DIR=/path/to/wasp/install` explicitly too.

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

The **App executable** field starts pre-filled with
`<MOOSE_DIR>/test/moose_test-opt` (MOOSE's own test application), derived
from the `MOOSE_DIR` you passed at configure time - it's just a starting
point and stays fully editable, so point it at your actual app instead if
that's what you're working with.

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
   **Run + Load Result**. This shells out to `<exe> -i <file>`
   asynchronously - the ParaView UI stays responsive, and solver output
   streams live into the **Solver Output** panel at the bottom (a
   `pqOutputWidget`, same widget ParaView uses for its own message log) as
   the run progresses. On success, `<stem>_out.e` loads automatically into
   the active ParaView pipeline via ParaView's own Exodus reader. **Run +
   Load Result** is disabled while a solve is in flight.
6. Optionally click **Load App Schema...** (same executable) to enable the
   type/enum dropdowns and tooltips described in "Schema awareness" above.
7. Click **View Mesh** to load the mesh referenced by the `[Mesh]` block
   directly, without running a solve. See "Viewing the input mesh" below.
8. Click any `boundary` or `block` parameter row to highlight the region
   it names in the render view. See "Highlighting boundaries/blocks"
   below.

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
