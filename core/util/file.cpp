#include "util/file.h"

#include <future>
#include <memory>
#include <thread>

#include <sys/stat.h>

#include <cstring>
#include <system_error>

#include "util/unicode.h"

namespace focal {

namespace fs = std::filesystem;

std::string path_to_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path utf8_to_path(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

FILE* open_file(const fs::path& path, const char* mode) {
#if defined(_WIN32)
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(path.wstring().c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::optional<FileStat> stat_file(const fs::path& path) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_wstat64(path.wstring().c_str(), &st) != 0) return std::nullopt;
    if ((st.st_mode & _S_IFREG) == 0) return std::nullopt;
    return FileStat{st.st_size, st.st_mtime};
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    if (!S_ISREG(st.st_mode)) return std::nullopt;
    return FileStat{static_cast<int64_t>(st.st_size), static_cast<int64_t>(st.st_mtime)};
#endif
}

std::optional<fs::path> find_entry_nfc(const fs::path& dir, std::string_view name_nfc) {
    std::error_code ec;
    const fs::path direct = dir / utf8_to_path(name_nfc);
    if (fs::exists(direct, ec)) return direct;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (to_nfc(path_to_utf8(it->path().filename())) == name_nfc) return it->path();
    }
    return std::nullopt;
}

std::optional<fs::path> resolve_nfc_path(std::string_view abs_nfc) {
    const fs::path p = utf8_to_path(abs_nfc);
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
    fs::path cur = p.root_path();
    for (const auto& part : p.relative_path()) {
        auto next = find_entry_nfc(cur, to_nfc(path_to_utf8(part)));
        if (!next) return std::nullopt;
        cur = *next;
    }
    return cur;
}

std::string normalized_path_string(const fs::path& p) {
    const auto g = fs::absolute(p).lexically_normal().generic_u8string();
    std::string s(reinterpret_cast<const char*>(g.data()), g.size());
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return to_nfc(s);
}

std::vector<bool> directories_reachable(const std::vector<std::string>& paths, std::chrono::milliseconds timeout) {
    // 応答しない共有に当たった確認スレッドは、戻ってくるまで残る（結果は共有の状態に書くので、呼び出し側が先に戻ってよい）
    std::vector<std::future<bool>> futures;
    for (const auto& p : paths) {
        auto task = std::make_shared<std::promise<bool>>();
        futures.push_back(task->get_future());
        std::thread([task, p] {
            std::error_code ec;
            const auto disk = resolve_nfc_path(p);
            task->set_value(disk && std::filesystem::is_directory(*disk, ec));
        }).detach();
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<bool> result;
    for (auto& f : futures)
        result.push_back(f.wait_until(deadline) == std::future_status::ready && f.get());
    return result;
}

} // namespace focal
