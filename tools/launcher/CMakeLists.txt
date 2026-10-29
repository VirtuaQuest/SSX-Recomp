add_executable(ARLauncher WIN32 main.cpp)
target_compile_definitions(ARLauncher PRIVATE UNICODE _UNICODE)
target_link_libraries(ARLauncher PRIVATE comctl32 comdlg32)
# Built next to ARRecomp.exe, which it starts.
set_target_properties(ARLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ARRecomp")
