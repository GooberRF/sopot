set(SRCS
    main/main.cpp
    main/main.h
    core/console.cpp
    core/console.h
    core/frame_limiter.cpp
    core/frame_limiter.h
    core/high_fps.cpp
    core/high_fps.h
    misc/misc.cpp
    misc/misc.h
    player/camera.cpp
    player/camera.h
    rf2/rf2.h
    rf2/gr/gr.h
    rf2/os/console.h
    rf2/os/input.h
    rf2/os/window.h
    rf2/player/autoaim.h
    rf2/player/camera.h
    rf2/player/reticle.h
    res/resources.rc
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRCS})

add_library(rf2patch SHARED ${SRCS})
target_compile_features(rf2patch PUBLIC cxx_std_20)
set_target_properties(rf2patch PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Sopot"
    CXX_EXTENSIONS NO
)
enable_warnings(rf2patch)
setup_debug_info(rf2patch)

target_compile_definitions(rf2patch PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    BUILD_DLL
)

target_include_directories(rf2patch PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/d3d8
)

target_link_libraries(rf2patch
    PatchCommon
    Xlog
    Common
    CrashHandlerStub
)
