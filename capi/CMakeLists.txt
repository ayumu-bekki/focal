add_library(focal_capi STATIC src/focal_capi.cpp)
target_include_directories(focal_capi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(focal_capi PRIVATE focal_core)

# macOS では表示を GPU（Metal）で描く（v3.14）
if(TARGET focal_gpu)
    target_link_libraries(focal_capi PRIVATE focal_gpu)
    target_compile_definitions(focal_capi PRIVATE FOCAL_HAVE_GPU)
endif()

# macOS では HEIF を ImageIO で読む（v3.22）
if(TARGET focal_platform_apple)
    target_link_libraries(focal_capi PRIVATE focal_platform_apple)
    target_compile_definitions(focal_capi PRIVATE FOCAL_HAVE_APPLE_IMAGE_READER)
endif()
