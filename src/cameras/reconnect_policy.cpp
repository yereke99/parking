#include "anpr/cameras/reconnect_policy.hpp"

#include <algorithm>

namespace anpr::cameras {

FailureClass classifyFailure(CameraError error) {
    switch (error) {
        case CameraError::kRtspAuthFailed:
        case CameraError::kRtspCredentialsMissing:
            return FailureClass::kAuthentication;
        case CameraError::kRtspStreamPathInvalid:
        case CameraError::kUnsupportedCodec:
        case CameraError::kCameraNotActivated:
        case CameraError::kCameraOnOtherSubnet:
        case CameraError::kDuplicateIpDetected:
            return FailureClass::kConfiguration;
        default:
            return FailureClass::kTransient;
    }
}

ReconnectPolicy::ReconnectPolicy(ReconnectSettings settings)
    : settings_(settings), next_transient_ms_(0) {
    // A zero or negative backoff would turn a dead camera into a busy loop; a cap below the
    // initial delay is read as "always wait the cap".
    settings_.initial_backoff_ms = std::max<std::int64_t>(1, settings_.initial_backoff_ms);
    settings_.max_backoff_ms = std::max(settings_.initial_backoff_ms, settings_.max_backoff_ms);
    settings_.auth_retry_interval_ms = std::max<std::int64_t>(1, settings_.auth_retry_interval_ms);
    settings_.auth_max_retries = std::max(0, settings_.auth_max_retries);
    settings_.configuration_retry_interval_ms =
        std::max<std::int64_t>(1, settings_.configuration_retry_interval_ms);
    next_transient_ms_ = settings_.initial_backoff_ms;
}

std::optional<std::int64_t> ReconnectPolicy::onFailure(CameraError error) {
    ++consecutive_failures_;
    last_error_ = error;
    if (exhausted_) {
        return std::nullopt;
    }
    switch (classifyFailure(error)) {
        case FailureClass::kAuthentication:
            // Counted across interleaved transient failures: the camera's lockout counter does
            // not reset when the link blips, so neither does ours.
            ++auth_failures_;
            if (auth_failures_ > settings_.auth_max_retries) {
                exhausted_ = true;
                return std::nullopt;
            }
            return settings_.auth_retry_interval_ms;
        case FailureClass::kConfiguration:
            return settings_.configuration_retry_interval_ms;
        case FailureClass::kTransient:
            break;
    }
    const std::int64_t delay = next_transient_ms_;
    next_transient_ms_ = delay > settings_.max_backoff_ms / 2 ? settings_.max_backoff_ms
                                                              : delay * 2;
    return delay;
}

void ReconnectPolicy::onSuccess() {
    next_transient_ms_ = settings_.initial_backoff_ms;
    consecutive_failures_ = 0;
    auth_failures_ = 0;
    exhausted_ = false;
    last_error_ = CameraError::kNone;
}

}  // namespace anpr::cameras
