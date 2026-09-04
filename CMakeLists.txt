cmake_minimum_required(VERSION 3.20)

# Single source of truth for CamSyringe's own version: version.txt at the
# repo root. Compiled into the binary below (CAMSYRINGE_VERSION, shown by
# Help > About) AND read directly by release/create-cam-syringe-bundle.sh
# and release/create-windows-bundle.sh (neither takes a version argument
# of its own anymore) -- one file, one number, nothing left to drift out
# of sync between the binary and its own release bundles.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" CAMSYRINGE_VERSION LIMIT_COUNT 1)
project(camsyringe VERSION ${CAMSYRINGE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)

find_package(Qt6 COMPONENTS Widgets REQUIRED)
find_package(Threads REQUIRED)

# vector_blf (Phase 3, BLF parsing) -- vendored as a git submodule under
# third_party/ (GPLv3, github.com/Technica-Engineering/vector_blf -- the
# tobylorenz/Tobi1kenobi names in earlier notes are stale, that repo moved).
# Its own CMakeLists.txt only builds the library itself (Vector_BLF target)
# plus optional doxygen/tests/examples/static-analysis targets we don't
# want pulled into our build -- force all of those off via cache variables
# BEFORE add_subdirectory(), the standard way to override a vendored
# project's own options without patching its CMakeLists.txt.
set(OPTION_RUN_DOXYGEN OFF CACHE BOOL "" FORCE)
set(OPTION_RUN_CCCC OFF CACHE BOOL "" FORCE)
set(OPTION_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(OPTION_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPTION_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPTION_USE_GCOV OFF CACHE BOOL "" FORCE)
set(OPTION_USE_GPROF OFF CACHE BOOL "" FORCE)
set(OPTION_ADD_LCOV OFF CACHE BOOL "" FORCE)
# vector_blf's OWN CMakeLists.txt (src/Vector/BLF/CMakeLists.txt) does
# `include_directories(${CMAKE_SOURCE_DIR}/src ...)` to find its own
# headers -- CMAKE_SOURCE_DIR is a known gotcha for any project not
# designed to be add_subdirectory()'d: it always resolves to the
# OUTERMOST project's root (this CMakeLists.txt's directory), not
# vector_blf's own root, regardless of vector_blf's own nested project()
# call. Confirmed for real: without this line, the very first vector_blf
# source file fails with "Vector/BLF/A429BusStatistic.h: No such file or
# directory" -- it was searching camsyringe's own src/ instead of
# vector_blf's. Fixed WITHOUT patching the vendored submodule: directory-
# scoped include_directories() calls accumulate (don't replace) and are
# inherited by add_subdirectory()'d children, so adding the CORRECT path
# here, before add_subdirectory() below, works alongside their wrong one
# -- the compiler just has an extra, harmlessly-unused search path too.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/vector_blf/src)
# Same CMAKE_BINARY_DIR-vs-CMAKE_CURRENT_BINARY_DIR version of the exact
# same bug, for their GENERATED config.h (configure_file(config.h.in
# config.h), relative -- correctly lands under
# <our-build-dir>/third_party/vector_blf/src/Vector/BLF/config.h since
# THAT part uses CMAKE_CURRENT_BINARY_DIR correctly, but their own
# `#include <Vector/BLF/config.h>` search path looks for it under
# CMAKE_BINARY_DIR/src instead, same outermost-project-root mistake as
# above).
include_directories(${CMAKE_CURRENT_BINARY_DIR}/third_party/vector_blf/src)
add_subdirectory(third_party/vector_blf)

add_executable(camsyringe
    src/main.cpp
    src/blf/BlfLoader.cpp
    src/blf/BlfReplayer.cpp
    src/camera/CameraStream.cpp
    src/camera/StreamPool.cpp
    src/net/CameraGeometryResolver.cpp
    src/net/DispatcherClient.cpp
    src/net/DispatcherRemoteControl.cpp
    src/net/DispatcherVersionProbe.cpp
    src/net/InjectorBundleInstaller.cpp
    src/net/TargetSsh.cpp
    src/net/TcpConnect.cpp
    src/orchestrator/Timeline.cpp
    src/ui/MainWindow.cpp
    src/ui/CameraWidget.cpp
    src/ui/CameraConfigDialog.cpp
    src/ui/SshCredentialsDialog.cpp
    src/util/InjectorBundleFinder.cpp
)

target_include_directories(camsyringe PRIVATE src)
target_link_libraries(camsyringe PRIVATE PkgConfig::FFMPEG Qt6::Widgets Threads::Threads Vector_BLF)
target_compile_definitions(camsyringe PRIVATE CAMSYRINGE_VERSION="${CAMSYRINGE_VERSION}")
