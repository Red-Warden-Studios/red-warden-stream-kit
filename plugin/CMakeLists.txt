cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

# Red Warden Stream Kit: ONE OBS module (rws-stream-kit.dll) built from the three tool source trees
# (Multistream, Pre-Flight, In-Flight), which stay where they are and are compiled in by relative path.
# plugin/src/plugin-main.cpp holds the only OBS_DECLARE_MODULE; plugin/src/common/ holds the one copy of
# everything the tools used to duplicate. The tools create panels; src/kit/kit-dock.cpp is the ONE dock that hosts them.
set(RWS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The tool folders are siblings (red-warden-<tool>) in the development layout, or multistream/ preflight/
# inflight/ next to plugin/ in the published repository.
function(rws_tool_src var tool)
  if(EXISTS "${RWS_ROOT}/${tool}/src")
    set(${var} "${RWS_ROOT}/${tool}/src" PARENT_SCOPE)
  else()
    set(${var} "${RWS_ROOT}/../red-warden-${tool}/src" PARENT_SCOPE)
  endif()
endfunction()
rws_tool_src(MS_SRC multistream)
rws_tool_src(PF_SRC preflight)
rws_tool_src(IF_SRC inflight)
get_filename_component(MS_SRC "${MS_SRC}" ABSOLUTE)
get_filename_component(PF_SRC "${PF_SRC}" ABSOLUTE)
get_filename_component(IF_SRC "${IF_SRC}" ABSOLUTE)
set(COMMON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/common")

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

if(MSVC)
  # Embed only the PDB file name, not the build machine's full path (the release leak gate scans the DLL).
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${CMAKE_PROJECT_NAME}>")
endif()

# ---- sources -------------------------------------------------------------------------------
# Every tool's plugin-main.cpp is NOT compiled (its logic moved to <tool>-module.cpp); the shared files
# each tool used to carry (plugin-support, update-check, update-manifest, obs-websocket-api.h,
# run-generation.hpp) are deleted from the tool trees and compiled once from src/common/.
set(MS_FILES
    platforms.cpp platforms.hpp config.cpp config.hpp credentials.cpp credentials.hpp
    output-runner.cpp output-runner.hpp edit-dialog.cpp edit-dialog.hpp
    multistream-dock.cpp multistream-dock.hpp obs-audio.cpp obs-audio.hpp retry-governor.hpp
    resources.qrc dock-ui.cpp dock-ui.hpp upload-test.cpp upload-test.hpp
    importers.cpp importers.hpp import-dialog.cpp import-dialog.hpp
    session-report.cpp session-report.hpp report-dialog.cpp report-dialog.hpp
    platform-limits.cpp platform-limits.hpp bandwidth-test.cpp bandwidth-test.hpp
    bandwidth-dialog.cpp bandwidth-dialog.hpp connection-budget.hpp server-url.cpp server-url.hpp
    multistream-module.cpp multistream-module.hpp
)
set(PF_FILES
    disk-probe.cpp disk-probe.hpp mic-test.cpp mic-test.hpp multistream-probe.cpp multistream-probe.hpp
    obs-probe.cpp obs-probe.hpp preflight-controller.cpp preflight-controller.hpp
    preflight-settings.cpp preflight-settings.hpp preflight-dock.cpp preflight-dock.hpp
    preflight-eval.cpp preflight-eval.hpp settings-dialog.cpp settings-dialog.hpp
    preflight-module.cpp preflight-module.hpp
)
set(IF_FILES
    inflight-dock.cpp inflight-dock.hpp inflight-eval.cpp inflight-eval.hpp
    inflight-ui-logic.cpp inflight-ui-logic.hpp lights-panel.cpp lights-panel.hpp
    inflight-probe-logic.cpp inflight-probe-logic.hpp inflight-clock.hpp inflight-probe.cpp inflight-probe.hpp
    stream-signals.cpp stream-signals.hpp multistream-live.cpp multistream-live.hpp
    audio-watch.cpp audio-watch.hpp level-accumulator.hpp dock-visibility-gate.cpp dock-visibility-gate.hpp
    levels-handoff.hpp levels-math.cpp levels-math.hpp loudness.cpp loudness.hpp
    loudness-worker.cpp loudness-worker.hpp spsc-ring.hpp levels-lufs.cpp levels-lufs.hpp
    levels-meters.cpp levels-meters.hpp levels-panel.cpp levels-panel.hpp
    inflight-controller.cpp inflight-controller.hpp inflight-settings.cpp inflight-settings.hpp
    inflight-import.cpp inflight-import.hpp inflight-settings-check.cpp inflight-settings-check.hpp
    settings-dialog.cpp settings-dialog.hpp inflight-module.cpp inflight-module.hpp
)
set(COMMON_FILES
    plugin-support.h obs-websocket-api.h run-generation.hpp update-check.cpp update-check.hpp
    update-manifest.cpp update-manifest.hpp kit-paths.cpp kit-paths.hpp kit-paths-obs.cpp kit-cost.hpp
)
set(KIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/kit")
set(KIT_FILES
    kit-stage.cpp kit-stage.hpp kit-settings.cpp kit-settings.hpp kit-settings-dialog.cpp kit-settings-dialog.hpp
    kit-dock.cpp kit-dock.hpp
)
set(KIT_ABS)
foreach(f IN LISTS KIT_FILES)
  list(APPEND KIT_ABS "${KIT_SRC}/${f}")
endforeach()

set(MS_ABS)
foreach(f IN LISTS MS_FILES)
  list(APPEND MS_ABS "${MS_SRC}/${f}")
endforeach()
set(PF_ABS)
foreach(f IN LISTS PF_FILES)
  list(APPEND PF_ABS "${PF_SRC}/${f}")
endforeach()
set(IF_ABS)
foreach(f IN LISTS IF_FILES)
  list(APPEND IF_ABS "${IF_SRC}/${f}")
endforeach()
set(COMMON_ABS)
foreach(f IN LISTS COMMON_FILES)
  list(APPEND COMMON_ABS "${COMMON_SRC}/${f}")
endforeach()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp ${COMMON_ABS} ${KIT_ABS} ${MS_ABS} ${PF_ABS} ${IF_ABS}
)

# Include path: the common folder first (so "update-check.hpp" etc. can only mean the one copy), then the
# three tool folders. A quoted include still finds a sibling in its own folder first, so same-named files
# of different tools (settings-dialog.hpp) never cross.
target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${COMMON_SRC}" "${KIT_SRC}" "${MS_SRC}" "${PF_SRC}" "${IF_SRC}"
)

# Log prefix: one PLUGIN_NAME would make every line say [rws-stream-kit]; the harnesses parse the
# per-tool prefixes, so each tool's sources define RWS_LOG_PREFIX (see common/plugin-support.h).
set_source_files_properties(${MS_ABS} PROPERTIES COMPILE_DEFINITIONS "RWS_LOG_PREFIX=\"rws-multistream\"")
set_source_files_properties(${PF_ABS} PROPERTIES COMPILE_DEFINITIONS "RWS_LOG_PREFIX=\"rws-preflight\"")
set_source_files_properties(${IF_ABS} PROPERTIES COMPILE_DEFINITIONS "RWS_LOG_PREFIX=\"rws-inflight\"")

if(WIN32)
  # CredWriteW / CredReadW for stream keys (Multistream); WinHTTP for the update check and the
  # bandwidth/upload tests
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Advapi32 Winhttp)
endif()

# ---- unit tests (no OBS) -------------------------------------------------------------------
# Every unit-test executable of the three tools, same target names as before, plus the two Kit tests.
# Run: build_x64\<config>\<name>.exe
option(BUILD_STREAMKIT_TESTS "Build Red Warden Stream Kit unit tests" ON)
if(BUILD_STREAMKIT_TESTS)
  find_package(Threads REQUIRED)

  # rws_unit_test(<name> TOOL <multistream|preflight|inflight|kit> SOURCES <tool-relative files...>
  #               [LIBS <libs...>])
  function(rws_unit_test name)
    cmake_parse_arguments(T "" "TOOL" "SOURCES;LIBS" ${ARGN})
    if(T_TOOL STREQUAL "multistream")
      set(base "${MS_SRC}/..")
    elseif(T_TOOL STREQUAL "preflight")
      set(base "${PF_SRC}/..")
    elseif(T_TOOL STREQUAL "inflight")
      set(base "${IF_SRC}/..")
    else()
      set(base "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(srcs)
    foreach(s IN LISTS T_SOURCES)
      if(s MATCHES "^common/")
        list(APPEND srcs "${CMAKE_CURRENT_SOURCE_DIR}/src/${s}")
      else()
        list(APPEND srcs "${base}/${s}")
      endif()
    endforeach()
    add_executable(${name} ${srcs})
    target_include_directories(${name} PRIVATE "${base}/src" "${COMMON_SRC}")
    if(T_LIBS)
      target_link_libraries(${name} PRIVATE ${T_LIBS})
    endif()
  endfunction()

  # Multistream
  rws_unit_test(rws-multistream-tests TOOL multistream SOURCES tests/retry-governor-test.cpp)
  # Standalone upload-speed check: build_x64\<config>\rws-multistream-uploadtest.exe
  rws_unit_test(rws-multistream-uploadtest TOOL multistream SOURCES tests/upload-test-cli.cpp src/upload-test.cpp)
  if(WIN32)
    target_link_libraries(rws-multistream-uploadtest PRIVATE Winhttp)
  endif()
  rws_unit_test(rws-multistream-importers-test TOOL multistream SOURCES tests/importers-test.cpp src/importers.cpp LIBS Qt6::Core)
  rws_unit_test(rws-multistream-session-test TOOL multistream SOURCES tests/session-report-test.cpp src/session-report.cpp LIBS Qt6::Core)
  rws_unit_test(rws-multistream-limits-test TOOL multistream SOURCES tests/platform-limits-test.cpp src/platform-limits.cpp LIBS Qt6::Core)
  rws_unit_test(rws-multistream-update-test TOOL multistream SOURCES tests/update-check-test.cpp common/update-manifest.cpp LIBS Qt6::Core)
  rws_unit_test(rws-multistream-serverurl-test TOOL multistream SOURCES tests/server-url-test.cpp src/server-url.cpp LIBS Qt6::Core)

  # Pre-Flight
  rws_unit_test(rws-preflight-update-test TOOL preflight SOURCES tests/update-check-test.cpp common/update-manifest.cpp LIBS Qt6::Core)
  rws_unit_test(rws-preflight-eval-test TOOL preflight SOURCES tests/preflight-eval-test.cpp src/preflight-eval.cpp LIBS Qt6::Core)
  rws_unit_test(rws-preflight-disk-test TOOL preflight SOURCES tests/disk-probe-test.cpp src/disk-probe.cpp LIBS Qt6::Core)
  rws_unit_test(rws-preflight-rungen-test TOOL preflight SOURCES tests/run-generation-test.cpp)

  # In-Flight
  rws_unit_test(rws-inflight-update-test TOOL inflight SOURCES tests/update-check-test.cpp common/update-manifest.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-settings-test TOOL inflight SOURCES tests/settings-import-test.cpp src/inflight-import.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-settings-check-test TOOL inflight SOURCES tests/settings-check-test.cpp src/inflight-settings-check.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-eval-test TOOL inflight SOURCES tests/inflight-eval-test.cpp src/inflight-eval.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-ui-test TOOL inflight SOURCES tests/ui-logic-test.cpp src/inflight-ui-logic.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-probe-test TOOL inflight SOURCES tests/probe-logic-test.cpp src/inflight-probe-logic.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-level-test TOOL inflight SOURCES tests/level-accumulator-test.cpp LIBS Threads::Threads)
  rws_unit_test(rws-inflight-levels-test TOOL inflight SOURCES tests/levels-math-test.cpp src/levels-math.cpp src/inflight-ui-logic.cpp LIBS Qt6::Core)
  rws_unit_test(rws-inflight-loudness-test TOOL inflight SOURCES tests/loudness-test.cpp src/loudness.cpp)
  rws_unit_test(rws-inflight-ring-test TOOL inflight SOURCES tests/spsc-ring-test.cpp src/loudness-worker.cpp src/loudness.cpp LIBS Threads::Threads)

  # Stream Kit
  rws_unit_test(rws-kit-paths-test TOOL kit SOURCES tests/kit-paths-test.cpp common/kit-paths.cpp)
  rws_unit_test(rws-kit-stage-test TOOL kit SOURCES tests/kit-stage-test.cpp src/kit/kit-stage.cpp)
  rws_unit_test(rws-kit-settings-test TOOL kit SOURCES tests/kit-settings-test.cpp src/kit/kit-settings.cpp common/update-manifest.cpp LIBS Qt6::Core)
  target_include_directories(rws-kit-settings-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/kit")
  rws_unit_test(rws-kit-locale-test TOOL kit SOURCES tests/locale-test.cpp)
  target_compile_definitions(rws-kit-locale-test PRIVATE "RWS_LOCALE_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini\"")
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
