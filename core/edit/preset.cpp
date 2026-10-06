#include "edit/preset.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "util/error.h"
#include "util/file.h"
#include "util/unicode.h"

namespace focal {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kExt = ".focalpreset";
constexpr size_t kMaxNameBytes = 120;

struct Loaded {
    std::string name;
    Settings settings;
};

std::optional<Loaded> read_preset_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        const json doc = json::parse(ss.str());
        if (!doc.is_object() || !doc.contains("focalPreset")) return std::nullopt;
        const auto name_it = doc.find("name");
        if (name_it == doc.end() || !name_it->is_string()) return std::nullopt;
        std::string name = to_nfc(name_it->get<std::string>());
        if (name.empty()) return std::nullopt;
        const auto settings_it = doc.find("settings");
        const std::string settings_text = settings_it == doc.end() ? "{}" : settings_it->dump();
        return Loaded{std::move(name), preset_adjustments(settings_from_json(settings_text))};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ファイル名に使えない文字を '_' にし、長すぎれば UTF-8 の文字の境目で切る
std::string file_stem_for(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        const bool bad = c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                         c == '<' || c == '>' || c == '|';
        out.push_back(bad ? '_' : static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    size_t start = out.find_first_not_of(' ');
    out = start == std::string::npos ? std::string() : out.substr(start);
    if (out.size() > kMaxNameBytes) {
        size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    if (out.empty() || out[0] == '.') out.insert(0, "preset");
    return out;
}

struct Entry {
    std::string stem;
    Loaded loaded;
};

std::vector<Entry> read_dir(const fs::path& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != kExt) continue;
        auto loaded = read_preset_file(e.path());
        if (!loaded) continue;
        out.push_back({path_to_utf8(e.path().stem()), std::move(*loaded)});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        const auto ka = casefold_key(a.loaded.name), kb = casefold_key(b.loaded.name);
        return ka != kb ? ka < kb : a.stem < b.stem;
    });
    return out;
}

} // namespace

Settings preset_adjustments(const Settings& settings) {
    Settings s = settings;
    s.geometry = GeometrySettings{};
    s.lens = LensSettings{};  // レンズ補正は写真のレンズに結びつくので、プリセットには入れない（v3.27）
    s.process_version = Settings::kLatestProcessVersion;
    s.preserved_json.clear();
    return s;
}

Settings apply_preset(const Settings& base, const Settings& preset) {
    Settings s = preset;
    s.geometry = base.geometry;
    s.lens = base.lens;
    s.process_version = base.process_version;
    s.preserved_json = base.preserved_json;
    return s;
}

bool adjustments_equal(const Settings& a, const Settings& b) {
    const bool wb_same = a.wb.mode == b.wb.mode &&
                         (a.wb.mode == WhiteBalanceSettings::Mode::AsShot ||
                          (a.wb.temperature == b.wb.temperature && a.wb.tint == b.wb.tint));
    return wb_same && a.exposure == b.exposure && a.contrast == b.contrast && a.highlights == b.highlights &&
           a.shadows == b.shadows && a.whites == b.whites && a.blacks == b.blacks && a.brightness == b.brightness &&
           a.saturation == b.saturation && a.vibrance == b.vibrance && a.clarity == b.clarity &&
           a.sharpness == b.sharpness && a.noise_reduction == b.noise_reduction &&
           a.color_noise_reduction == b.color_noise_reduction;
}

PresetStore::PresetStore(fs::path user_dir, std::optional<fs::path> builtin_dir)
    : user_dir_(std::move(user_dir)), builtin_dir_(std::move(builtin_dir)) {}

std::vector<PresetStore::Cached> PresetStore::read_all() const {
    std::vector<Cached> out;
    if (builtin_dir_)
        for (auto& e : read_dir(*builtin_dir_))
            out.push_back({{"builtin:" + e.stem, e.loaded.name, true}, std::move(e.loaded.settings)});
    for (auto& e : read_dir(user_dir_))
        out.push_back({{"user:" + e.stem, e.loaded.name, false}, std::move(e.loaded.settings)});
    return out;
}

std::vector<PresetInfo> PresetStore::list() const {
    std::lock_guard lock(mutex_);
    cache_ = read_all();
    std::vector<PresetInfo> out;
    for (const auto& c : *cache_) out.push_back(c.info);
    return out;
}

std::optional<std::string> PresetStore::find_match(const Settings& settings) const {
    std::lock_guard lock(mutex_);
    if (!cache_) cache_ = read_all();
    for (const auto& c : *cache_)
        if (adjustments_equal(settings, c.settings)) return c.info.id;
    return std::nullopt;
}

Settings PresetStore::load(const std::string& id) const {
    const bool builtin = id.rfind("builtin:", 0) == 0;
    const bool user = id.rfind("user:", 0) == 0;
    if (!builtin && !user) throw Error(Error::Code::InvalidArgument, "invalid preset id: " + id);
    const std::string stem = id.substr(builtin ? 8 : 5);
    if (stem.empty() || stem.find_first_of("/\\") != std::string::npos || stem.find("..") != std::string::npos)
        throw Error(Error::Code::InvalidArgument, "invalid preset id: " + id);
    const fs::path dir = builtin ? builtin_dir_.value_or(fs::path()) : user_dir_;
    if (dir.empty()) throw Error(Error::Code::NotFound, "preset not found: " + id);
    auto loaded = read_preset_file(dir / utf8_to_path(stem + kExt));
    if (!loaded) throw Error(Error::Code::NotFound, "preset not found: " + id);
    return std::move(loaded->settings);
}

std::string PresetStore::save(const std::string& name_in, const Settings& settings) {
    const std::string name = to_nfc(name_in);
    const size_t first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw Error(Error::Code::InvalidArgument, "preset name is empty");
    const size_t last = name.find_last_not_of(" \t\r\n");
    const std::string trimmed = name.substr(first, last - first + 1);
    if (trimmed.size() > 200) throw Error(Error::Code::InvalidArgument, "preset name is too long");

    std::error_code ec;
    fs::create_directories(user_dir_, ec);
    if (ec) throw Error(Error::Code::Io, "cannot create " + path_to_utf8(user_dir_) + ": " + ec.message());

    // 同じ名前があればそのファイルを上書きする。なければ、使われていないファイル名を探す
    const auto existing = read_dir(user_dir_);
    std::string stem;
    const std::string key = casefold_key(trimmed);
    for (const auto& e : existing)
        if (casefold_key(e.loaded.name) == key) stem = e.stem;
    if (stem.empty()) {
        const std::string base = file_stem_for(trimmed);
        stem = base;
        for (int n = 2;; ++n) {
            const bool taken = std::any_of(existing.begin(), existing.end(), [&](const Entry& e) {
                return casefold_key(e.stem) == casefold_key(stem);
            });
            if (!taken && !fs::exists(user_dir_ / utf8_to_path(stem + kExt), ec)) break;
            stem = base + " " + std::to_string(n);
        }
    }

    json doc = json::object();
    doc["focalPreset"] = 1;
    doc["name"] = trimmed;
    doc["settings"] = json::parse(settings_to_json(preset_adjustments(settings)));

    const fs::path path = user_dir_ / utf8_to_path(stem + kExt);
    const fs::path tmp = user_dir_ / utf8_to_path(stem + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error(Error::Code::Io, "cannot write " + path_to_utf8(tmp));
        out << doc.dump(2) << '\n';
        if (!out) throw Error(Error::Code::Io, "cannot write " + path_to_utf8(tmp));
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw Error(Error::Code::Io, "cannot save " + path_to_utf8(path));
    }
    {
        std::lock_guard lock(mutex_);
        cache_.reset();
    }
    return "user:" + stem;
}

void PresetStore::remove(const std::string& id) {
    if (id.rfind("user:", 0) != 0) throw Error(Error::Code::InvalidArgument, "only your own presets can be deleted");
    const std::string stem = id.substr(5);
    if (stem.empty() || stem.find_first_of("/\\") != std::string::npos || stem.find("..") != std::string::npos)
        throw Error(Error::Code::InvalidArgument, "invalid preset id: " + id);
    std::error_code ec;
    const bool removed = fs::remove(user_dir_ / utf8_to_path(stem + kExt), ec);
    {
        std::lock_guard lock(mutex_);
        cache_.reset();
    }
    if (!removed) throw Error(Error::Code::NotFound, "preset not found: " + id);
}

void PresetStore::rename(const std::string& id, const std::string& new_name) {
    if (id.rfind("user:", 0) != 0) throw Error(Error::Code::InvalidArgument, "only your own presets can be renamed");
    const std::string stem = id.substr(5);
    if (stem.empty() || stem.find_first_of("/\\") != std::string::npos || stem.find("..") != std::string::npos)
        throw Error(Error::Code::InvalidArgument, "invalid preset id: " + id);
    const std::string name = to_nfc(new_name);
    const size_t first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw Error(Error::Code::InvalidArgument, "preset name is empty");
    const std::string trimmed = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    if (trimmed.size() > 200) throw Error(Error::Code::InvalidArgument, "preset name is too long");

    const fs::path path = user_dir_ / utf8_to_path(stem + kExt);
    const auto current = read_preset_file(path);
    if (!current) throw Error(Error::Code::NotFound, "preset not found: " + id);
    const std::string key = casefold_key(trimmed);
    for (const auto& e : read_dir(user_dir_))
        if (e.stem != stem && casefold_key(e.loaded.name) == key)
            throw Error(Error::Code::InvalidArgument, "a preset with this name already exists");

    json doc = json::object();
    doc["focalPreset"] = 1;
    doc["name"] = trimmed;
    doc["settings"] = json::parse(settings_to_json(preset_adjustments(current->settings)));
    const fs::path tmp = user_dir_ / utf8_to_path(stem + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error(Error::Code::Io, "cannot write " + path_to_utf8(tmp));
        out << doc.dump(2) << '\n';
        if (!out) throw Error(Error::Code::Io, "cannot write " + path_to_utf8(tmp));
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw Error(Error::Code::Io, "cannot save " + path_to_utf8(path));
    }
    std::lock_guard lock(mutex_);
    cache_.reset();
}

} // namespace focal
