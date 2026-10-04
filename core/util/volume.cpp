#include "util/volume.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "util/file.h"
#include "util/unicode.h"

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <cstring>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#else
#include <sys/stat.h>

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#endif

namespace focal {

namespace fs = std::filesystem;

std::string network_volume_id(const std::string& source) {
    std::string s = source;
    if (s.rfind("//", 0) == 0) {  // SMB・AFP: //[user[:pass]@]host/share
        s = s.substr(2);
        if (const auto at = s.find('@'); at != std::string::npos && at < s.find('/')) s = s.substr(at + 1);
    } else if (s.find(':') != std::string::npos && s.rfind("/dev/", 0) != 0) {  // NFS: host:/path
        if (s.size() > 1 && s[1] == ':' && std::isalpha(static_cast<unsigned char>(s[0]))) return {};  // C:\ のようなドライブ
    } else {
        return {};
    }
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s.empty() ? std::string() : "net:" + s;
}

namespace {

bool path_is_inside(const fs::path& mount, const fs::path& path) {
    const auto rel = path.lexically_relative(mount);
    if (rel.empty()) return false;
    return *rel.begin() != "..";
}

#if defined(__APPLE__)

// macOS はユーザーのファイルを Data ボリュームに置き、"/" から firmlink で見せている。
// 利用者から見えるマウントポイントは "/"（ID は Data ボリュームのもの）として扱う。
constexpr const char* kDataVolume = "/System/Volumes/Data";

struct VolumeAttrs {
    std::string name;
    std::string uuid;
};

VolumeAttrs volume_attrs(const char* mount) {
    VolumeAttrs a;
    struct attrlist al {};
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME | ATTR_VOL_UUID;
    alignas(4) char buf[2048];
    if (getattrlist(mount, &al, buf, sizeof buf, 0) != 0) return a;
    // 並びは length(4) + name(attrreference_t) + uuid(16)
    const char* p = buf + sizeof(uint32_t);
    attrreference_t ref;
    std::memcpy(&ref, p, sizeof ref);
    const char* name = p + ref.attr_dataoffset;
    if (name >= buf && name < buf + sizeof buf) a.name.assign(name, strnlen(name, static_cast<size_t>(buf + sizeof buf - name)));
    uuid_t u;
    std::memcpy(u, p + sizeof(attrreference_t), sizeof u);
    char text[40];
    uuid_unparse_upper(u, text);
    a.uuid = text;
    return a;
}

std::vector<VolumeInfo> list_volumes() {
    std::vector<VolumeInfo> out;
    struct statfs* mnts = nullptr;
    const int n = getmntinfo(&mnts, MNT_NOWAIT);
    std::string root_name;
    for (int i = 0; i < n; ++i)
        if (std::string(mnts[i].f_mntonname) == "/") root_name = volume_attrs("/").name;
    for (int i = 0; i < n; ++i) {
        const std::string mount = mnts[i].f_mntonname;
        const bool is_data = mount == kDataVolume;
        if (mount == "/") continue;  // 起動ボリューム（システム）は Data ボリュームの方を "/" として出す
        if ((mnts[i].f_flags & MNT_DONTBROWSE) && !is_data) continue;
        VolumeInfo v;
        const VolumeAttrs a = volume_attrs(mount.c_str());
        v.id = a.uuid.empty() ? network_volume_id(mnts[i].f_mntfromname) : a.uuid;  // SMB・NFS は UUID がない
        v.mount_point = is_data ? fs::path("/") : fs::path(mount);
        v.name = is_data && !root_name.empty() ? root_name : (a.name.empty() ? v.mount_point.filename().string() : a.name);
        v.removable = false;
        v.fs_type = mnts[i].f_fstypename;
        out.push_back(std::move(v));
    }
    return out;
}

std::optional<fs::path> mount_of(const fs::path& path) {
    struct statfs sf;
    if (statfs(path.c_str(), &sf) != 0) return std::nullopt;
    const std::string mount = sf.f_mntonname;
    if (mount == "/" || mount == kDataVolume) return fs::path("/");
    return fs::path(mount);
}

#elif defined(_WIN32)

std::wstring volume_guid(const std::wstring& root) {
    wchar_t buf[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), buf, MAX_PATH)) return {};
    return buf;
}

VolumeInfo make_volume(const std::wstring& root) {
    VolumeInfo v;
    v.mount_point = fs::path(root);
    const std::wstring guid = volume_guid(root);
    // \\?\Volume{xxxxxxxx-...}\ から波括弧の中身を取る
    const auto b = guid.find(L'{'), e = guid.find(L'}');
    if (b != std::wstring::npos && e != std::wstring::npos && e > b)
        v.id = path_to_utf8(fs::path(guid.substr(b + 1, e - b - 1)));
    wchar_t label[MAX_PATH + 1] = {};
    wchar_t fs[MAX_PATH + 1] = {};
    GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH);
    v.fs_type = path_to_utf8(fs::path(std::wstring(fs)));
    v.name = path_to_utf8(fs::path(std::wstring(label)));
    if (v.name.empty()) v.name = path_to_utf8(fs::path(root));
    v.removable = GetDriveTypeW(root.c_str()) == DRIVE_REMOVABLE;
    return v;
}

std::vector<VolumeInfo> list_volumes() {
    std::vector<VolumeInfo> out;
    const DWORD len = GetLogicalDriveStringsW(0, nullptr);
    if (len == 0) return out;
    std::wstring buf(len, L'\0');
    GetLogicalDriveStringsW(len, buf.data());
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
        const UINT type = GetDriveTypeW(p);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN || type == DRIVE_CDROM) continue;
        out.push_back(make_volume(p));
    }
    return out;
}

std::optional<fs::path> mount_of(const fs::path& path) {
    wchar_t buf[MAX_PATH];
    const std::wstring abs = fs::absolute(path).wstring();
    if (!GetVolumePathNameW(abs.c_str(), buf, MAX_PATH)) return std::nullopt;
    return fs::path(std::wstring(buf));
}

#else  // Linux ほか

std::string unescape_mountinfo(const std::string& s) {  // 空白などが \040 のように 8 進で入っている
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            r += static_cast<char>(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else {
            r += s[i];
        }
    }
    return r;
}

std::string unescape_label(const std::string& s) {  // /dev/disk/by-label のリンク名は \x20 のような形
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            r += static_cast<char>(std::stoi(s.substr(i + 2, 2), nullptr, 16));
            i += 3;
        } else {
            r += s[i];
        }
    }
    return r;
}

// /dev/disk/by-<kind> のリンク名 → 実体のデバイスパス
std::map<std::string, std::string> disk_links(const char* kind) {
    std::map<std::string, std::string> m;  // デバイス → リンク名
    std::error_code ec;
    for (fs::directory_iterator it(std::string("/dev/disk/") + kind, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        const auto target = fs::canonical(it->path(), ec2);
        if (!ec2) m[target.string()] = it->path().filename().string();
    }
    return m;
}

std::vector<VolumeInfo> list_volumes() {
    std::vector<VolumeInfo> out;
    std::ifstream in("/proc/self/mountinfo");
    const auto uuids = disk_links("by-uuid");
    const auto labels = disk_links("by-label");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::vector<std::string> f;
        std::string tok;
        while (ss >> tok) f.push_back(tok);
        const auto dash = std::find(f.begin(), f.end(), "-");
        if (f.size() < 7 || dash == f.end() || dash + 2 >= f.end()) continue;
        const std::string mount = unescape_mountinfo(f[4]);
        const std::string fstype = *(dash + 1);
        const std::string source = *(dash + 2);
        const std::string net = network_volume_id(source);
        if (!net.empty()) {  // ネットワークの共有（SMB・NFS）
            VolumeInfo v;
            v.id = net;
            v.fs_type = fstype;
            v.mount_point = fs::path(mount);
            v.name = mount == "/" ? "/" : v.mount_point.filename().string();
            out.push_back(std::move(v));
            continue;
        }
        if (source.rfind("/dev/", 0) != 0) continue;  // 実デバイスのボリュームだけ
        if (mount.rfind("/boot", 0) == 0 || mount.rfind("/snap", 0) == 0) continue;
        VolumeInfo v;
        std::error_code ec;
        const auto dev = fs::canonical(source, ec).string();
        if (auto u = uuids.find(dev); u != uuids.end()) v.id = u->second;
        if (auto l = labels.find(dev); l != labels.end()) v.name = unescape_label(l->second);
        v.mount_point = fs::path(mount);
        v.fs_type = fstype;
        if (v.name.empty()) v.name = mount == "/" ? "/" : v.mount_point.filename().string();
        out.push_back(std::move(v));
    }
    return out;
}

std::optional<fs::path> mount_of(const fs::path& path) {
    // 一番長く一致するマウントポイント
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(path, ec);
    if (ec) return std::nullopt;
    std::optional<fs::path> best;
    for (const auto& v : list_volumes())
        if (path_is_inside(v.mount_point, p) && (!best || v.mount_point.native().size() > best->native().size()))
            best = v.mount_point;
    return best;
}

#endif

} // namespace

std::vector<VolumeInfo> mounted_volumes() {
    auto v = list_volumes();
    std::sort(v.begin(), v.end(), [](const VolumeInfo& a, const VolumeInfo& b) {
        return a.mount_point.native() < b.mount_point.native();
    });
    return v;
}

VolumeKind volume_kind(const VolumeInfo& v) {
    if (v.id.rfind("net:", 0) == 0) return VolumeKind::Network;
    if (v.mount_point == fs::path("/")) return VolumeKind::Internal;
    if (v.removable) return VolumeKind::External;
#if defined(__APPLE__)
    return VolumeKind::External;  // macOS の起動ボリューム以外（/Volumes の下）
#else
    return VolumeKind::Unknown;
#endif
}

std::optional<VolumeInfo> volume_for_path(const fs::path& path) {
    const auto mount = mount_of(path);
    if (!mount) return std::nullopt;
    for (auto& v : list_volumes())
        if (v.mount_point == *mount) return v;
    return std::nullopt;
}

std::optional<std::string> volume_relative_path(const VolumeInfo& volume, const fs::path& path) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(path, ec);
    if (ec) return std::nullopt;
    fs::path mount = volume.mount_point;
    if (path_is_inside(mount, p) || p == mount) {
        const auto rel = p.lexically_relative(mount).generic_u8string();
        std::string s(reinterpret_cast<const char*>(rel.data()), rel.size());
        if (s == ".") s.clear();
        while (!s.empty() && s.back() == '/') s.pop_back();
        return to_nfc(s);
    }
    return std::nullopt;
}

} // namespace focal
