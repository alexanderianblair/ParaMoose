# This file is only ever included via paraview_plugin_build() in the
# top-level CMakeLists.txt, after paraview_plugin_scan() has discovered it
# through ../paraview.plugin. Do not find_package(ParaView) again here.

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
    HitParser.cxx
    HitParser.h
    ${dock_interface_sources}
)

target_link_libraries(ParaMoose
  PRIVATE
    ParaView::pqApplicationComponents
    ParaView::pqCore
)
