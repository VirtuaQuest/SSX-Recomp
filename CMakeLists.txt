cmake_minimum_required(VERSION 3.20)

project(AnarchyReignsRecomp CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# RelWithDebInfo defaults to /Ob1 (only functions marked inline are inlined), which costs the recompiled code and
# the renderer a lot; inline like Release does.
foreach (flags CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REPLACE "/Ob1" "/Ob2" ${flags} "${${flags}}")
endforeach()

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The recompiled code requires Clang (clang-cl on Windows).")
endif()

# XenonRecomp provides the recompiler, XenonUtils (XEX loading) and shared third party libraries: XENONRECOMP_ROOT, a
# checkout next to this folder (where it's worked on), or the copy in thirdparty/XenonRecomp (the submodule, and what a
# release carries).
if (NOT XENONRECOMP_ROOT AND DEFINED ENV{XENONRECOMP_ROOT})
    set(XENONRECOMP_ROOT "$ENV{XENONRECOMP_ROOT}")
endif()
if (NOT XENONRECOMP_ROOT)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp/XenonUtils/ppc_context.h")
        set(XENONRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp")
    else()
        set(XENONRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenonRecomp")
    endif()
endif()
set(XENONRECOMP_ROOT "${XENONRECOMP_ROOT}" CACHE PATH "Path to the XenonRecomp checkout" FORCE)
if (NOT EXISTS "${XENONRECOMP_ROOT}/XenonUtils/ppc_context.h")
    message(FATAL_ERROR "XenonRecomp was not found (XENONRECOMP_ROOT: '${XENONRECOMP_ROOT}'). Run git submodule update --init "
        "--recursive, or clone the xenon-ssx branch next to this folder as XenonRecomp.")
endif()
add_subdirectory(${XENONRECOMP_ROOT} XenonRecomp EXCLUDE_FROM_ALL)

# The source's version (VERSION). A build knows which one it was made from, so an update to the source that hasn't been
# built yet is noticed when the game starts.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AR_SOURCE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

add_library(o1heap STATIC "thirdparty/o1heap/o1heap.c")
target_include_directories(o1heap PUBLIC "thirdparty/o1heap")
# o1heap counts leading zeros of size_t with __builtin_clzl by default, which is 32 bit on Windows.
target_compile_definitions(o1heap PRIVATE O1HEAP_CLZ=__builtin_clzll)

add_subdirectory(thirdparty/xenia)
# The game's code comes from the player's copy of the game, by the recompiler above: before that exists, only the tools can be built (Setup.bat does this).
file(GLOB AR_GENERATED_CODE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ARRecompLib/ppc/*.cpp")
if (AR_GENERATED_CODE)
    add_subdirectory(ARRecompLib)
    add_subdirectory(ARRecomp)
else()
    message(STATUS "No recompiled code yet: only the tools can be built.")
endif()
add_subdirectory(tools/shader_tool)
add_subdirectory(tools/launcher)
