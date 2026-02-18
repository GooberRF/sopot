# CMake 3.15 is required for CMAKE_MSVC_RUNTIME_LIBRARY.
cmake_minimum_required(VERSION 3.15)
project(SOPOT)

if(NOT "${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
    message(FATAL_ERROR "Only x86 (32 bit) platform is supported!")
endif()

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _CRT_SECURE_NO_DEPRECATE
        _CRT_NONSTDC_NO_DEPRECATE
        _SCL_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# Keep parity with the legacy game runtime environment.
add_compile_definitions(
    WINVER=0x0501
    _WIN32_WINNT=0x0601
    _WIN32_IE=0x0501
    PSAPI_VERSION=1
    RF2PATCH
    "$<$<CONFIG:Debug>:DEBUG>"
)

if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MANIFEST:NO")
else()
    # Use static linking for MinGW builds.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")
endif()

# Output all executable and DLL files into the bin directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/arch:SSE2)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-msse2)
endif()

macro(enable_warnings target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wundef)
    else()
        target_compile_options(${target} PRIVATE /W3)
    endif()
endmacro()

macro(setup_debug_info target)
    if(MSVC)
        target_link_options(${target} PRIVATE /DEBUG)
    else()
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-g>$<$<CONFIG:RelWithDebInfo>:-g>)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug "$<TARGET_FILE:${target}>" "$<TARGET_FILE:${target}>.debug"
            COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded "$<TARGET_FILE:${target}>"
            COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink="$<TARGET_FILE:${target}>.debug" "$<TARGET_FILE:${target}>")
    endif()
endmacro()

add_subdirectory(vendor)
add_subdirectory(xlog)
add_subdirectory(common)
add_subdirectory(patch_common)
add_subdirectory(crash_handler_stub)
add_subdirectory(launcher_common)
add_subdirectory(game_patch)
add_subdirectory(launcher)
