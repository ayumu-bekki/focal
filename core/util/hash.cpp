#include "util/hash.h"

#include <blake3.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "util/error.h"
#include "util/file.h"

namespace focal {

namespace {

constexpr size_t kChunk = 1 << 20;

std::string hex(const uint8_t* bytes, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = digits[bytes[i] >> 4];
        s[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return s;
}

std::string finish(blake3_hasher& h) {
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

} // namespace

std::string blake3_hex(std::string_view data) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data.data(), data.size());
    return finish(h);
}

struct Blake3Stream::Impl {
    blake3_hasher h;
};

Blake3Stream::Blake3Stream() : impl_(new Impl) { blake3_hasher_init(&impl_->h); }
Blake3Stream::~Blake3Stream() { delete impl_; }
void Blake3Stream::update(const void* data, size_t size) { blake3_hasher_update(&impl_->h, data, size); }
std::string Blake3Stream::finish_hex() { return finish(impl_->h); }

std::string blake3_file_hex(const std::filesystem::path& path, const std::atomic<bool>* cancel) {
    struct Closer {
        void operator()(FILE* f) const { std::fclose(f); }
    };
    std::unique_ptr<FILE, Closer> f(open_file(path, "rb"));
    if (!f) throw Error(Error::Code::Io, "cannot open: " + path_to_utf8(path));
    Blake3Stream h;
    std::vector<uint8_t> buf(kChunk);
    for (;;) {
        if (cancel && cancel->load()) throw Error(Error::Code::Cancelled, "cancelled");
        const size_t got = std::fread(buf.data(), 1, buf.size(), f.get());
        if (got > 0) h.update(buf.data(), got);
        if (got < buf.size()) {
            if (std::ferror(f.get())) throw Error(Error::Code::Io, "read error: " + path_to_utf8(path));
            break;
        }
    }
    return h.finish_hex();
}

std::string quick_hash(const std::filesystem::path& path) {
    struct Closer {
        void operator()(FILE* f) const { std::fclose(f); }
    };
    std::unique_ptr<FILE, Closer> f(open_file(path, "rb"));
    if (!f) throw Error(Error::Code::Io, "cannot open: " + path_to_utf8(path));

    std::fseek(f.get(), 0, SEEK_END);
    const auto size = static_cast<uint64_t>(std::ftell(f.get()));
    std::fseek(f.get(), 0, SEEK_SET);

    blake3_hasher h;
    blake3_hasher_init(&h);
    uint8_t le[8];
    for (int i = 0; i < 8; ++i) le[i] = static_cast<uint8_t>(size >> (8 * i));
    blake3_hasher_update(&h, le, sizeof le);

    std::vector<uint8_t> buf(kChunk);
    auto read_into_hash = [&](size_t n) {
        const size_t got = std::fread(buf.data(), 1, n, f.get());
        if (got != n) throw Error(Error::Code::Io, "short read: " + path_to_utf8(path));
        blake3_hasher_update(&h, buf.data(), got);
    };

    if (size <= 2 * kChunk) {
        size_t left = size;
        while (left > 0) {
            const size_t n = std::min(left, kChunk);
            read_into_hash(n);
            left -= n;
        }
    } else {
        read_into_hash(kChunk);
        std::fseek(f.get(), -static_cast<long>(kChunk), SEEK_END);
        read_into_hash(kChunk);
    }
    return finish(h);
}

} // namespace focal
