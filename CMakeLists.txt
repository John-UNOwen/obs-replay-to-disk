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
endif()
