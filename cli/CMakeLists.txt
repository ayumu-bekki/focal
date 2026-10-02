add_executable(focal
    main.cpp
    args.cpp
    cmd_render.cpp
    cmd_info.cpp
    cmd_compare.cpp
    cmd_catalog.cpp
    ${PROJECT_SOURCE_DIR}/bench/cmd_bench.cpp
)
target_include_directories(focal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(focal PRIVATE focal_core)

# bench で表示用の GPU レンダラーも測る（macOS のみ）
if(TARGET focal_gpu)
    target_link_libraries(focal PRIVATE focal_gpu)
    target_compile_definitions(focal PRIVATE FOCAL_HAVE_GPU)
endif()
