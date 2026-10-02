#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace focal::cli {

// "--key value" と "--flag" と位置引数だけの簡単な引数解析。
class Args {
public:
    // flags: 値を取らないオプション名（"--" なし）
    Args(int argc, char** argv, std::vector<std::string> flags = {});

    const std::vector<std::string>& positional() const { return positional_; }
    bool has(const std::string& key) const { return values_.count(key) > 0; }
    std::optional<std::string> get(const std::string& key) const;
    double get_double(const std::string& key, double def) const;
    int get_int(const std::string& key, int def) const;

private:
    std::vector<std::string> positional_;
    std::map<std::string, std::string> values_;
};

} // namespace focal::cli
