#include "catalog/photo_delete.h"

#include <system_error>

#include "catalog/catalog.h"
#include "import/card_import.h"
#include "util/error.h"
#include "util/file.h"
#include "util/unicode.h"
#include "util/volume.h"

namespace focal {

namespace fs = std::filesystem;

namespace {

bool on_network_volume(const fs::path& p) {
    const auto v = volume_for_path(p);
    return v && v->id.rfind("net:", 0) == 0;
}

} // namespace

DeletePlan plan_delete(Catalog& catalog, std::span<const int64_t> photo_ids) {
    DeletePlan plan;
    for (int64_t id : photo_ids) {
        DeleteItem item;
        item.photo_id = id;
        const auto raw = catalog.photo_disk_path(id);
        if (!raw) {
            if (!catalog.photo(id)) continue;  // カタログにもない
            ++plan.missing_photos;
            plan.items.push_back(std::move(item));
            continue;
        }
        std::error_code ec;
        if (!fs::exists(*raw, ec)) {
            ++plan.missing_photos;
            plan.items.push_back(std::move(item));
            continue;
        }
        item.raw = *raw;
        item.network = on_network_volume(*raw);
        // 同じフォルダで、名前の幹が同じ JPEG・動画・サイドカー
        const std::string stem = casefold_key(path_to_utf8(raw->stem()));
        for (fs::directory_iterator it(raw->parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string name = to_nfc(path_to_utf8(it->path().filename()));
            if (it->path() == *raw || name.empty() || name[0] == '.') continue;
            if (!is_companion_file_name(name)) continue;
            if (casefold_key(path_to_utf8(it->path().stem())) == stem) item.companions.push_back(it->path());
        }
        plan.files += 1 + static_cast<int>(item.companions.size());
        if (item.network) ++plan.network_photos;
        plan.items.push_back(std::move(item));
    }
    return plan;
}

DeleteResult delete_photos(Catalog& catalog, const DeletePlan& plan, const TrashFn& trash) {
    DeleteResult result;
    std::vector<int64_t> to_forget;  // カタログから消す写真

    auto remove_file = [&](const fs::path& p, bool network, std::string& error) {
        if (network) {
            std::error_code ec;
            fs::remove(p, ec);
            if (ec) {
                error = path_to_utf8(p) + ": " + ec.message();
                return false;
            }
            ++result.files_removed;
            return true;
        }
        if (!trash) {
            error = path_to_utf8(p) + ": no way to move it to the Trash";
            return false;
        }
        if (!trash(p)) {
            error = path_to_utf8(p) + ": could not move it to the Trash";
            return false;
        }
        ++result.files_trashed;
        return true;
    };

    for (const auto& item : plan.items) {
        std::string error;
        if (!item.raw.empty() && !remove_file(item.raw, item.network, error)) {
            ++result.photos_failed;
            ++result.files_failed;
            result.errors.push_back(error);
            continue;  // RAW が残っているので、カタログにも残す
        }
        for (const auto& c : item.companions) {
            if (!remove_file(c, item.network, error)) {
                ++result.files_failed;
                result.errors.push_back(error);
            }
        }
        to_forget.push_back(item.photo_id);
        ++result.photos_deleted;
    }
    if (!to_forget.empty()) catalog.remove_photos(to_forget);
    return result;
}

} // namespace focal
