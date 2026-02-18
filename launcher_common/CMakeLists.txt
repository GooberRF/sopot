set(SRCS
    DllInjector.cpp
    DllInjector.h
    InjectingProcessLauncher.cpp
    InjectingProcessLauncher.h
    PatchedAppLauncher.cpp
    Process.h
    Thread.h
    Win32Handle.h
    include/launcher_common/PatchedAppLauncher.h
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRCS})

add_library(LauncherCommon STATIC ${SRCS})

target_compile_features(LauncherCommon PUBLIC cxx_std_20)
set_target_properties(LauncherCommon PROPERTIES CXX_EXTENSIONS NO)
enable_warnings(LauncherCommon)

target_include_directories(LauncherCommon
    PUBLIC
    include
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/launcher_common
)

target_link_libraries(LauncherCommon Common Xlog)
