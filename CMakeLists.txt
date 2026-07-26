cmake_minimum_required(VERSION 3.16)
project(ParaMooseProject)
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build plugin as a shared library" FORCE)

# You must point CMake at a ParaView build/install that includes development
# headers (either a "ParaView SDK"/devel package, or a build tree you
# compiled yourself), e.g.:
#   cmake -DParaView_DIR=/path/to/paraview_build/lib/cmake/paraview-6.0 ..
find_package(ParaView REQUIRED)

# paraview_plugin_build() needs CMAKE_INSTALL_INCLUDEDIR/LIBDIR/BINDIR (etc.)
# to default destinations for build artifacts; GNUInstallDirs defines them.
include(GNUInstallDirs)

# ParaView 5.7+ (including 6.0) builds plugins via a scan/build pattern:
# each plugin lives in its own subdirectory containing a `paraview.plugin`
# descriptor file plus its own CMakeLists.txt. Calling paraview_add_plugin()
# directly from a top-level CMakeLists.txt (without going through
# paraview_plugin_scan/paraview_plugin_build) is what produces the
# "CMakeLists.txt may not add the <name> plugin" error.
paraview_plugin_scan(
  PLUGIN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/ParaMoose/paraview.plugin"
  PROVIDES_PLUGINS plugins
  ENABLE_BY_DEFAULT ON
)

paraview_plugin_build(
  PLUGINS ${plugins}
)
