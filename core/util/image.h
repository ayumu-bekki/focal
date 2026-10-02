#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace focal {

// RGB インターリーブの画像バッファ。
template <class T>
struct Image {
    int width = 0;
    int height = 0;
    std::vector<T> data;

    Image() = default;
    Image(int w, int h) : width(w), height(h), data(static_cast<size_t>(w) * h * 3) {}

    bool empty() const { return width == 0 || height == 0; }
    size_t pixel_count() const { return static_cast<size_t>(width) * height; }
    T* row(int y) { return data.data() + static_cast<size_t>(y) * width * 3; }
    const T* row(int y) const { return data.data() + static_cast<size_t>(y) * width * 3; }
};

using ImageU8 = Image<uint8_t>;
using ImageU16 = Image<uint16_t>;
using ImageF = Image<float>;

} // namespace focal
