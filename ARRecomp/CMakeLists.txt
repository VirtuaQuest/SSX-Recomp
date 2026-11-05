project(ARRecomp CXX)

file(GLOB_RECURSE AR_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# The name Windows shows for the program. Its icon isn't in this project: it comes from the player's copy of the game (gpu/game_icon.cpp), and setup puts it in the program afterwards.
enable_language(RC)
set(AR_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/version.rc")

add_executable(ARRecomp ${AR_RUNTIME_SOURCES} ${AR_RESOURCES})

target_include_directories(ARRecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${XENONRECOMP_ROOT}/thirdparty/simde"
    "${XENONRECOMP_ROOT}/thirdparty/TinySHA1")

target_precompile_headers(ARRecomp PRIVATE "stdafx.h")

target_link_libraries(ARRecomp PRIVATE
    ARRecompLib
    XenonUtils
    xenia_shader
    fmt::fmt
    o1heap
    winmm
    xinput
    xaudio2
    ole32
    dbghelp
    d3d12
    dxgi
    dxguid)

# DXC compiles the translated shaders at run time; the Windows SDK ships it.
find_file(AR_DXCOMPILER_DLL dxcompiler.dll PATHS "$ENV{WindowsSdkDir}bin/$ENV{WindowsSDKVersion}x64" "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" NO_DEFAULT_PATH)
if (AR_DXCOMPILER_DLL)
    get_filename_component(AR_DXC_DIR "${AR_DXCOMPILER_DLL}" DIRECTORY)
    add_custom_command(TARGET ARRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AR_DXC_DIR}/dxcompiler.dll" "${AR_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:ARRecomp>")
else()
    message(WARNING "dxcompiler.dll was not found in the Windows SDK; copy it and dxil.dll next to ARRecomp.exe.")
endif()

target_compile_options(ARRecomp PRIVATE
    "/FI${CMAKE_SOURCE_DIR}/ARRecompLib/ar_memory.h"
    -march=sandybridge
    -fno-strict-aliasing
    /EHsc
    -Wno-unused-variable
    -Wno-unused-parameter)

# The main guest thread runs on a host thread created by the runtime, so the default stack is enough here.
set_target_properties(ARRecomp PROPERTIES OUTPUT_NAME "ARRecomp")
