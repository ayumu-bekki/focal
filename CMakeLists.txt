cmake_minimum_required(VERSION 3.25)
project(focal VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FOCAL_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(FOCAL_TSAN "Build with ThreadSanitizer" OFF)
option(FOCAL_BUILD_TESTS "Build tests" ON)
# レンズ補正（Lensfun、LGPL-3.0 を動的リンク。v3.27）。vcpkg.json で macOS だけ入れる
option(FOCAL_ENABLE_LENSFUN "Enable lens correction with Lensfun" ${APPLE})

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
find_package(PNG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(BLAKE3 CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(OpenMP)

if(FOCAL_ENABLE_LENSFUN)
    # Lensfun に CMake の設定ファイルはない。pkg-config は使わない（ホストの MacPorts 等を拾うため）。
    # vcpkg のインストール先だけを見る
    set(_focal_vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    find_path(LENSFUN_INCLUDE_DIR lensfun/lensfun.h PATHS "${_focal_vcpkg_prefix}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(LENSFUN_LIBRARY NAMES lensfun PATHS "${_focal_vcpkg_prefix}/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(lensfun::lensfun SHARED IMPORTED)
    set_target_properties(lensfun::lensfun PROPERTIES
        IMPORTED_LOCATION "${LENSFUN_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LENSFUN_INCLUDE_DIR}")
    message(STATUS "Lensfun: ${LENSFUN_LIBRARY}")
endif()

add_subdirectory(core)
# 表示用の GPU レンダラー（Metal）は macOS だけ
if(APPLE)
    add_subdirectory(gpu)
    # macOS の画像の読み取り（ImageIO で HEIF を読む。v3.22）
    add_subdirectory(platform/apple)
endif()
add_subdirectory(capi)
add_subdirectory(cli)

if(FOCAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
