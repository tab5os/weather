# Weather for Tab5 OS, set up like github.com/tab5os/app-template.
# Build with ./build.sh (it fetches the SDK and calls this).
# To use CMake directly: cmake -S . -B build -G Ninja [-DT5_SDK=<sdk dir>] && cmake --build build
cmake_minimum_required(VERSION 3.22)

# The SDK: -DT5_SDK, $T5_SDK, or the one ./build.sh fetched into .t5sdk/.
if(NOT T5_SDK)
    set(T5_SDK "$ENV{T5_SDK}")
endif()
if(NOT T5_SDK)
    file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/tab5os-sdk.txt T5_SDK_VERSION LIMIT_COUNT 1)
    set(T5_SDK ${CMAKE_CURRENT_LIST_DIR}/.t5sdk/tab5os-sdk-${T5_SDK_VERSION}/sdk)
endif()
if(NOT EXISTS ${T5_SDK}/cmake/t5_app.cmake)
    message(FATAL_ERROR "No Tab5 OS SDK at ${T5_SDK}: run ./build.sh once, or set T5_SDK")
endif()
set(CMAKE_TOOLCHAIN_FILE ${T5_SDK}/cmake/toolchain-t5.cmake)

project(weather C)
include(${T5_SDK}/cmake/t5_app.cmake)
t5_app(weather SRCS main.c wx.c json.c MANIFEST manifest.toml ICON icon.png)
