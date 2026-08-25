cmake_minimum_required(VERSION 3.20)
project(camsyringe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(camsyringe
    src/main.cpp
    src/camera/CameraStream.cpp
)

target_include_directories(camsyringe PRIVATE src)
target_link_libraries(camsyringe PRIVATE PkgConfig::FFMPEG)
