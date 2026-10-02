# ヘッダのみ。Apple の配布 zip を展開して include/metal-cpp/ に置く
vcpkg_download_distfile(ARCHIVE
    URLS "https://developer.apple.com/metal/cpp/files/metal-cpp_${VERSION}.zip"
    FILENAME "metal-cpp_${VERSION}.zip"
    SHA512 d153f8d26dac38867b49d6ef8f98417191a7cb753908f4798c12c7110ed706e8bc5b72d4d94b9f237ceb36c42b795ae260dc9f3fff03ebf118c5d6e4efc53abe
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

foreach(dir Foundation Metal QuartzCore)
    file(INSTALL "${SOURCE_PATH}/${dir}" DESTINATION "${CURRENT_PACKAGES_DIR}/include/metal-cpp")
endforeach()
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
