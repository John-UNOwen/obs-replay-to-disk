cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_TESTS "Build unit tests for the pure-C core and the remux CLI tool" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(FFmpeg REQUIRED avformat avcodec avutil)

add_library(rr_core STATIC)
target_include_directories(rr_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_sources(
  rr_core
  PRIVATE
    src/core/ledger.c
    src/core/paths.c
    src/core/settings-clamp.c
    src/core/space.c
    src/core/window.c
    src/save/remux.c
)
target_link_libraries(rr_core PUBLIC FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE rr_core)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE RR_UI=1)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/ui/settings-dialog.cpp
      src/ui/settings-dialog.hpp
      src/ui/status-dock.cpp
      src/ui/status-dock.hpp
      src/ui/status-util.hpp
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/plugin/engine.c
    src/plugin/platform-win.c
    src/plugin/plugin-ui.h
    src/plugin/rename.c
    src/plugin/settings.c
)

# Widen OBS version support (docs/STATUS.md "Widening OBS compatibility"):
# the plugin delay-loads its three FFmpeg imports and ships the full FFmpeg
# dependency closure inside the plugin folder. OBS's module loader maps the
# plugin DLL fine without FFmpeg on the host (delay imports); at
# obs_module_load, rr_ffmpeg_preload loads the closure from the plugin's own
# directory, dependency-first (Windows only applies LOAD_LIBRARY_SEARCH_*
# to the DLL named in LoadLibraryEx, not to that DLL's own imports). A copy
# OBS already loaded with the same name (same FFmpeg major) is reused.
# The closure list must stay in sync with ffmpeg-load.c. Everything installs
# flat, next to the plugin DLL — the OBS 33 plugin layout (the bin/64bit
# nesting is flagged Legacy; see cmake/windows/helpers.cmake).
if(WIN32 AND FFmpeg_avformat_LIBRARY AND FFmpeg_avcodec_LIBRARY AND FFmpeg_avutil_LIBRARY)
  get_filename_component(RR_FFMPEG_BIN_DIR "${FFmpeg_avutil_LIBRARY}" DIRECTORY)
  set(
    RR_FFMPEG_BUNDLE
    avutil-60.dll
    zlib.dll
    swresample-6.dll
    libx264-164.dll
    srt.dll
    librist.dll
    avcodec-62.dll
    avformat-62.dll
  )
  set(RR_FFMPEG_FILES "")
  foreach(rr_dll ${RR_FFMPEG_BUNDLE})
    if(NOT EXISTS "${RR_FFMPEG_BIN_DIR}/${rr_dll}")
      message(
        FATAL_ERROR
        "FFmpeg closure member ${rr_dll} is missing from ${RR_FFMPEG_BIN_DIR}; "
        "update the bundle list in CMakeLists.txt and ffmpeg-load.c together."
      )
    endif()
    list(APPEND RR_FFMPEG_FILES "${RR_FFMPEG_BIN_DIR}/${rr_dll}")
  endforeach()

  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin/ffmpeg-load.c)

  if(MSVC)
    get_filename_component(RR_DELAY_AVFORMAT "${FFmpeg_avformat_LIBRARY}" NAME)
    get_filename_component(RR_DELAY_AVCODEC "${FFmpeg_avcodec_LIBRARY}" NAME)
    get_filename_component(RR_DELAY_AVUTIL "${FFmpeg_avutil_LIBRARY}" NAME)
    target_link_options(
      ${CMAKE_PROJECT_NAME}
      PRIVATE "/DELAYLOAD:${RR_DELAY_AVFORMAT}" "/DELAYLOAD:${RR_DELAY_AVCODEC}" "/DELAYLOAD:${RR_DELAY_AVUTIL}"
    )
  endif()

  install(FILES ${RR_FFMPEG_FILES} DESTINATION "${CMAKE_PROJECT_NAME}" COMPONENT Runtime)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(ENABLE_TESTS)
  enable_testing()

  add_executable(test_core tests/test_core.c)
  target_link_libraries(test_core PRIVATE rr_core)
  add_test(NAME core COMMAND test_core)

  # The exclusive save rename (D13 no-clobber), testable without libobs.
  add_executable(test_rename tests/test_rename.c src/plugin/rename.c)
  target_include_directories(test_rename PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_test(NAME rename COMMAND test_rename)

  # The rr_remux CLI (same code as the plugin's save step) so the remux path can
  # be exercised outside OBS.
  add_executable(rr-remux-cli src/save/remux.c)
  target_compile_definitions(rr-remux-cli PRIVATE RR_REMUX_MAIN)
  target_link_libraries(rr-remux-cli PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)

  # The FFmpeg preload closure, against the real dependency folder (Windows).
  if(WIN32 AND RR_FFMPEG_BIN_DIR)
    add_executable(test_ffmpeg_load tests/test_ffmpeg_load.c src/plugin/ffmpeg-load.c)
    target_include_directories(test_ffmpeg_load PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(test_ffmpeg_load PRIVATE RR_TEST_FFMPEG_DIR="${RR_FFMPEG_BIN_DIR}")
    add_test(NAME ffmpeg_load COMMAND test_ffmpeg_load)
  endif()
endif()
