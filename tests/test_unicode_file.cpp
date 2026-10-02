#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "test_util.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/unicode.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

// 「がっこう」の NFC と NFD（が = か + 濁点）
const std::string kNfc = "\xe3\x81\x8c\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86";
const std::string kNfd = "\xe3\x81\x8b\xe3\x82\x99\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86";

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

} // namespace

TEST_CASE("NFC 正規化: NFD の濁点付き文字を合成する", "[unicode]") {
    CHECK(kNfc != kNfd);
    CHECK(to_nfc(kNfd) == kNfc);
    CHECK(to_nfc(kNfc) == kNfc);
    CHECK(to_nfc("ascii/IMG_0001.CR3") == "ascii/IMG_0001.CR3");
    // 不正な UTF-8 はそのまま返す
    const std::string bad = "a\xff\xfe" "b";
    CHECK(to_nfc(bad) == bad);
}

TEST_CASE("大文字小文字を無視した照合キー", "[unicode]") {
    CHECK(casefold_key("IMG_0001.CR3") == casefold_key("img_0001.cr3"));
    CHECK(casefold_key(kNfd + ".ARW") == casefold_key(kNfc + ".arw"));
    // 全角英字も畳み込む
    CHECK(casefold_key("\xef\xbc\xa1") == casefold_key("\xef\xbd\x81"));  // Ａ / ａ
    CHECK(casefold_key("IMG_0001.CR3") != casefold_key("IMG_0002.CR3"));
}

TEST_CASE("NFD の名前のファイルを NFC の名前で見つける", "[unicode][file]") {
    TempDir dir;
    write_file(dir.path() / utf8_to_path(kNfd + ".txt"), "x");
    const auto found = find_entry_nfc(dir.path(), kNfc + ".txt");
    REQUIRE(found);
    CHECK(fs::exists(*found));
    CHECK_FALSE(find_entry_nfc(dir.path(), "nothing.txt"));

    // 途中のフォルダ名も NFD の場合
    write_file(dir.path() / utf8_to_path(kNfd) / "a.txt", "y");
    const auto resolved = resolve_nfc_path(normalized_path_string(dir.path()) + "/" + kNfc + "/a.txt");
    REQUIRE(resolved);
    CHECK(fs::exists(*resolved));
}

TEST_CASE("カタログに保存するパスの形", "[file]") {
    TempDir dir;
    fs::create_directories(dir / "a/b");
    const std::string s = normalized_path_string(dir.path() / "a" / ".." / "a" / "b" / "");
    CHECK(s.back() == 'b');
    CHECK(s.find("..") == std::string::npos);
    CHECK(s.find('\\') == std::string::npos);
}

TEST_CASE("ファイル情報", "[file]") {
    TempDir dir;
    write_file(dir / "f.bin", "12345");
    const auto st = stat_file(dir / "f.bin");
    REQUIRE(st);
    CHECK(st->size == 5);
    CHECK(st->mtime > 1600000000);
    CHECK_FALSE(stat_file(dir / "none"));
    CHECK_FALSE(stat_file(dir.path()));  // ディレクトリは通常ファイルではない
}

TEST_CASE("quick_hash: サイズ + 先頭 1MB + 末尾 1MB", "[hash]") {
    TempDir dir;
    const size_t mb = 1 << 20;
    std::string big(3 * mb, 'a');
    write_file(dir / "big.bin", big);
    const std::string h = quick_hash(dir / "big.bin");
    CHECK(h.size() == 64);
    CHECK(quick_hash(dir / "big.bin") == h);

    // 中央（先頭 1MB と末尾 1MB の外）の変更は検出しない（仕様）
    std::string mid = big;
    mid[mb + 10] = 'b';
    write_file(dir / "big.bin", mid);
    CHECK(quick_hash(dir / "big.bin") == h);

    // 末尾の変更は検出する
    std::string tail = big;
    tail[3 * mb - 1] = 'b';
    write_file(dir / "big.bin", tail);
    CHECK(quick_hash(dir / "big.bin") != h);

    // サイズの違いも検出する
    write_file(dir / "big.bin", big + "a");
    CHECK(quick_hash(dir / "big.bin") != h);

    // 2MB 以下は全体
    write_file(dir / "small.bin", "hello");
    write_file(dir / "small2.bin", "hellp");
    CHECK(quick_hash(dir / "small.bin") != quick_hash(dir / "small2.bin"));
    CHECK(blake3_hex("") == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}
