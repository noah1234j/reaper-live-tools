cmake_minimum_required(VERSION 3.15)
project(reaper_transitions LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# Install to the user's REAPER UserPlugins folder
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  if(WIN32)
    set(USER_CONFIG_DIR "$ENV{APPDATA}")
  elseif(APPLE)
    set(USER_CONFIG_DIR "~/Library/Application Support")
  else()
    set(USER_CONFIG_DIR "~/.config")
  endif()
  set(CMAKE_INSTALL_PREFIX "${USER_CONFIG_DIR}/REAPER" CACHE PATH
    "REAPER resource path" FORCE)
endif()

# ---------------------------------------------------------------------------
# Architecture name (used in output filename)
# ---------------------------------------------------------------------------
if(CMAKE_OSX_ARCHITECTURES)
  list(JOIN CMAKE_OSX_ARCHITECTURES "-" ARCH_NAME)
elseif(MSVC_CXX_ARCHITECTURE_ID)
  set(ARCH_NAME ${MSVC_CXX_ARCHITECTURE_ID})
else()
  set(ARCH_NAME ${CMAKE_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${ARCH_NAME}" ARCH_NAME)

# ---------------------------------------------------------------------------
# Global compile settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_VISIBILITY_PRESET "hidden")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

  # Static runtime + embedded debug info
  foreach(flag
    CMAKE_C_FLAGS_DEBUG   CMAKE_CXX_FLAGS_DEBUG
    CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE
    CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO
    CMAKE_C_FLAGS_MINSIZEREL     CMAKE_CXX_FLAGS_MINSIZEREL
  )
    string(REPLACE "/MD" "/MT" ${flag} "${${flag}}")
    string(REGEX REPLACE "/Z[iI]" "/Z7" ${flag} "${${flag}}")
  endforeach()

  add_compile_options(/utf-8 /GS)
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/Zo>)
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/GF>)
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/Gy>)
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/Zc:inline>)
  # Disable thread-safe statics (XP compat, same as SWS)
  add_compile_options(/Zc:threadSafeInit-)
else()
  add_compile_options(-fsigned-char -fstack-protector-strong -fdiagnostics-color)
endif()

# ---------------------------------------------------------------------------
# Main target
# ---------------------------------------------------------------------------
add_library(reaper_transitions SHARED
  # Entry point
  reaper_transitions.cpp
  # Core snapshot engine
  core/TransitionSnapshot.cpp
  core/TransitionEngine.cpp
  core/PluginSafetyEngine.cpp
  # Scenes
  scenes/TransitionWnd.cpp
  scenes/SafesWnd.cpp
  # Layouts
  layouts/LayoutSnapshot.cpp
  layouts/LayoutsWnd.cpp
  # Layers
  layers/LayersEngine.cpp
  layers/LayersWnd.cpp
  # DCA groups
  dca/DcaGroup.cpp
  dca/DcaEngine.cpp
  dca/DcaWnd.cpp
  # Mute groups
  mute/MuteGroup.cpp
  mute/MuteGroupsWnd.cpp
  # Monitors
  monitors/PaflWnd.cpp
  monitors/MonitorWnd.cpp
  monitors/MeterBridgeWnd.cpp
  monitors/LiveOptimizeWnd.cpp
  # Livelock
  livelock/LiveLockEngine.cpp
  livelock/LiveLockWnd.cpp
  # Control surface (CSI)
  csurf/control_surface_integrator.cpp
  csurf/control_surface_integrator_ui.cpp
  csurf/SurfaceMonitorWnd.cpp
  # WDL UTF8 shims (required by CSI)
  ${VENDOR_DIR}/WDL/WDL/win32_utf8.c
  # Talkback
  talkback/TalkbackWnd.cpp
  # Surface & Zone Editor
  surface_editor/SurfaceModel.cpp
  surface_editor/ZoneModel.cpp
  surface_editor/CanvasWnd.cpp
  surface_editor/ActionSearchDlg.cpp
  surface_editor/ZoneEditorWnd.cpp
  surface_editor/SurfaceEditorWnd.cpp
  # Resources
  reaper_transitions.rc
)

target_compile_features(reaper_transitions PRIVATE cxx_std_17)

target_include_directories(reaper_transitions PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/core
  ${CMAKE_CURRENT_SOURCE_DIR}/scenes
  ${CMAKE_CURRENT_SOURCE_DIR}/layouts
  ${CMAKE_CURRENT_SOURCE_DIR}/layers
  ${CMAKE_CURRENT_SOURCE_DIR}/dca
  ${CMAKE_CURRENT_SOURCE_DIR}/mute
  ${CMAKE_CURRENT_SOURCE_DIR}/monitors
  ${CMAKE_CURRENT_SOURCE_DIR}/livelock
  ${CMAKE_CURRENT_SOURCE_DIR}/csurf
  ${CMAKE_CURRENT_SOURCE_DIR}/talkback
  ${CMAKE_CURRENT_SOURCE_DIR}/surface_editor
  "${VENDOR_DIR}/reaper-sdk/sdk"
  "${VENDOR_DIR}/WDL"          # only needed for swell.h on non-Windows
)

if(MSVC)
  target_compile_options(reaper_transitions PRIVATE /EHsc /W3)
  target_link_libraries(reaper_transitions PRIVATE Shlwapi comctl32 PowrProf Psapi Advapi32)
  target_link_options(reaper_transitions PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
    $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
    $<$<NOT:$<CONFIG:Debug>>:/PDBALTPATH:%_PDB%>
  )
else()
  target_compile_options(reaper_transitions PRIVATE
    -Wall -Wno-unused-function -Wno-unused-parameter
    -Wno-sign-compare -Wno-deprecated-declarations
  )
  # On macOS/Linux REAPER provides SWELL; link against it
  find_library(SWELL_LIB NAMES SWELL swell)
  if(SWELL_LIB)
    target_link_libraries(reaper_transitions PRIVATE ${SWELL_LIB})
  endif()
endif()

set_target_properties(reaper_transitions PROPERTIES
  PREFIX ""
  OUTPUT_NAME "reaper_transitions"
)

install(TARGETS reaper_transitions
  RUNTIME DESTINATION "UserPlugins"   # Windows .dll
  LIBRARY DESTINATION "UserPlugins"   # macOS .dylib / Linux .so
)
