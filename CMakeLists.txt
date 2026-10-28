# CMake build based on obsproject/obs-plugintemplate (Windows and macOS in CI; build-macos.sh stays for local Mac builds).
cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(CURL REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets CURL::libcurl
)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE version)
endif()

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PLUGIN_VERSION="${_version}")
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/backup-store.cpp
    src/backup-store.hpp
    src/checker-dialog.cpp
    src/checker-dialog.hpp
    src/http-worker.cpp
    src/http-worker.hpp
    src/pe-file.cpp
    src/pe-file.hpp
    src/plugin-main.cpp
    src/plugin-remover.cpp
    src/plugin-updater.cpp
    src/plugin-updater.hpp
    src/plugin-remover.hpp
    src/plugin-scanner.cpp
    src/plugin-scanner.hpp
    src/plugin-usage.cpp
    src/plugin-usage.hpp
    src/update-checker.cpp
    src/update-checker.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# manifest.json (OBS 33 plugin manager) with version and architecture filled in;
# Windows/Linux: into the data folder, macOS: into the bundle resources.
if(WIN32)
  set(_os_arch "windows-x64")
elseif(APPLE)
  set(_os_arch "macos-universal")
else()
  set(_os_arch "linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/manifest.json.in" _manifest)
string(REPLACE "@VERSION@" "${_version}" _manifest "${_manifest}")
string(REPLACE "@OS_ARCH@" "${_os_arch}" _manifest "${_manifest}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" "${_manifest}")
if(APPLE)
  # inside the bundle next to the other resources
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
  set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" PROPERTY MACOSX_PACKAGE_LOCATION Resources)
else()
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" DESTINATION "${CMAKE_PROJECT_NAME}/data")
endif()
