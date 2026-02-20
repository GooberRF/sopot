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

add_library(Sopot SHARED ${SRCS})
target_compile_features(Sopot PUBLIC cxx_std_20)
set_target_properties(Sopot PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Sopot"
    CXX_EXTENSIONS NO
)
enable_warnings(Sopot)
setup_debug_info(Sopot)

target_compile_definitions(Sopot PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    BUILD_DLL
)

target_include_directories(Sopot PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/d3d8
)

target_link_libraries(Sopot
    PatchCommon
    Xlog
    Common
    CrashHandlerStub
)

# Keep d3d8to9.dll built alongside Sopot so runtime D3D8->D3D9 translation is available.
add_dependencies(Sopot d3d8to9)
