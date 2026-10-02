# ADR-01 / ADR-11: LibRaw (LGPL) と libomp は動的リンク、それ以外は静的リンク。
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)

if(PORT MATCHES "^(libraw|llvm-openmp)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
