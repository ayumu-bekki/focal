#pragma once

#include <stdexcept>
#include <string>

namespace focal {

// core 内部のエラーは例外で伝える。C API の境界でステータスコードに変換する（ADR-13）。
class Error : public std::runtime_error {
public:
    enum class Code { InvalidArgument, Io, Unsupported, Decode, Internal, Cancelled, Database, NotFound };

    Error(Code code, const std::string& message) : std::runtime_error(message), code_(code) {}

    Code code() const noexcept { return code_; }

private:
    Code code_;
};

} // namespace focal
