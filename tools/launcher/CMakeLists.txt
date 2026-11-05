add_executable(ARLauncher WIN32 main.cpp ../ui/theme.cpp)
target_include_directories(ARLauncher PRIVATE ../ui)
target_compile_definitions(ARLauncher PRIVATE UNICODE _UNICODE)
target_link_libraries(ARLauncher PRIVATE comctl32 comdlg32)
# Built next to ARRecomp.exe, which it starts.
set_target_properties(ARLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ARRecomp")
add_custom_command(TARGET ARLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/launcher.ini -DTARGET=$<TARGET_FILE_DIR:ARLauncher>/launcher.ini
        -P ${CMAKE_CURRENT_SOURCE_DIR}/install_settings.cmake)
