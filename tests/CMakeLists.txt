find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(focal_tests
    test_settings.cpp
    test_preset.cpp
    test_photo_files.cpp
    test_white_balance.cpp
    test_pipeline.cpp
    test_local_contrast.cpp
    test_detail.cpp
    test_geometry.cpp
    test_image_io.cpp
    test_raw.cpp
    test_unicode_file.cpp
    test_schema.cpp
    test_catalog.cpp
    test_library.cpp
    test_card_import.cpp
    test_delete.cpp
    test_thumbnail.cpp
    test_editor.cpp
    test_crop_tool.cpp
    test_export.cpp
)
target_link_libraries(focal_tests PRIVATE focal_core Catch2::Catch2WithMain)
target_compile_definitions(focal_tests PRIVATE FOCAL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
# 表示用の GPU レンダラーが CPU 版と一致するか（macOS のみ）
if(TARGET focal_gpu)
    target_sources(focal_tests PRIVATE test_gpu.cpp)
    target_link_libraries(focal_tests PRIVATE focal_gpu)
endif()
# macOS の ImageIO で HEIF を読む（v3.22）
if(TARGET focal_platform_apple)
    target_sources(focal_tests PRIVATE test_apple_image.cpp)
    target_link_libraries(focal_tests PRIVATE focal_platform_apple)
endif()
if(WIN32)
    # テスト名が日本語なので、catch_discover_tests が作る「名前を引数に渡す」実行は、Windows のコマンドラインの
    # 文字コードで名前が化けて「該当なし」になる（CI で全件が 0.02 秒で失敗した）。Windows は全体を 1 回で走らせる
    add_test(NAME focal_tests COMMAND focal_tests)
    set_tests_properties(focal_tests PROPERTIES SKIP_RETURN_CODE 4)
else()
    catch_discover_tests(focal_tests SKIP_RETURN_CODE 4)
endif()

# ゴールデン画像テスト（12 章）。CLI でレンダリングして保存済み画像と比較する（最大差 2/255）。
# 更新: cmake -DFOCAL_UPDATE_GOLDEN=ON ... で再構成してから ctest -R golden（process_version を変えるときだけ）
option(FOCAL_UPDATE_GOLDEN "Overwrite golden images instead of comparing" OFF)

set(GOLDEN_RAWS canon_eos_m50.CR3 nikon_z7.NEF sony_ilce7m3.ARW fujifilm_xt3.RAF ricoh_gr3.DNG)
set(GOLDEN_EDIT_ARGS --ev 0.3 --contrast 25 --highlights -40 --shadows 30 --temp 4800 --tint 8
                     --rotate 1 --straighten 2.5 --crop 0.05,0.1,0.85,0.8)
# v3.9 で足した項目（白・黒・明るさ・彩度・自然な彩度・明瞭度）。既存のゴールデン画像は変えずに種類を足した
set(GOLDEN_LOOK_ARGS --whites 20 --blacks -15 --brightness 10 --saturation 20 --vibrance 30 --clarity 40)
# v3.13 のノイズ低減・シャープネスは原寸で効くので、書き出し経路（フル解像度）で確かめる
set(GOLDEN_DETAIL_ARGS --nr 40 --color-nr 40 --sharpness 70)
set(GOLDEN_VARIANTS preview_default preview_edit export_edit preview_look export_detail)
set(GOLDEN_ARGS_preview_default --proxy 512)
set(GOLDEN_ARGS_preview_edit --proxy 512 ${GOLDEN_EDIT_ARGS})
# 書き出し経路はフル解像度でデコードする。X-Trans の half_size は OpenMP 有効時に結果が実行ごとに変わるため使わない
# （CLAUDE.md「既知の問題」）
set(GOLDEN_ARGS_export_edit --long-edge 512 --bits 8 ${GOLDEN_EDIT_ARGS})
set(GOLDEN_ARGS_preview_look --proxy 512 ${GOLDEN_EDIT_ARGS} ${GOLDEN_LOOK_ARGS})
set(GOLDEN_ARGS_export_detail --long-edge 512 --bits 8 ${GOLDEN_EDIT_ARGS} ${GOLDEN_DETAIL_ARGS})

foreach(raw IN LISTS GOLDEN_RAWS)
    get_filename_component(stem ${raw} NAME_WE)
    foreach(vname IN LISTS GOLDEN_VARIANTS)
        string(REPLACE ";" "\\;" vargs_escaped "${GOLDEN_ARGS_${vname}}")
        add_test(NAME golden.${stem}.${vname}
            COMMAND ${CMAKE_COMMAND}
                -DFOCAL=$<TARGET_FILE:focal>
                -DRAW=${CMAKE_CURRENT_SOURCE_DIR}/data/${raw}
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/golden_out/${stem}.${vname}.tif
                -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden/${stem}.${vname}.tif
                -DARGS=${vargs_escaped}
                -DUPDATE=${FOCAL_UPDATE_GOLDEN}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/golden/run_golden.cmake)
        set_tests_properties(golden.${stem}.${vname} PROPERTIES SKIP_REGULAR_EXPRESSION "GOLDEN_SKIPPED")
    endforeach()
endforeach()

# C API のテスト（C だけで書き、C API だけを使う）。pthread などの POSIX を使うので Windows では作らない
if(NOT WIN32)
add_executable(focal_capi_tests capi/test_capi.c)
target_link_libraries(focal_capi_tests PRIVATE focal_capi focal_core)
if(TARGET focal_gpu)
    target_link_libraries(focal_capi_tests PRIVATE focal_gpu)
endif()
set_target_properties(focal_capi_tests PROPERTIES LINKER_LANGUAGE CXX C_STANDARD 11)
target_compile_definitions(focal_capi_tests PRIVATE FOCAL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME capi COMMAND focal_capi_tests)
set_tests_properties(capi PROPERTIES SKIP_RETURN_CODE 77)
endif()
