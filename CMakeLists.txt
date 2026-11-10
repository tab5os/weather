# Weather for Tab5 OS. Build with the Tab5 OS SDK (API level 1):
#   t5 app-build .                         # from a tab5os checkout or the SDK release
#   cmake -S . -B build -G Ninja -DT5_SDK=<tab5os or SDK>/sdk && cmake --build build
cmake_minimum_required(VERSION 3.22)
if(NOT T5_SDK)
    set(T5_SDK "$ENV{T5_SDK}")
endif()
if(NOT T5_SDK)
    message(FATAL_ERROR "Set T5_SDK to the sdk/ folder of a tab5os checkout or SDK release (or build with `t5 app-build .`)")
endif()
set(CMAKE_TOOLCHAIN_FILE ${T5_SDK}/cmake/toolchain-t5.cmake)
project(weather C)
include(${T5_SDK}/cmake/t5_app.cmake)
t5_app(weather SRCS main.c wx.c json.c MANIFEST manifest.toml ICON icon.png)
