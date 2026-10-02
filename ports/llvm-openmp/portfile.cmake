# LLVM 23 以降は OpenMP 単体のソース配布と standalone ビルドがないため、
# llvm-project の tarball から必要なディレクトリだけを展開し、runtimes 経由でビルドする。
set(LLVM_SRC "llvm-project-${VERSION}.src")

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/llvm/llvm-project/releases/download/llvmorg-${VERSION}/${LLVM_SRC}.tar.xz"
    FILENAME "${LLVM_SRC}.tar.xz"
    SHA512 91174e69ea787034f8a6b17120aae5f139204e13292119d1b066b4930dcea586c76b82228d3db281d8d4211384c7cb74c6b61fdef41b39d310908a8c7cf080cc
)

set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/${LLVM_SRC}")
file(REMOVE_RECURSE "${SOURCE_PATH}")
file(MAKE_DIRECTORY "${CURRENT_BUILDTREES_DIR}/src")
vcpkg_execute_required_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${ARCHIVE}" --
        "${LLVM_SRC}/openmp" "${LLVM_SRC}/runtimes" "${LLVM_SRC}/cmake"
        "${LLVM_SRC}/llvm/cmake" "${LLVM_SRC}/llvm/utils/llvm-lit" "${LLVM_SRC}/third-party"
    WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}/src"
    LOGNAME extract-${TARGET_TRIPLET}
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/runtimes"
    OPTIONS
        -DLLVM_ENABLE_RUNTIMES=openmp
        -DLIBOMP_ENABLE_SHARED=ON
        -DLIBOMP_OMPD_SUPPORT=OFF
        -DLIBOMP_OMPT_SUPPORT=OFF
        -DOPENMP_ENABLE_OMPT_TOOLS=OFF
        -DLIBOMP_INSTALL_ALIASES=OFF
        -DOPENMP_ENABLE_TESTING=OFF
        -DLLVM_INCLUDE_TESTS=OFF
        -DLLVM_INCLUDE_DOCS=OFF
)
vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(GLOB _gtest LIST_DIRECTORIES false "${CURRENT_PACKAGES_DIR}/lib/libruntimes_gtest*" "${CURRENT_PACKAGES_DIR}/debug/lib/libruntimes_gtest*")
if(_gtest)
    file(REMOVE ${_gtest})
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake"
               "${CURRENT_PACKAGES_DIR}/share/openmp/vcpkg-cmake-wrapper.cmake" @ONLY)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/openmp/LICENSE.TXT")
