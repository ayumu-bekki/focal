# macOS の画像の読み取り（ImageIO。HEIF / HEIC を読む。v3.22）。core は OS に依存しないので別のモジュールにする。
# core が持つ「OS の読み取り部品」の口（set_platform_image_reader）に登録して使う
add_library(focal_platform_apple STATIC src/apple_image_reader.cpp)
target_include_directories(focal_platform_apple PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(focal_platform_apple PUBLIC focal_core
    "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation")
