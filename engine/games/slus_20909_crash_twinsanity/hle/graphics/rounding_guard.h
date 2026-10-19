#pragma once

namespace title {

// Source geometry arithmetic rounds toward zero within this scope. Restore
// the caller's mode on every exit, including exceptions and nested scopes.
class RoundingGuard final {
public:
    RoundingGuard() noexcept;
    ~RoundingGuard() noexcept;
    RoundingGuard(const RoundingGuard&) = delete;
    RoundingGuard& operator=(const RoundingGuard&) = delete;
private:
    int previous_;
};

}  // namespace title
