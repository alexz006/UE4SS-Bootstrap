cmake_minimum_required(VERSION 3.21)

# MASM is used for the export jump stubs.
project(UE4SSUpdater LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "UE4SSUpdater must be built as x64 (the game and UE4SS are x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT (no VC++ redistributable dependency).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# OFF builds the pure proxy (no updater).
option(UPDATER_ENABLED "Build with the self-updater" ON)

set(PROXY_SOURCES
    src/dllmain.cpp
    src/proxy_exports.asm
    src/proxy.rc
)

set(UPDATER_SOURCES
    src/updater.cpp
    src/progress.cpp
    src/miniz.c
)

if(UPDATER_ENABLED)
    list(APPEND PROXY_SOURCES ${UPDATER_SOURCES})
endif()

add_library(dwmapi SHARED ${PROXY_SOURCES})

# Export table from the generated .def.
set_target_properties(dwmapi PROPERTIES
    OUTPUT_NAME "dwmapi"
    PREFIX ""
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_exports.def\" /VERSION:1.0"
)

target_include_directories(dwmapi PRIVATE src)
target_compile_definitions(dwmapi PRIVATE UNICODE _UNICODE NOMINMAX)

if(UPDATER_ENABLED)
    target_compile_definitions(dwmapi PRIVATE UPDATER_ENABLED)
    target_link_libraries(dwmapi PRIVATE winhttp)
endif()

set_source_files_properties(src/miniz.c PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_ZLIB_COMPATIBLE_NAMES")
