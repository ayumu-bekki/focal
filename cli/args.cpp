#include "args.h"

#include <algorithm>
#include <stdexcept>

namespace focal::cli {

Args::Args(int argc, char** argv, std::vector<std::string> flags) {
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            const std::string key = a.substr(2);
            if (std::find(flags.begin(), flags.end(), key) != flags.end()) {
                values_[key] = "1";
            } else {
                if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
                values_[key] = argv[++i];
            }
        } else {
            positional_.push_back(std::move(a));
        }
    }
}

std::optional<std::string> Args::get(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

double Args::get_double(const std::string& key, double def) const {
    auto v = get(key);
    return v ? std::stod(*v) : def;
}

int Args::get_int(const std::string& key, int def) const {
    auto v = get(key);
    return v ? std::stoi(*v) : def;
}

} // namespace focal::cli
