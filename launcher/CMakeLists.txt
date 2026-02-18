set(SRCS
    main.cpp
    res/resources.rc
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRCS})

add_executable(SopotLauncher WIN32 ${SRCS})

target_compile_features(SopotLauncher PUBLIC cxx_std_20)
set_target_properties(SopotLauncher PROPERTIES CXX_EXTENSIONS NO)
set_target_properties(SopotLauncher PROPERTIES OUTPUT_NAME "SopotLauncher")
enable_warnings(SopotLauncher)
setup_debug_info(SopotLauncher)

target_link_libraries(SopotLauncher
    LauncherCommon
    Xlog
    shell32
    comdlg32
)

add_custom_command(TARGET SopotLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/README.md
        $<TARGET_FILE_DIR:SopotLauncher>/README.md
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/changelog.md
        $<TARGET_FILE_DIR:SopotLauncher>/changelog.md
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/licensing-info.txt
        $<TARGET_FILE_DIR:SopotLauncher>/licensing-info.txt
)
