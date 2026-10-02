# find_package(OpenMP) に、このポートの libomp を使わせる。
# Apple clang は -fopenmp を直接受け付けず、ランタイムも同梱しないため、FindOpenMP にフラグとライブラリを教える。
if(APPLE)
    get_filename_component(_focal_omp_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    foreach(_lang C CXX)
        set(OpenMP_${_lang}_FLAGS "-Xclang -fopenmp -I${_focal_omp_root}/include" CACHE STRING "")
        set(OpenMP_${_lang}_LIB_NAMES "omp" CACHE STRING "")
    endforeach()
    find_library(OpenMP_omp_LIBRARY NAMES omp PATHS "${_focal_omp_root}/lib" NO_DEFAULT_PATH)
    unset(_focal_omp_root)
endif()
_find_package(${ARGS})
