add_library(focal_core STATIC
    util/thread_pool.cpp
    util/unicode.cpp
    util/hash.cpp
    util/file.cpp
    util/omp_threads.cpp
    util/volume.cpp
    edit/settings.cpp
    edit/preset.cpp
    edit/undo_stack.cpp
    edit/editor.cpp
    imaging/raw_decoder.cpp
    imaging/resample.cpp
    imaging/white_balance.cpp
    imaging/tone_curve.cpp
    imaging/color_pipeline.cpp
    imaging/geometry.cpp
    imaging/crop_tool.cpp
    imaging/output_transform.cpp
    imaging/renderer.cpp
    imaging/local_contrast.cpp
    imaging/detail.cpp
    imaging/image_io.cpp
    catalog/sqlite.cpp
    catalog/schema.cpp
    catalog/db_writer.cpp
    catalog/catalog.cpp
    catalog/smart_query.cpp
    catalog/photo_delete.cpp
    thumbs/thumbnail.cpp
    thumbs/thumbnail_service.cpp
    export/exporter.cpp
    import/card_import.cpp
)

target_include_directories(focal_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(focal_core
    PUBLIC
        libraw::raw_r
    PRIVATE
        lcms2::lcms2
        nlohmann_json::nlohmann_json
        JPEG::JPEG
        TIFF::TIFF
        unofficial::sqlite3::sqlite3
        BLAKE3::blake3
        utf8proc::utf8proc
)

# LibRaw が使う OpenMP のスレッド数を、サムネイルプールで 1 に絞るためだけに使う（design.md 5.1 章）。
# core 自身は OpenMP の並列化を使わない。
if(OpenMP_CXX_FOUND)
    target_link_libraries(focal_core PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(focal_core PRIVATE FOCAL_HAVE_OPENMP)
endif()

# 画像処理のホットパスは最適化を強める（-ffast-math は再現性のため使わない）
if(NOT MSVC)
    target_compile_options(focal_core PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()
