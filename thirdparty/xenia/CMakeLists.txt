add_library(xenia_shader STATIC
    src/xenia/base/string_buffer.cc
    src/xenia/gpu/registers.cc
    src/xenia/gpu/shader.cc
    src/xenia/gpu/shader_translator.cc
    src/xenia/gpu/shader_translator_disasm.cc
    src/xenia/gpu/ucode.cc
    src/xenia/gpu/xenos.cc
    shim/memory_shim.cc)

target_include_directories(xenia_shader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xenia_shader PUBLIC fmt::fmt)
target_compile_definitions(xenia_shader PUBLIC NOMINMAX)
target_compile_options(xenia_shader PRIVATE /EHsc -Wno-everything)
