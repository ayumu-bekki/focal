# 表示用の GPU レンダラー（Metal、macOS のみ。ADR-03・ADR-05 v3.14）。core は OS に依存しないので別のモジュールにする
find_path(METAL_CPP_INCLUDE_DIR Metal/Metal.hpp PATH_SUFFIXES metal-cpp REQUIRED)

# シェーダーは実行時にコンパイルする（ビルドに Metal のツールチェーンを要求しない）。本文を C++ の文字列にする
set(FOCAL_SHADER_INC ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders.metal.inc)
add_custom_command(
    OUTPUT ${FOCAL_SHADER_INC}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/shaders.metal -DOUT=${FOCAL_SHADER_INC}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders.metal ${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake
    VERBATIM)

add_library(focal_gpu STATIC src/metal_renderer.cpp ${FOCAL_SHADER_INC})
target_include_directories(focal_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                     PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(focal_gpu SYSTEM PRIVATE ${METAL_CPP_INCLUDE_DIR})
target_link_libraries(focal_gpu PUBLIC focal_core "-framework Metal" "-framework Foundation")
