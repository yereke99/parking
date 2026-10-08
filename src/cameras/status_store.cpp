#include "anpr/cameras/status_store.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "anpr/common/filesystem.hpp"
#include "anpr/common/json.hpp"

namespace anpr::cameras {
namespace {

/// Bumped only when a field changes meaning; readers ignore fields they do not know.
constexpr int kStatusFormatVersion = 1;

std::string jsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[byte >> 4U]);
                    out.push_back(kHex[byte & 0x0FU]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

/// Three decimals are finer than any rate or latency shown here and keep the file readable.
/// JSON has no NaN or infinity: those become null and read back as the field's default.
std::string jsonNumber(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", value);
    std::string out(text);
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    if (out == "-0") {
        out = "0";
    }
    return out;
}

/// Writes one object member per line, indented, with commas between members.
class ObjectWriter {
public:
    ObjectWriter(std::ostringstream& out, int indent) : out_(out), indent_(indent) {
        out_ << "{";
    }

    void field(const char* key, const std::string& value) { raw(key, jsonString(value)); }
    void field(const char* key, double value) { raw(key, jsonNumber(value)); }
    void field(const char* key, std::int64_t value) { raw(key, std::to_string(value)); }
    void field(const char* key, int value) { raw(key, std::to_string(value)); }

    /// Starts a member whose value the caller writes next.
    void key(const char* name) {
        separator();
        out_ << jsonString(name) << ": ";
    }

    void close() {
        if (!empty_) {
            out_ << '\n' << std::string(static_cast<std::size_t>(indent_), ' ');
        }
        out_ << "}";
    }

private:
    std::ostringstream& out_;
    int indent_;
    bool empty_{true};

    void separator() {
        out_ << (empty_ ? "\n" : ",\n") << std::string(static_cast<std::size_t>(indent_ + 2), ' ');
        empty_ = false;
    }

    void raw(const char* name, const std::string& value) {
        key(name);
        out_ << value;
    }
};

void writeCamera(std::ostringstream& out, const CameraStatus& camera, int indent) {
    ObjectWriter object(out, indent);
    object.field("id", camera.id);
    object.field("ip", camera.ip);
    object.field("mac", camera.mac);
    object.field("model", camera.model);
    object.field("link", camera.link);
    object.field("rtsp", camera.rtsp);
    object.field("video", camera.video);
    object.field("decoder", camera.decoder);
    object.field("width", camera.width);
    object.field("height", camera.height);
    object.field("anpr", camera.anpr);
    object.field("error", camera.error);
    object.field("action", camera.action);
    object.field("detail", camera.detail);
    object.field("input_fps", camera.input_fps);
    object.field("processed_fps", camera.processed_fps);
    object.field("detector_fps", camera.detector_fps);
    object.field("detector_avg_ms", camera.detector_avg_ms);
    object.field("ocr_calls", camera.ocr_calls);
    object.field("ocr_avg_ms", camera.ocr_avg_ms);
    object.field("plates_confirmed", camera.plates_confirmed);
    object.field("live_dropped", camera.live_dropped);
    object.field("stale_dropped", camera.stale_dropped);
    object.field("reconnects", camera.reconnects);
    object.field("capture_to_process_ms", camera.capture_to_process_ms);
    object.field("first_frame_ms", camera.first_frame_ms);
    object.field("last_plate", camera.last_plate);
    object.field("last_plate_time", camera.last_plate_time);
    object.close();
}

void writeSystem(std::ostringstream& out, const SystemStatus& system, int indent) {
    ObjectWriter object(out, indent);
    object.field("mem_total_mb", system.mem_total_mb);
    object.field("mem_available_mb", system.mem_available_mb);
    object.field("process_rss_mb", system.process_rss_mb);
    object.field("container_mem_mb", system.container_mem_mb);
    object.field("load1", system.load1);
    object.field("cpu_percent", system.cpu_percent);
    object.field("gpu_percent", system.gpu_percent);
    object.field("detector_backend", system.detector_backend);
    object.field("ocr_backend", system.ocr_backend);
    object.close();
}

int asIntField(const json::Value& object, const char* key, int fallback) {
    return static_cast<int>(object.getInt(key, fallback));
}

void readCamera(const json::Value& object, CameraStatus& camera) {
    camera.id = object.getString("id", camera.id);
    camera.ip = object.getString("ip", camera.ip);
    camera.mac = object.getString("mac", camera.mac);
    camera.model = object.getString("model", camera.model);
    camera.link = object.getString("link", camera.link);
    camera.rtsp = object.getString("rtsp", camera.rtsp);
    camera.video = object.getString("video", camera.video);
    camera.decoder = object.getString("decoder", camera.decoder);
    camera.width = asIntField(object, "width", camera.width);
    camera.height = asIntField(object, "height", camera.height);
    camera.anpr = object.getString("anpr", camera.anpr);
    camera.error = object.getString("error", camera.error);
    camera.action = object.getString("action", camera.action);
    camera.detail = object.getString("detail", camera.detail);
    camera.input_fps = object.getNumber("input_fps", camera.input_fps);
    camera.processed_fps = object.getNumber("processed_fps", camera.processed_fps);
    camera.detector_fps = object.getNumber("detector_fps", camera.detector_fps);
    camera.detector_avg_ms = object.getNumber("detector_avg_ms", camera.detector_avg_ms);
    camera.ocr_calls = object.getInt("ocr_calls", camera.ocr_calls);
    camera.ocr_avg_ms = object.getNumber("ocr_avg_ms", camera.ocr_avg_ms);
    camera.plates_confirmed = object.getInt("plates_confirmed", camera.plates_confirmed);
    camera.live_dropped = object.getInt("live_dropped", camera.live_dropped);
    camera.stale_dropped = object.getInt("stale_dropped", camera.stale_dropped);
    camera.reconnects = object.getInt("reconnects", camera.reconnects);
    camera.capture_to_process_ms =
        object.getNumber("capture_to_process_ms", camera.capture_to_process_ms);
    camera.first_frame_ms = object.getNumber("first_frame_ms", camera.first_frame_ms);
    camera.last_plate = object.getString("last_plate", camera.last_plate);
    camera.last_plate_time = object.getString("last_plate_time", camera.last_plate_time);
}

void readSystem(const json::Value& object, SystemStatus& system) {
    system.mem_total_mb = object.getInt("mem_total_mb", system.mem_total_mb);
    system.mem_available_mb = object.getInt("mem_available_mb", system.mem_available_mb);
    system.process_rss_mb = object.getInt("process_rss_mb", system.process_rss_mb);
    system.container_mem_mb = object.getInt("container_mem_mb", system.container_mem_mb);
    system.load1 = object.getNumber("load1", system.load1);
    system.cpu_percent = object.getNumber("cpu_percent", system.cpu_percent);
    system.gpu_percent = object.getNumber("gpu_percent", system.gpu_percent);
    system.detector_backend = object.getString("detector_backend", system.detector_backend);
    system.ocr_backend = object.getString("ocr_backend", system.ocr_backend);
}

/// Unique per process and call, so two writers never share a half-written temporary file.
std::string temporaryPathFor(const std::string& path) {
    static std::atomic<unsigned> counter{0};
    return path + ".tmp." + std::to_string(static_cast<long>(::getpid())) + "." +
           std::to_string(counter.fetch_add(1));
}

std::string orDash(const std::string& value) {
    return value.empty() ? std::string("-") : value;
}

}  // namespace

std::string toJson(const StatusSnapshot& snapshot) {
    std::ostringstream out;
    ObjectWriter root(out, 0);
    root.field("version", kStatusFormatVersion);
    root.field("updated_at", snapshot.updated_at);
    root.field("updated_unix_ms", snapshot.updated_unix_ms);
    root.field("pid", snapshot.pid);
    root.field("camera_lan", snapshot.camera_lan);
    root.field("internet", snapshot.internet);
    root.key("system");
    writeSystem(out, snapshot.system, 2);
    root.key("cameras");
    if (snapshot.cameras.empty()) {
        out << "[]";
    } else {
        out << "[";
        for (std::size_t i = 0; i < snapshot.cameras.size(); ++i) {
            out << (i == 0 ? "\n" : ",\n") << "    ";
            writeCamera(out, snapshot.cameras[i], 4);
        }
        out << "\n  ]";
    }
    root.close();
    out << '\n';
    return out.str();
}

bool fromJson(const std::string& text, StatusSnapshot& snapshot, std::string& error) {
    error.clear();
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.ok) {
        error = "malformed status file: " + parsed.error;
        return false;
    }
    const json::Value& root = parsed.value;
    if (!root.isObject()) {
        error = "malformed status file: the document must be a JSON object";
        return false;
    }

    StatusSnapshot result;
    result.updated_at = root.getString("updated_at", result.updated_at);
    result.updated_unix_ms = root.getInt("updated_unix_ms", result.updated_unix_ms);
    result.pid = asIntField(root, "pid", result.pid);
    result.camera_lan = root.getString("camera_lan", result.camera_lan);
    result.internet = root.getString("internet", result.internet);

    if (const json::Value* system = root.find("system"); system != nullptr && !system->isNull()) {
        if (!system->isObject()) {
            error = "malformed status file: 'system' must be an object";
            return false;
        }
        readSystem(*system, result.system);
    }
    if (const json::Value* cameras = root.find("cameras");
        cameras != nullptr && !cameras->isNull()) {
        if (!cameras->isArray()) {
            error = "malformed status file: 'cameras' must be an array";
            return false;
        }
        for (const json::Value& item : cameras->items()) {
            if (!item.isObject()) {
                error = "malformed status file: every 'cameras' entry must be an object";
                return false;
            }
            CameraStatus camera;
            readCamera(item, camera);
            result.cameras.push_back(std::move(camera));
        }
    }
    snapshot = std::move(result);
    return true;
}

bool writeStatusFile(const std::string& path, const StatusSnapshot& snapshot, std::string& error) {
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
    // `camera-status` may read while the runner writes: the rename swaps the whole file at once.
    const std::string temporary = temporaryPathFor(path);
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output) {
            error = "cannot write " + temporary;
            return false;
        }
        output << toJson(snapshot);
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

bool readStatusFile(const std::string& path, StatusSnapshot& snapshot, std::string& error) {
    error.clear();
    std::error_code ec;
    if (filesystem::is_directory(path, ec)) {
        error = "cannot read " + path + ": it is a directory";
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!fromJson(buffer.str(), snapshot, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

std::string formatStatusTable(const StatusSnapshot& snapshot) {
    std::ostringstream out;
    out << "updated " << orDash(snapshot.updated_at)
        << "  camera LAN: " << orDash(snapshot.camera_lan)
        << "  Internet: " << orDash(snapshot.internet) << '\n';
    if (snapshot.cameras.empty()) {
        out << "no cameras\n";
        return out.str();
    }

    constexpr std::size_t kColumns = 7;
    using Row = std::array<std::string, kColumns>;
    std::vector<Row> rows;
    rows.push_back(Row{"CAMERA", "IP", "LINK", "RTSP", "VIDEO", "ANPR", "ERROR"});
    for (const CameraStatus& camera : snapshot.cameras) {
        rows.push_back(Row{orDash(camera.id), orDash(camera.ip), orDash(camera.link),
                           orDash(camera.rtsp), orDash(camera.video), orDash(camera.anpr),
                           camera.error});
    }
    std::array<std::size_t, kColumns> widths{};
    for (const Row& row : rows) {
        for (std::size_t column = 0; column < kColumns; ++column) {
            widths[column] = std::max(widths[column], row[column].size());
        }
    }
    for (const Row& row : rows) {
        std::string line;
        for (std::size_t column = 0; column < kColumns; ++column) {
            line += row[column];
            if (column + 1 < kColumns) {
                line.append(widths[column] - row[column].size() + 2, ' ');
            }
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out << line << '\n';
    }
    return out.str();
}

}  // namespace anpr::cameras
