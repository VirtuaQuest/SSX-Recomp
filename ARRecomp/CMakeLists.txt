project(ARRecomp CXX)

file(GLOB_RECURSE AR_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(ARRecomp ${AR_RUNTIME_SOURCES})

target_include_directories(ARRecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${XENONRECOMP_ROOT}/thirdparty/simde"
    "${XENONRECOMP_ROOT}/thirdparty/TinySHA1")

target_precompile_headers(ARRecomp PRIVATE "stdafx.h")

target_link_libraries(ARRecomp PRIVATE
    ARRecompLib
    XenonUtils
    fmt::fmt
    o1heap
    winmm
    dbghelp)

target_compile_options(ARRecomp PRIVATE
    -march=sandybridge
    -fno-strict-aliasing
    /EHsc
    -Wno-unused-variable
    -Wno-unused-parameter)

# The main guest thread runs on a host thread created by the runtime, so the default stack is enough here.
set_target_properties(ARRecomp PROPERTIES OUTPUT_NAME "ARRecomp")
