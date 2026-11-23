#pragma once
#include <source_location>
#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>
#include <optional>
namespace engine::core {
enum class ErrorCode { InvalidArgument, EmptyInput, NonFinite, NonMonotonicTime, InvalidState, NumericalError, AllocationFailure, ResourceLimit };
// Static messages: constructing/propagating an error does not allocate.
struct Error {
    ErrorCode code;
    std::string_view message;
    std::source_location location;
    template<std::size_t N>
    constexpr Error(ErrorCode c, const char (&m)[N], std::source_location loc = std::source_location::current()) noexcept
        : code(c), message(m, N-1), location(loc) {}
    // Non-owning message: call sites must pass static-lifetime character arrays.
    // C++ cannot enforce static storage duration for an arbitrary char array.
    Error(ErrorCode, std::string_view, std::source_location = std::source_location::current()) = delete;
};
template<class T> class [[nodiscard]] Result {
    std::variant<T, Error> data_;
public:
    Result(T v) : data_(std::in_place_type<T>, std::move(v)) {}
    Result(Error e) noexcept : data_(std::in_place_type<Error>, e) {}
    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }
    T& value() & { return std::get<T>(data_); }
    const T& value() const & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }
    const Error& error() const { return std::get<Error>(data_); }
};
template<> class [[nodiscard]] Result<void> {
    std::optional<Error> error_;
public:
    Result() noexcept = default;
    Result(Error e) noexcept : error_(e) {}
    [[nodiscard]] bool ok() const noexcept { return !error_; }
    explicit operator bool() const noexcept { return ok(); }
    void value() const { if (error_) throw std::bad_variant_access(); }
    const Error& error() const { return error_.value(); }
};
}
