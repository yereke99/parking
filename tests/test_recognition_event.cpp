#include <fstream>
#include <string>

#include "anpr/common/filesystem.hpp"
#include "anpr/pipeline/plate_sink.hpp"
#include "anpr/pipeline/recognition_event.hpp"

#include "test_framework.hpp"

namespace {

anpr::PlateRecognitionEvent confirmedEvent() {
    anpr::PlateRecognitionEvent event;
    event.status = anpr::RecognitionStatus::kValidHighConfidence;
    event.normalized_plate = "152JTA02";
    event.raw_plate = "152JTA02";
    event.confidence = 0.83;
    event.timestamp_ms = 8533;
    event.unix_time_ms = 1791443523120;  // 2026-10-08T07:12:03.120Z
    event.camera_id = "gate-01";
    event.region_code = std::string("02");
    event.region_name = std::string("Almaty");
    event.format_name = "kz_individual";
    return event;
}

int countLines(const std::string& path) {
    std::ifstream in(path);
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lines;
    }
    return lines;
}

}  // namespace

TEST("an event carries its wall-clock time as ISO 8601 UTC") {
    const std::string json = anpr::toJson(confirmedEvent());
    CHECK(json.find("\"time\":\"2026-10-08T07:12:03.120Z\"") != std::string::npos);
    CHECK(json.find("\"status\":\"VALID_HIGH_CONFIDENCE\"") != std::string::npos);
    CHECK(json.find("\"normalized_plate\":\"152JTA02\"") != std::string::npos);
    CHECK(json.find("\"region_name\":\"Almaty\"") != std::string::npos);

    anpr::PlateRecognitionEvent epoch;
    CHECK(anpr::toJson(epoch).find("\"time\":\"1970-01-01T00:00:00.000Z\"") != std::string::npos);
}

TEST("only valid readings may open the barrier") {
    CHECK(anpr::isAccepted(anpr::RecognitionStatus::kValidHighConfidence));
    CHECK(anpr::isAccepted(anpr::RecognitionStatus::kValidLowConfidence));
    CHECK(!anpr::isAccepted(anpr::RecognitionStatus::kLowConfidence));
    CHECK(!anpr::isAccepted(anpr::RecognitionStatus::kInvalidFormat));
    CHECK(!anpr::isAccepted(anpr::RecognitionStatus::kTimeout));
}

TEST("snapshot metadata is optional and carries the selected video frame time") {
    auto event = confirmedEvent();
    CHECK(anpr::toJson(event).find("snapshot_path") == std::string::npos);
    event.snapshot_path = "photos/152JTA02_8400_t1.jpg";
    event.snapshot_timestamp_ms = 8400;
    const std::string json = anpr::toJson(event);
    CHECK(json.find("\"snapshot_path\":\"photos/152JTA02_8400_t1.jpg\"") != std::string::npos);
    CHECK(json.find("\"snapshot_timestamp_ms\":8400") != std::string::npos);
    CHECK(json.find("\"timestamp_ms\":8533") != std::string::npos);
}

TEST("the events file gets one line per event and is appended across restarts") {
    const anpr::filesystem::path path =
        anpr::filesystem::temp_directory_path() / "kz_anpr_test_events.jsonl";
    anpr::filesystem::remove(path);
    {
        anpr::JsonLinesFileSink sink(path.string());
        CHECK(sink.ok());
        sink.onRecognition(confirmedEvent());
        sink.onRecognition(confirmedEvent());
    }
    CHECK_EQ(countLines(path.string()), 2);
    {
        anpr::JsonLinesFileSink sink(path.string());
        sink.onRecognition(confirmedEvent());
    }
    CHECK_EQ(countLines(path.string()), 3);
    anpr::filesystem::remove(path);
}

TEST("an events file in a missing directory reports that it cannot be opened") {
    anpr::JsonLinesFileSink sink("/nonexistent-kz-anpr-dir/events.jsonl");
    CHECK(!sink.ok());
}
