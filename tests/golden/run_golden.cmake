# ゴールデン画像テストの 1 件分。tests/CMakeLists.txt から呼ばれる。
if(NOT EXISTS "${RAW}")
    message("GOLDEN_SKIPPED: ${RAW} がない（tests/data/fetch.sh で取得する）")
    return()
endif()

get_filename_component(out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")

execute_process(COMMAND "${FOCAL}" render "${RAW}" "${OUT}" ${ARGS} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "render failed (${rc})")
endif()

if(UPDATE)
    file(COPY_FILE "${OUT}" "${GOLDEN}")
    message("updated ${GOLDEN}")
    return()
endif()

if(NOT EXISTS "${GOLDEN}")
    message(FATAL_ERROR "golden image missing: ${GOLDEN}（FOCAL_UPDATE_GOLDEN=ON で作成する）")
endif()

execute_process(COMMAND "${FOCAL}" compare "${OUT}" "${GOLDEN}" --max-diff 2 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "golden mismatch: ${GOLDEN}")
endif()
