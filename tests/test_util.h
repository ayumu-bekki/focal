#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>

// テスト用の一時ディレクトリ（破棄時に削除）
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "focal") {
        static std::atomic<int> counter{0};
        static const unsigned salt = std::random_device{}();
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "-" + std::to_string(salt) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& rel) const { return path_ / std::filesystem::path(rel); }

private:
    std::filesystem::path path_;
};
