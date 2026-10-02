#include "util/unicode.h"

#include <utf8proc.h>

#include <cstdlib>

namespace focal {

namespace {

std::string map(std::string_view s, int options) {
    utf8proc_uint8_t* out = nullptr;
    const utf8proc_ssize_t n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(s.data()),
                                            static_cast<utf8proc_ssize_t>(s.size()), &out,
                                            static_cast<utf8proc_option_t>(options));
    if (n < 0) return std::string(s);
    std::string r(reinterpret_cast<const char*>(out), static_cast<size_t>(n));
    std::free(out);
    return r;
}

} // namespace

std::string to_nfc(std::string_view utf8) { return map(utf8, UTF8PROC_STABLE | UTF8PROC_COMPOSE); }

std::string casefold_key(std::string_view utf8) {
    return map(utf8, UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
}

} // namespace focal
