#pragma once

#include <string>

namespace cad {

enum class ErrorCode {
    Ok = 0,
    // File errors
    FileNotFound,
    FileOpenFailed,
    FileReadError,
    // Parse errors
    InvalidFormat,
    UnexpectedSection,
    UnexpectedToken,
    UnsupportedEntity,
    ParseError,
    // Memory errors
    OutOfMemory,
    // Render errors
    RenderError,
    DeviceInitFailed,
    ShaderCompileFailed,
    // General
    InvalidArgument,
    NotInitialized,
    AlreadyInitialized,
};

struct Result {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    int line_number = 0;

    bool ok() const { return code == ErrorCode::Ok; }
    explicit operator bool() const { return ok(); }

    static Result success() {
        return {ErrorCode::Ok, "", 0};
    }

    static Result error(ErrorCode code, const std::string& msg, int line = 0) {
        return {code, msg, line};
    }
};

template<typename T>
struct ResultOf {
    Result result;
    T value;

    bool ok() const { return result.ok(); }
    explicit operator bool() const { return ok(); }

    static ResultOf<T> success(T val) {
        return {{ErrorCode::Ok, "", 0}, std::move(val)};
    }

    static ResultOf<T> error(ErrorCode code, const std::string& msg, int line = 0) {
        return {{code, msg, line}, T{}};
    }
};

} // namespace cad
