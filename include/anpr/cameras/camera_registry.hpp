#pragma once

#include <string>
#include <vector>

namespace anpr::cameras {

/// Stable camera ids across reboots and IP changes.
///
/// The first time a device is seen it gets the lowest free id ("camera-01", "camera-02", ...).
/// From then on the same physical camera keeps its id, recognised by the strongest identifier
/// available: MAC address, then serial number, then Hikvision device ID, then ONVIF endpoint
/// UUID. The IP address is only used for an entry that has nothing stronger, and never when the
/// device at that IP reports a different MAC or serial: a camera that took over another camera's
/// address must not inherit its id. Ids of cameras that are offline stay reserved.
///
/// The file holds no secrets (var/cameras/registry.yaml, outside Git) and can be edited to rename
/// cameras: an `id` may be any token of letters, digits, '-' and '_'.
struct RegistryEntry {
    std::string id;
    std::string mac;
    std::string serial;
    std::string device_id;
    std::string onvif_uuid;
    std::string model;
    std::string last_ip;
    std::string first_seen;
    std::string last_seen;
};

struct DeviceIdentity {
    std::string mac;  ///< normalized
    std::string serial;
    std::string device_id;
    std::string onvif_uuid;
    std::string ip;
    std::string model;
};

class CameraRegistry {
public:
    /// Missing file: empty registry, no error. Unreadable or malformed: empty registry and
    /// `error` set (the caller warns and carries on; the file is rewritten on save).
    static CameraRegistry load(const std::string& path, std::string& error);
    static CameraRegistry parse(const std::string& yaml_text, std::string& error);

    /// Atomic write (temporary file + rename), creating parent directories.
    bool save(const std::string& path, std::string& error) const;
    [[nodiscard]] std::string serialize() const;

    /// The stable id for `identity`, adding an entry when the device is new. Updates the entry's
    /// identifiers (filling gaps, never overwriting a different MAC/serial), model, last_ip and
    /// last_seen. `now_iso` is an ISO 8601 UTC timestamp.
    std::string assign(const DeviceIdentity& identity, const std::string& now_iso);

    /// The entry with `id`, or nullptr.
    [[nodiscard]] const RegistryEntry* find(const std::string& id) const;
    [[nodiscard]] const std::vector<RegistryEntry>& entries() const { return entries_; }

    /// Which identifier matched in the last `assign` ("mac", "serial", "device_id", "onvif_uuid",
    /// "ip" or "new").
    [[nodiscard]] const std::string& lastMatchKey() const { return last_match_key_; }

private:
    std::vector<RegistryEntry> entries_;
    std::string last_match_key_;
};

/// "camera-01" style id with the lowest number not in `taken`.
std::string nextCameraId(const std::vector<std::string>& taken);

/// Current UTC time as "2026-10-09T10:15:00Z".
std::string utcNowIso();

}  // namespace anpr::cameras
