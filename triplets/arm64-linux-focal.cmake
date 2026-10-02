# ADR-01 / ADR-11: LibRaw (LGPL) は動的リンク、それ以外は静的リンク（Linux、CI 用）
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(PORT STREQUAL "libraw")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
