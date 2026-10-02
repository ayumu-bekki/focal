cmake_minimum_required(VERSION 3.25)
project(focal VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FOCAL_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(FOCAL_TSAN "Build with ThreadSanitizer" OFF)
option(FOCAL_BUILD_TESTS "Build tests" ON)

if(FOCAL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(FOCAL_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(libraw CONFIG REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(BLAKE3 CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(OpenMP)

add_subdirectory(core)
# 表示用の GPU レンダラー（Metal）は macOS だけ
if(APPLE)
    add_subdirectory(gpu)
endif()
add_subdirectory(capi)
add_subdirectory(cli)

if(FOCAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
