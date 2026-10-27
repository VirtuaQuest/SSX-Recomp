add_executable(ARShaderTool main.cpp "${CMAKE_SOURCE_DIR}/ARRecomp/gpu/hlsl_translator.cpp")
target_include_directories(ARShaderTool PRIVATE "${CMAKE_SOURCE_DIR}/ARRecomp")
target_link_libraries(ARShaderTool PRIVATE xenia_shader)
target_compile_options(ARShaderTool PRIVATE /EHsc)
