# ADR-01 / ADR-11: LGPL のライブラリ（LibRaw・Lensfun とその依存の GLib・libiconv・libintl）と libomp は動的リンク、それ以外は静的リンク。
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)

# Lensfun（LGPL-3.0）・GLib（LGPL-2.1+）・libiconv・gettext-libintl（LGPL）は、利用者が差し替えられるよう動的リンクにする（v3.27）
if(PORT MATCHES "^(libraw|llvm-openmp|lensfun|glib|libiconv|gettext-libintl)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
