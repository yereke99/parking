#include "anpr/cameras/camera_registry.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include "anpr/common/filesystem.hpp"
#include "anpr/common/yaml.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::cameras {
namespace {

bool isValidId(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
    });
}

/// Double-quoted YAML scalar that the project's YAML reader takes back verbatim after
/// `unescapeScalar`. A quote is written as \x22 rather than \": the reader's comment and colon
/// scanners toggle on every '"', so an escaped quote followed by " #" would cut the line.
/// Control characters (a tab would make the whole file unreadable) become \xNN as well.
std::string quoteScalar(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"' || byte < 0x20 || byte == 0x7F) {
            static const char kHex[] = "0123456789ABCDEF";
            out += "\\x";
            out.push_back(kHex[byte >> 4U]);
            out.push_back(kHex[byte & 0x0FU]);
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/// Inverse of `quoteScalar`, also accepting the common hand-written escapes (\" \n \t \r).
/// An unknown escape is kept literally.
std::string unescapeScalar(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch != '\\' || i + 1 >= value.size()) {
            out.push_back(ch);
            continue;
        }
        const char next = value[i + 1];
        if (next == '\\' || next == '"') {
            out.push_back(next);
            ++i;
        } else if (next == 'n') {
            out.push_back('\n');
            ++i;
        } else if (next == 't') {
            out.push_back('\t');
            ++i;
        } else if (next == 'r') {
            out.push_back('\r');
            ++i;
        } else if (next == 'x' && i + 3 < value.size() && hexDigit(value[i + 2]) >= 0 &&
                   hexDigit(value[i + 3]) >= 0) {
            out.push_back(static_cast<char>(hexDigit(value[i + 2]) * 16 + hexDigit(value[i + 3])));
            i += 3;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string readField(const yaml::Node& entry, const char* key) {
    const auto value = yaml::asString(entry.find(key));
    return value ? unescapeScalar(*value) : std::string{};
}

void appendError(std::string& error, const std::string& message) {
    if (!error.empty()) {
        error += "; ";
    }
    error += message;
}

/// Both identifiers known and different: proof of two different devices.
bool conflicts(const std::string& stored, const std::string& reported) {
    return !stored.empty() && !reported.empty() && stored != reported;
}

/// Unique per process and call: `--camera-scan` may save while a running `--cameras` does.
std::string temporaryPathFor(const std::string& path) {
    static std::atomic<unsigned> counter{0};
    return path + ".tmp." + std::to_string(static_cast<long>(::getpid())) + "." +
           std::to_string(counter.fetch_add(1));
}

/// The preferred candidate among several entries matching at the same strength: the one seen
/// most recently, then the first in the file. ISO 8601 UTC timestamps sort as text.
bool preferCandidate(const RegistryEntry& candidate, const RegistryEntry& current) {
    return candidate.last_seen > current.last_seen;
}

}  // namespace

CameraRegistry CameraRegistry::parse(const std::string& yaml_text, std::string& error) {
    CameraRegistry registry;
    error.clear();

    const yaml::ParseResult parsed = yaml::parse(yaml_text);
    if (!parsed.ok) {
        error = "malformed camera registry: " + parsed.error;
        return registry;
    }
    if (parsed.root.isNull()) {
        return registry;
    }
    if (!parsed.root.isMap()) {
        error = "malformed camera registry: the document root must be a mapping";
        return registry;
    }
    const yaml::Node* cameras = parsed.root.find("cameras");
    if (cameras == nullptr || cameras->isNull()) {
        return registry;
    }
    if (!cameras->isSequence()) {
        error = "malformed camera registry: 'cameras' must be a list";
        return registry;
    }

    std::set<std::string> ids;
    for (std::size_t i = 0; i < cameras->sequence().size(); ++i) {
        const yaml::Node& node = cameras->sequence()[i];
        const std::string where = "cameras[" + std::to_string(i) + "]";
        if (!node.isMap()) {
            appendError(error, where + " is not a mapping; entry ignored");
            continue;
        }
        RegistryEntry entry;
        entry.id = readField(node, "id");
        if (!isValidId(entry.id)) {
            appendError(error, where + ": id '" + entry.id +
                                   "' must be letters, digits, '-' or '_'; entry ignored");
            continue;
        }
        if (!ids.insert(entry.id).second) {
            appendError(error, where + ": id '" + entry.id + "' is used twice; entry ignored");
            continue;
        }
        const std::string mac = readField(node, "mac");
        entry.mac = net::normalizeMac(mac);
        if (!mac.empty() && entry.mac.empty()) {
            appendError(error, where + ": mac '" + mac + "' is not a MAC address; ignored");
        }
        entry.serial = readField(node, "serial");
        entry.device_id = readField(node, "device_id");
        entry.onvif_uuid = readField(node, "onvif_uuid");
        entry.model = readField(node, "model");
        entry.last_ip = readField(node, "last_ip");
        entry.first_seen = readField(node, "first_seen");
        entry.last_seen = readField(node, "last_seen");
        registry.entries_.push_back(std::move(entry));
    }
    return registry;
}

CameraRegistry CameraRegistry::load(const std::string& path, std::string& error) {
    error.clear();
    std::error_code ec;
    const bool present = filesystem::exists(path, ec);
    if (ec) {
        error = "cannot access camera registry " + path + ": " + ec.message();
        return CameraRegistry{};
    }
    if (!present) {
        return CameraRegistry{};
    }
    if (filesystem::is_directory(path, ec)) {
        error = "cannot read camera registry " + path + ": it is a directory";
        return CameraRegistry{};
    }
    std::ifstream input(path);
    if (!input) {
        error = "cannot read camera registry " + path;
        return CameraRegistry{};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        error = "cannot read camera registry " + path;
        return CameraRegistry{};
    }
    CameraRegistry registry = parse(buffer.str(), error);
    if (!error.empty()) {
        error = path + ": " + error;
    }
    return registry;
}

std::string CameraRegistry::serialize() const {
    std::ostringstream out;
    out << "# Camera identities, generated by kz_anpr on every scan. No secrets are stored here.\n"
        << "# Ids may be edited (letters, digits, '-' and '_'): a camera keeps its id, matched by\n"
        << "# MAC, serial, device ID, ONVIF UUID and only then IP, while its entry exists.\n"
        << "version: 1\n";
    if (entries_.empty()) {
        out << "cameras: []\n";
        return out.str();
    }
    out << "cameras:\n";
    for (const RegistryEntry& entry : entries_) {
        out << "  - id: " << quoteScalar(entry.id) << '\n'
            << "    mac: " << quoteScalar(entry.mac) << '\n'
            << "    serial: " << quoteScalar(entry.serial) << '\n'
            << "    device_id: " << quoteScalar(entry.device_id) << '\n'
            << "    onvif_uuid: " << quoteScalar(entry.onvif_uuid) << '\n'
            << "    model: " << quoteScalar(entry.model) << '\n'
            << "    last_ip: " << quoteScalar(entry.last_ip) << '\n'
            << "    first_seen: " << quoteScalar(entry.first_seen) << '\n'
            << "    last_seen: " << quoteScalar(entry.last_seen) << '\n';
    }
    return out.str();
}

bool CameraRegistry::save(const std::string& path, std::string& error) const {
    error.clear();
    const filesystem::path target(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "cannot create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    // Written next to the target so the rename stays on one filesystem and is atomic: a reader
    // sees the old file or the new one, never half of it.
    const std::string temporary = temporaryPathFor(path);
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output) {
            error = "cannot write " + temporary;
            return false;
        }
        output << serialize();
        output.flush();
        if (!output) {
            error = "cannot write " + temporary;
            output.close();
            filesystem::remove(temporary, ec);
            return false;
        }
    }
    filesystem::rename(temporary, target, ec);
    if (ec) {
        error = "cannot replace " + path + ": " + ec.message();
        std::error_code ignored;
        filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::string CameraRegistry::assign(const DeviceIdentity& identity, const std::string& now_iso) {
    DeviceIdentity device = identity;
    device.mac = net::normalizeMac(identity.mac);

    RegistryEntry* match = nullptr;
    auto consider = [&match](RegistryEntry& entry) {
        if (match == nullptr || preferCandidate(entry, *match)) {
            match = &entry;
        }
    };

    if (!device.mac.empty()) {
        for (RegistryEntry& entry : entries_) {
            if (entry.mac == device.mac) {
                consider(entry);
            }
        }
        if (match != nullptr) {
            last_match_key_ = "mac";
        }
    }
    if (match == nullptr && !device.serial.empty()) {
        for (RegistryEntry& entry : entries_) {
            if (entry.serial == device.serial && !conflicts(entry.mac, device.mac)) {
                consider(entry);
            }
        }
        if (match != nullptr) {
            last_match_key_ = "serial";
        }
    }
    if (match == nullptr && !device.device_id.empty()) {
        for (RegistryEntry& entry : entries_) {
            if (entry.device_id == device.device_id && !conflicts(entry.mac, device.mac) &&
                !conflicts(entry.serial, device.serial)) {
                consider(entry);
            }
        }
        if (match != nullptr) {
            last_match_key_ = "device_id";
        }
    }
    if (match == nullptr && !device.onvif_uuid.empty()) {
        for (RegistryEntry& entry : entries_) {
            if (entry.onvif_uuid == device.onvif_uuid && !conflicts(entry.mac, device.mac) &&
                !conflicts(entry.serial, device.serial)) {
                consider(entry);
            }
        }
        if (match != nullptr) {
            last_match_key_ = "onvif_uuid";
        }
    }
    if (match == nullptr && !device.ip.empty()) {
        // Only an entry nothing stronger contradicts: a different MAC, serial, device ID or
        // ONVIF UUID at this address is a different camera that took over the IP. Among the
        // remaining candidates an entry known only by its IP is the closest fit.
        bool match_is_ip_only = false;
        for (RegistryEntry& entry : entries_) {
            if (entry.last_ip != device.ip || conflicts(entry.mac, device.mac) ||
                conflicts(entry.serial, device.serial) ||
                conflicts(entry.device_id, device.device_id) ||
                conflicts(entry.onvif_uuid, device.onvif_uuid)) {
                continue;
            }
            const bool ip_only = entry.mac.empty() && entry.serial.empty() &&
                                 entry.device_id.empty() && entry.onvif_uuid.empty();
            if (match == nullptr || (ip_only && !match_is_ip_only) ||
                (ip_only == match_is_ip_only && preferCandidate(entry, *match))) {
                match = &entry;
                match_is_ip_only = ip_only;
            }
        }
        if (match != nullptr) {
            last_match_key_ = "ip";
        }
    }

    if (match == nullptr) {
        std::vector<std::string> taken;
        taken.reserve(entries_.size());
        for (const RegistryEntry& entry : entries_) {
            taken.push_back(entry.id);
        }
        RegistryEntry entry;
        entry.id = nextCameraId(taken);
        entry.first_seen = now_iso;
        entries_.push_back(std::move(entry));
        match = &entries_.back();
        last_match_key_ = "new";
    }

    // Fill gaps; a stored MAC or serial is never replaced by a different one (the matching
    // above guarantees they agree when both are known). Device ID and ONVIF UUID follow the
    // device, since a firmware reset may regenerate them.
    if (match->mac.empty()) {
        match->mac = device.mac;
    }
    if (match->serial.empty()) {
        match->serial = device.serial;
    }
    if (!device.device_id.empty()) {
        match->device_id = device.device_id;
    }
    if (!device.onvif_uuid.empty()) {
        match->onvif_uuid = device.onvif_uuid;
    }
    if (!device.model.empty()) {
        match->model = device.model;
    }
    if (!device.ip.empty()) {
        match->last_ip = device.ip;
    }
    if (match->first_seen.empty()) {
        match->first_seen = now_iso;
    }
    match->last_seen = now_iso;
    return match->id;
}

const RegistryEntry* CameraRegistry::find(const std::string& id) const {
    for (const RegistryEntry& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

std::string nextCameraId(const std::vector<std::string>& taken) {
    const std::set<std::string> used(taken.begin(), taken.end());
    for (int number = 1;; ++number) {
        char id[32];
        std::snprintf(id, sizeof(id), "camera-%02d", number);
        if (used.count(id) == 0) {
            return id;
        }
    }
}

std::string utcNowIso() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

}  // namespace anpr::cameras
