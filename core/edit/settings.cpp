#include "edit/settings.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "util/error.h"

namespace focal {

using nlohmann::json;

namespace {

constexpr struct {
    AspectMode mode;
    const char* name;
} kAspects[] = {
    {AspectMode::Free, "free"}, {AspectMode::Original, "original"}, {AspectMode::R1x1, "1:1"},
    {AspectMode::R3x2, "3:2"},  {AspectMode::R4x3, "4:3"},          {AspectMode::R16x9, "16:9"},
    {AspectMode::R5x4, "5:4"},
};

double get_number(const json& obj, const char* key, double def) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return def;
    return it->get<double>();
}

const json& get_object(const json& obj, const char* key) {
    static const json kEmpty = json::object();
    if (!obj.is_object()) return kEmpty;
    auto it = obj.find(key);
    return (it != obj.end() && it->is_object()) ? *it : kEmpty;
}

} // namespace

const char* aspect_to_string(AspectMode a) {
    for (const auto& e : kAspects)
        if (e.mode == a) return e.name;
    return "free";
}

bool aspect_from_string(std::string_view s, AspectMode& out) {
    for (const auto& e : kAspects)
        if (s == e.name) {
            out = e.mode;
            return true;
        }
    return false;
}

void Settings::clamp() {
    wb.temperature = std::clamp(wb.temperature, 2000.0, 50000.0);
    wb.tint = std::clamp(wb.tint, -150.0, 150.0);
    exposure = std::clamp(exposure, -5.0, 5.0);
    contrast = std::clamp(contrast, -100.0, 100.0);
    highlights = std::clamp(highlights, -100.0, 100.0);
    shadows = std::clamp(shadows, -100.0, 100.0);
    for (double* v : {&whites, &blacks, &brightness, &saturation, &vibrance}) *v = std::clamp(*v, -100.0, 100.0);
    clarity = std::clamp(clarity, -200.0, 200.0);
    sharpness = std::clamp(sharpness, 0.0, 150.0);
    noise_reduction = std::clamp(noise_reduction, 0.0, 100.0);
    color_noise_reduction = std::clamp(color_noise_reduction, 0.0, 100.0);
    geometry.rotate90 = ((geometry.rotate90 % 4) + 4) % 4;
    geometry.straighten = std::clamp(geometry.straighten, -45.0, 45.0);

    auto& c = geometry.crop;
    c.x = std::clamp(c.x, 0.0, 1.0);
    c.y = std::clamp(c.y, 0.0, 1.0);
    c.w = std::clamp(c.w, 1e-6, 1.0 - c.x);
    c.h = std::clamp(c.h, 1e-6, 1.0 - c.y);
}

Settings settings_from_json(std::string_view text) {
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::exception& e) {
        throw Error(Error::Code::InvalidArgument, std::string("settings JSON parse error: ") + e.what());
    }
    if (!doc.is_object()) throw Error(Error::Code::InvalidArgument, "settings JSON must be an object");

    Settings s;
    s.preserved_json = std::string(text);
    s.process_version = static_cast<int>(get_number(doc, "processVersion", Settings::kLatestProcessVersion));

    const json& wb = get_object(doc, "wb");
    if (auto it = wb.find("mode"); it != wb.end() && it->is_string() && *it == "custom") {
        s.wb.mode = WhiteBalanceSettings::Mode::Custom;
        s.wb.temperature = get_number(wb, "temperature", s.wb.temperature);
        s.wb.tint = get_number(wb, "tint", s.wb.tint);
    }

    s.exposure = get_number(doc, "exposure", 0.0);
    s.contrast = get_number(doc, "contrast", 0.0);
    s.highlights = get_number(doc, "highlights", 0.0);
    s.shadows = get_number(doc, "shadows", 0.0);
    s.whites = get_number(doc, "whites", 0.0);
    s.blacks = get_number(doc, "blacks", 0.0);
    s.brightness = get_number(doc, "brightness", 0.0);
    s.saturation = get_number(doc, "saturation", 0.0);
    s.vibrance = get_number(doc, "vibrance", 0.0);
    s.clarity = get_number(doc, "clarity", 0.0);
    s.sharpness = get_number(doc, "sharpness", 0.0);
    s.noise_reduction = get_number(doc, "noiseReduction", 0.0);
    s.color_noise_reduction = get_number(doc, "colorNoiseReduction", 0.0);

    const json& geo = get_object(doc, "geometry");
    s.geometry.rotate90 = static_cast<int>(get_number(geo, "rotate90", 0));
    s.geometry.straighten = get_number(geo, "straighten", 0.0);
    const json& crop = get_object(geo, "crop");
    s.geometry.crop = {get_number(crop, "x", 0.0), get_number(crop, "y", 0.0), get_number(crop, "w", 1.0),
                       get_number(crop, "h", 1.0)};
    if (auto it = geo.find("aspect"); it != geo.end() && it->is_string())
        aspect_from_string(it->get<std::string>(), s.geometry.aspect);

    s.clamp();
    return s;
}

std::string settings_to_json(const Settings& s) {
    // 原文を土台にして既知のキーだけ上書きすることで、未知のキーを保持する
    json doc = json::object();
    if (!s.preserved_json.empty()) {
        try {
            json parsed = json::parse(s.preserved_json);
            if (parsed.is_object()) doc = std::move(parsed);
        } catch (const json::exception&) {
        }
    }

    auto ensure_object = [](json& parent, const char* key) -> json& {
        json& child = parent[key];
        if (!child.is_object()) child = json::object();
        return child;
    };

    doc["schema"] = Settings::kSchema;
    doc["processVersion"] = s.process_version;

    json& wb = ensure_object(doc, "wb");
    if (s.wb.mode == WhiteBalanceSettings::Mode::Custom) {
        wb["mode"] = "custom";
        wb["temperature"] = s.wb.temperature;
        wb["tint"] = s.wb.tint;
    } else {
        wb["mode"] = "asShot";
        wb.erase("temperature");
        wb.erase("tint");
    }

    doc["exposure"] = s.exposure;
    doc["contrast"] = s.contrast;
    doc["highlights"] = s.highlights;
    doc["shadows"] = s.shadows;
    // v3.9 で足したキーは、既定値なら書かない（古い版の JSON と同じ形を保つ）
    for (auto [key, v] : {std::pair{"whites", s.whites}, {"blacks", s.blacks}, {"brightness", s.brightness},
                          {"saturation", s.saturation}, {"vibrance", s.vibrance},
                          {"clarity", s.clarity}, {"sharpness", s.sharpness}, {"noiseReduction", s.noise_reduction},
                          {"colorNoiseReduction", s.color_noise_reduction}}) {
        if (v != 0.0)
            doc[key] = v;
        else
            doc.erase(key);
    }

    json& geo = ensure_object(doc, "geometry");
    geo["rotate90"] = s.geometry.rotate90;
    geo["straighten"] = s.geometry.straighten;
    json& crop = ensure_object(geo, "crop");
    crop["x"] = s.geometry.crop.x;
    crop["y"] = s.geometry.crop.y;
    crop["w"] = s.geometry.crop.w;
    crop["h"] = s.geometry.crop.h;
    geo["aspect"] = aspect_to_string(s.geometry.aspect);

    return doc.dump();
}

bool settings_need_no_row(const Settings& s) {
    if (!s.is_default()) return false;
    if (s.preserved_json.empty()) return true;
    // 原文から既知のキーを除いて、何か残っていれば行を残す（未知のキーを保持するため）
    try {
        json doc = json::parse(s.preserved_json);
        if (!doc.is_object()) return true;
        for (const char* k : {"schema", "processVersion", "exposure", "contrast", "highlights", "shadows", "whites",
                              "blacks", "brightness", "saturation", "vibrance", "clarity", "sharpness", "noiseReduction",
                              "colorNoiseReduction"})
            doc.erase(k);
        auto strip = [](json& obj, std::initializer_list<const char*> keys) {
            if (!obj.is_object()) return;
            for (const char* k : keys) obj.erase(k);
        };
        if (doc.contains("wb")) {
            strip(doc["wb"], {"mode", "temperature", "tint"});
            if (doc["wb"].empty()) doc.erase("wb");
        }
        if (doc.contains("geometry")) {
            json& g = doc["geometry"];
            if (g.contains("crop")) {
                strip(g["crop"], {"x", "y", "w", "h"});
                if (g["crop"].empty()) g.erase("crop");
            }
            strip(g, {"rotate90", "straighten", "aspect"});
            if (g.empty()) doc.erase("geometry");
        }
        return doc.empty();
    } catch (const json::exception&) {
        return true;
    }
}

} // namespace focal
