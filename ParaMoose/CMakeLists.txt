# This file is only ever included via paraview_plugin_build() in the
# top-level CMakeLists.txt, after paraview_plugin_scan() has discovered it
# through ../paraview.plugin. Do not find_package(ParaView) again here.

# ---------------------------------------------------------------------------
# MOOSE's real hit input-file parser (framework/contrib/hit), replacing the
# hand-rolled parser this plugin started with. hit depends on WASP, a
# separate compiled library that MOOSE builds alongside it -- see
# MOOSE_DIR/framework/contrib/hit/Makefile in your own checkout for the
# same dependency, expressed as a plain Makefile instead of CMake.
# ---------------------------------------------------------------------------
if(NOT MOOSE_DIR)
  message(FATAL_ERROR
    "Set -DMOOSE_DIR=/path/to/your/moose (the checkout that also built "
    "your MOOSE application executable) so ParaMoose can link against "
    "MOOSE's real hit input-file parser.")
endif()

set(HIT_DIR "${MOOSE_DIR}/framework/contrib/hit" CACHE PATH
  "Path to MOOSE's hit parser sources")
set(WASP_DIR "${MOOSE_DIR}/framework/contrib/wasp/install" CACHE PATH
  "Path to WASP install (hit's dependency); matches hit's own Makefile default")

if(NOT EXISTS "${HIT_DIR}/include/hit/hit.h")
  message(FATAL_ERROR
    "Could not find ${HIT_DIR}/include/hit/hit.h -- check MOOSE_DIR "
    "(expected framework/contrib/hit underneath it).")
endif()
if(NOT EXISTS "${WASP_DIR}/include")
  message(FATAL_ERROR
    "Could not find ${WASP_DIR}/include -- check WASP_DIR, or run "
    "scripts/update_and_rebuild_wasp.sh in your MOOSE checkout if you "
    "haven't built WASP yet (hit needs it).")
endif()

# WASP ships several libwasp*.{so,dylib} libraries; glob for them the same
# way framework/contrib/hit/Makefile itself does, rather than hardcoding
# names that may change between WASP versions.
file(GLOB WASP_LIBRARIES "${WASP_DIR}/lib/libwasp*.so" "${WASP_DIR}/lib/libwasp*.dylib")
if(NOT WASP_LIBRARIES)
  message(FATAL_ERROR "No libwasp*.{so,dylib} found under ${WASP_DIR}/lib -- check WASP_DIR.")
endif()

# hit itself is just 3 translation units; compile them directly rather than
# depending on however MOOSE's own Makefile-based build happens to have
# built (or not built) a standalone hit library artifact.
add_library(hit STATIC
  "${HIT_DIR}/src/hit/parse.cc"
  "${HIT_DIR}/src/hit/lex.cc"
  "${HIT_DIR}/src/hit/braceexpr.cc"
)
target_include_directories(hit PUBLIC "${HIT_DIR}/include" "${WASP_DIR}/include")
target_link_libraries(hit PUBLIC ${WASP_LIBRARIES})
target_compile_features(hit PUBLIC cxx_std_17)
set_target_properties(hit PROPERTIES POSITION_INDEPENDENT_CODE ON)

paraview_plugin_add_dock_window(
  CLASS_NAME MooseHitEditorPanel
  DOCK_AREA Left
  INTERFACES dock_interfaces
  SOURCES dock_interface_sources
)

paraview_add_plugin(ParaMoose
  VERSION "1.0"
  UI_INTERFACES ${dock_interfaces}
  SOURCES
    MooseHitEditorPanel.cxx
    MooseHitEditorPanel.h
    MooseSchema.cxx
    MooseSchema.h
    ${dock_interface_sources}
)

target_link_libraries(ParaMoose
  PRIVATE
    ParaView::pqApplicationComponents
    ParaView::pqCore
    hit
)

# Pre-fill the "App executable" field with MOOSE's own test app, built from
# the same MOOSE_DIR the person already had to point us at for hit/WASP.
# Still just a starting point in an editable field -- real apps live
# elsewhere, this just saves retyping the obvious default while testing.
target_compile_definitions(ParaMoose PRIVATE
  "MOOSE_DEFAULT_APP_EXECUTABLE=\"${MOOSE_DIR}/test/moose_test-opt\""
)

# WASP's shared libraries live under WASP_DIR, which usually isn't on the
# system's normal linker search path -- bake in an rpath so ParaView can
# actually find them at runtime when it loads ParaMoose.so.
set_target_properties(ParaMoose PROPERTIES
  BUILD_RPATH "${WASP_DIR}/lib"
  INSTALL_RPATH "${WASP_DIR}/lib"
)
