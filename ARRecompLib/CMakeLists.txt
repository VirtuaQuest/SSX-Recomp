project(ARRecompLib CXX)

# The recompiled code is generated from private/default.xex by XenonRecomp:
#   XenonRecomp config/AR.toml <XenonRecomp>/XenonUtils/ppc_context.h
# Re-run CMake after regenerating, as the number of output files can change.
file(GLOB AR_RECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ppc/*.cpp")

if (NOT AR_RECOMP_SOURCES)
    message(FATAL_ERROR "No recompiled sources found in ${CMAKE_CURRENT_SOURCE_DIR}/ppc. Run XenonRecomp first.")
endif()

add_library(ARRecompLib STATIC ${AR_RECOMP_SOURCES})

target_include_directories(ARRecompLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(ARRecompLib PRIVATE "${XENONRECOMP_ROOT}/thirdparty/simde")
target_precompile_headers(ARRecompLib PRIVATE "ppc/ppc_recomp_shared.h")

# Guest code relies on exact floating point behavior and type punning through memory.
target_compile_options(ARRecompLib PRIVATE
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/ar_memory.h"
    -march=sandybridge
    -fno-strict-aliasing
    /fp:strict
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable)
