#pragma once

#include <cstdint>
#include <optional>

#include "anpr/cameras/diagnostics.hpp"

namespace anpr::cameras {

/// How a failure should be retried.
enum class FailureClass {
    /// Network blips, camera reboots, PoE link loss, timeouts, EOF, resets: bounded exponential
    /// backoff, forever.
    kTransient,
    /// Wrong or missing credentials: very slow, a few attempts, then stop. Hikvision locks an
    /// address out after a handful of failed logins, and retrying cannot fix a password.
    kAuthentication,
    /// Wrong stream path, unsupported codec, inactive camera: slow retries (someone has to change
    /// the camera or the config), forever.
    kConfiguration,
};

FailureClass classifyFailure(CameraError error);

struct ReconnectSettings {
    std::int64_t initial_backoff_ms{1000};
    std::int64_t max_backoff_ms{30000};
    std::int64_t auth_retry_interval_ms{900000};
    /// Further authentication attempts after the first failure. 0: never retry.
    int auth_max_retries{2};
    std::int64_t configuration_retry_interval_ms{120000};
};

/// Decides the delay before the next connection attempt of one camera. Pure logic, no clock.
class ReconnectPolicy {
public:
    explicit ReconnectPolicy(ReconnectSettings settings);

    /// Records a failed attempt. Returns the delay before the next one, or nullopt when the
    /// camera must not be retried any more (authentication attempts exhausted).
    std::optional<std::int64_t> onFailure(CameraError error);
    /// Records a successful connection: backoff and counters reset.
    void onSuccess();

    [[nodiscard]] bool exhausted() const { return exhausted_; }
    [[nodiscard]] int consecutiveFailures() const { return consecutive_failures_; }
    [[nodiscard]] CameraError lastError() const { return last_error_; }

private:
    ReconnectSettings settings_;
    std::int64_t next_transient_ms_;
    int consecutive_failures_{0};
    int auth_failures_{0};
    bool exhausted_{false};
    CameraError last_error_{CameraError::kNone};
};

}  // namespace anpr::cameras
