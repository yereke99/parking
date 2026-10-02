#include "anpr/ocr/nomeroff_ocr.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

struct WorkerResult {
    OcrResult result;
    double inference_ms{0.0};
};

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

bool parseDouble(const std::string& text, double& value) {
    try {
        std::size_t used = 0;
        value = std::stod(text, &used);
        return used == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

std::string workerKey(const OcrConfig& config) {
    std::ostringstream out;
    out << config.python_executable << '\n' << config.worker_script << '\n'
        << config.model_cache_dir << '\n' << config.device << '\n' << config.region_mode << '\n'
        << config.lines_count << '\n' << config.fp16;
    return out.str();
}

}  // namespace

class NomeroffWorker {
public:
    explicit NomeroffWorker(OcrConfig config) : config_(std::move(config)) {}
    ~NomeroffWorker() { stop(); }

    NomeroffWorker(const NomeroffWorker&) = delete;
    NomeroffWorker& operator=(const NomeroffWorker&) = delete;

    bool start(std::string& error) {
        static std::once_flag sigpipe_once;
        std::call_once(sigpipe_once, [] { std::signal(SIGPIPE, SIG_IGN); });

        if (!std::filesystem::exists(config_.python_executable)) {
            error = "NOMEROFF_UNAVAILABLE: Python executable not found: " +
                    config_.python_executable + ". Run tools/setup_nomeroff_env.sh";
            return false;
        }
        if (!std::filesystem::exists(config_.worker_script)) {
            error = "NOMEROFF_UNAVAILABLE: worker script not found: " + config_.worker_script;
            return false;
        }

        int parent_to_child[2] = {-1, -1};
        int child_to_parent[2] = {-1, -1};
        if (::pipe(parent_to_child) != 0 || ::pipe(child_to_parent) != 0) {
            error = std::string("NOMEROFF_UNAVAILABLE: pipe failed: ") + std::strerror(errno);
            closePair(parent_to_child);
            closePair(child_to_parent);
            return false;
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            error = std::string("NOMEROFF_UNAVAILABLE: fork failed: ") + std::strerror(errno);
            closePair(parent_to_child);
            closePair(child_to_parent);
            return false;
        }
        if (pid_ == 0) {
            ::dup2(parent_to_child[0], STDIN_FILENO);
            ::dup2(child_to_parent[1], STDOUT_FILENO);
            closePair(parent_to_child);
            closePair(child_to_parent);

            const std::string lines = std::to_string(config_.lines_count);
            const char* precision = config_.fp16 ? "--fp16" : "--no-fp16";
            ::execl(config_.python_executable.c_str(), config_.python_executable.c_str(),
                    config_.worker_script.c_str(), "--serve", "--region",
                    config_.region_mode.c_str(), "--lines", lines.c_str(), "--device",
                    config_.device.c_str(), precision, "--model-cache",
                    config_.model_cache_dir.c_str(), static_cast<char*>(nullptr));
            std::cerr << "NOMEROFF_UNAVAILABLE: exec failed: " << std::strerror(errno) << '\n';
            ::_exit(127);
        }

        ::close(parent_to_child[0]);
        ::close(child_to_parent[1]);
        write_fd_ = parent_to_child[1];
        read_fd_ = child_to_parent[0];

        std::string ready;
        if (!readLine(ready, config_.startup_timeout_ms, error)) {
            error = "NOMEROFF_UNAVAILABLE: worker did not become ready: " + error;
            stop();
            return false;
        }
        const auto fields = splitTabs(ready);
        if (fields.size() < 10 || fields[0] != "READY") {
            error = "NOMEROFF_UNAVAILABLE: invalid worker handshake: " + ready;
            stop();
            return false;
        }
        version_ = fields[1];
        commit_ = fields[2];
        device_ = fields[3];
        model_ = fields[4];
        region_ = fields[5];
        fp16_ = fields[6] == "1";
        double startup_ms = 0.0;
        double rss_mb = 0.0;
        double cuda_memory_mb = 0.0;
        parseDouble(fields[7], startup_ms);
        parseDouble(fields[8], rss_mb);
        parseDouble(fields[9], cuda_memory_mb);
        const std::string fallback = fields.size() > 10 ? fields[10] : "";
        logEvent(LogLevel::kInfo, "nomeroff_ready",
                 LogFields()
                     .add("version", version_)
                     .add("commit", commit_)
                     .add("model", model_)
                     .add("region", region_)
                     .add("device", device_)
                     .add("fp16", fp16_ ? 1 : 0)
                     .add("startup_ms", startup_ms)
                     .add("rss_mb", rss_mb)
                     .add("cuda_memory_mb", cuda_memory_mb)
                     .add("fallback", fallback.empty() ? "none" : fallback));
        return true;
    }

    WorkerResult recognize(const cv::Mat& plate) {
        const std::lock_guard<std::mutex> guard(mutex_);
        WorkerResult response;
        if (plate.empty() || plate.channels() != 3 || plate.depth() != CV_8U) {
            response.result.rejection = OcrRejection::kEmptyCrop;
            return response;
        }
        if (write_fd_ < 0 || read_fd_ < 0) {
            response.result.rejection = OcrRejection::kInferenceFailed;
            return response;
        }

        const std::uint64_t request_id = ++next_request_id_;
        const std::size_t row_bytes = static_cast<std::size_t>(plate.cols) * plate.elemSize();
        const std::size_t byte_count = row_bytes * static_cast<std::size_t>(plate.rows);
        std::ostringstream header;
        header << "OCR\t" << request_id << '\t' << plate.rows << '\t' << plate.cols << '\t'
               << plate.channels() << '\t' << byte_count << "\t0\n";
        std::string error;
        const std::string header_text = header.str();
        if (!writeAll(header_text.data(), header_text.size(), error)) {
            response.result.rejection = OcrRejection::kInferenceFailed;
            logEvent(LogLevel::kError, "nomeroff_worker_write_failed",
                     LogFields().add("reason", error));
            return response;
        }
        for (int row = 0; row < plate.rows; ++row) {
            if (!writeAll(plate.ptr(row), row_bytes, error)) {
                response.result.rejection = OcrRejection::kInferenceFailed;
                logEvent(LogLevel::kError, "nomeroff_worker_write_failed",
                         LogFields().add("reason", error));
                return response;
            }
        }

        std::string line;
        if (!readLine(line, config_.request_timeout_ms, error)) {
            response.result.rejection = OcrRejection::kInferenceFailed;
            logEvent(LogLevel::kError, "nomeroff_worker_timeout",
                     LogFields().add("reason", error).add("request_id", request_id));
            return response;
        }
        const auto fields = splitTabs(line);
        if (fields.size() >= 4 && fields[0] == "ERROR") {
            response.result.rejection = OcrRejection::kInferenceFailed;
            logEvent(fields[2] == "CUDA_OOM" ? LogLevel::kError : LogLevel::kWarn,
                     fields[2] == "CUDA_OOM" ? "gpu_oom" : "nomeroff_ocr_failed",
                     LogFields().add("reason", fields[3]).add("request_id", request_id));
            return response;
        }
        if (fields.size() != 7 || fields[0] != "RESULT" ||
            fields[1] != std::to_string(request_id)) {
            response.result.rejection = OcrRejection::kInferenceFailed;
            logEvent(LogLevel::kError, "nomeroff_protocol_error",
                     LogFields().add("response", line));
            return response;
        }
        double confidence = 0.0;
        double min_confidence = 0.0;
        if (!parseDouble(fields[3], confidence) || !parseDouble(fields[4], min_confidence) ||
            !parseDouble(fields[6], response.inference_ms)) {
            response.result.rejection = OcrRejection::kInferenceFailed;
            return response;
        }
        response.result.text = fields[2];
        response.result.confidence = static_cast<float>(std::clamp(confidence, 0.0, 1.0));
        response.result.min_char_confidence =
            static_cast<float>(std::clamp(min_confidence, 0.0, 1.0));
        response.result.region = fields[5];
        if (response.result.text.empty()) {
            response.result.rejection = OcrRejection::kAllPadding;
        }
        return response;
    }

    [[nodiscard]] std::string backendName() const {
        return "nomeroff_pytorch_" + device_ + (fp16_ ? "_fp16" : "_fp32");
    }

    [[nodiscard]] std::string description() const {
        return "Nomeroff Net " + version_ + "@" + commit_.substr(0, 12) + " model=" + model_ +
               " region=" + region_;
    }

private:
    OcrConfig config_;
    pid_t pid_{-1};
    int write_fd_{-1};
    int read_fd_{-1};
    std::uint64_t next_request_id_{0};
    std::mutex mutex_;
    std::string version_;
    std::string commit_;
    std::string device_;
    std::string model_;
    std::string region_;
    bool fp16_{false};

    static void closePair(int (&fds)[2]) {
        for (int& fd : fds) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    }

    bool writeAll(const void* data, std::size_t size, std::string& error) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t written = 0;
        while (written < size) {
            const ssize_t count = ::write(write_fd_, bytes + written, size - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                error = std::strerror(errno);
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool readLine(std::string& line, std::int64_t timeout_ms, std::string& error) {
        line.clear();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (line.size() < 65536) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                error = "response timeout";
                return false;
            }
            pollfd descriptor{read_fd_, POLLIN, 0};
            const int status = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
            if (status < 0 && errno == EINTR) {
                continue;
            }
            if (status <= 0) {
                error = status == 0 ? "response timeout" : std::strerror(errno);
                return false;
            }
            char value = '\0';
            const ssize_t count = ::read(read_fd_, &value, 1);
            if (count <= 0) {
                error = "worker closed its output";
                return false;
            }
            if (value == '\n') {
                return true;
            }
            if (value != '\r') {
                line.push_back(value);
            }
        }
        error = "worker response exceeded 64 KiB";
        return false;
    }

    void stop() {
        if (write_fd_ >= 0) {
            std::string ignored;
            writeAll("STOP\n", 5, ignored);
            ::close(write_fd_);
            write_fd_ = -1;
        }
        if (read_fd_ >= 0) {
            ::close(read_fd_);
            read_fd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                    pid_ = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }
};

NomeroffRecognizer::NomeroffRecognizer(OcrConfig config,
                                       std::shared_ptr<NomeroffWorker> worker,
                                       PipelineMetrics* metrics)
    : config_(std::move(config)), worker_(std::move(worker)), metrics_(metrics) {}

OcrResult NomeroffRecognizer::recognize(const cv::Mat& plate) {
    const auto started = std::chrono::steady_clock::now();
    WorkerResult response = worker_->recognize(plate);
    const double total_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    if (metrics_ != nullptr) {
        metrics_->ocr_total.add(total_ms);
        metrics_->ocr_inference.add(response.inference_ms);
        metrics_->ocr_preprocess.add(std::max(0.0, total_ms - response.inference_ms));
        ++metrics_->ocr_calls;
    }
    if (response.result.rejection == OcrRejection::kNone &&
        response.result.min_char_confidence < config_.min_char_confidence) {
        response.result.rejection = OcrRejection::kWeakCharacter;
    }
    if (response.result.rejection == OcrRejection::kNone &&
        response.result.confidence < config_.min_confidence) {
        response.result.rejection = OcrRejection::kLowConfidence;
    }
    if (metrics_ != nullptr && !response.result.ok()) {
        ++metrics_->ocr_empty;
    }
    return response.result;
}

std::string NomeroffRecognizer::backendName() const { return worker_->backendName(); }

std::string NomeroffRecognizer::modelDescription() const { return worker_->description(); }

std::unique_ptr<IPlateOcr> makeNomeroffRecognizer(const OcrConfig& config,
                                                   PipelineMetrics* metrics,
                                                   std::string& error) {
    static std::mutex registry_mutex;
    static std::unordered_map<std::string, std::weak_ptr<NomeroffWorker>> registry;
    const std::string key = workerKey(config);

    std::shared_ptr<NomeroffWorker> worker;
    {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        worker = registry[key].lock();
        if (worker == nullptr) {
            worker = std::make_shared<NomeroffWorker>(config);
            if (!worker->start(error)) {
                return nullptr;
            }
            registry[key] = worker;
        }
    }
    return std::make_unique<NomeroffRecognizer>(config, std::move(worker), metrics);
}

}  // namespace anpr
