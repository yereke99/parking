#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "anpr/cameras/gst_capture.hpp"
#include "anpr/cameras/rtsp_camera_source.hpp"
#include "anpr/common/filesystem.hpp"
#include "test_framework.hpp"

using anpr::cameras::GstCapture;

TEST("redactSecrets masks pipeline credentials and URL user-info") {
    using anpr::cameras::redactSecrets;
    const std::string pipeline =
        "rtspsrc location=\"rtsp://192.168.10.21:554/Streaming/Channels/101\" "
        "user-id=\"admin\" user-pw=\"p@ss \\\"word\" protocols=tcp ! appsink name=sink";
    const std::string redacted = redactSecrets(pipeline);
    CHECK(redacted.find("admin") == std::string::npos);
    CHECK(redacted.find("p@ss") == std::string::npos);
    CHECK(redacted.find("word") == std::string::npos);
    CHECK(redacted.find("user-id=*** user-pw=*** protocols=tcp") != std::string::npos);
    CHECK(redacted.find("rtsp://192.168.10.21:554/Streaming/Channels/101") != std::string::npos);

    CHECK_EQ(redactSecrets("could not open rtsp://admin:se:cr@t@192.168.10.21:554/x now"),
             std::string("could not open rtsp://<redacted>@192.168.10.21:554/x now"));
    CHECK_EQ(redactSecrets("user-pw=plain!"), std::string("user-pw=***!"));
    CHECK_EQ(redactSecrets("rtsp://<redacted>@10.0.0.1/a"),
             std::string("rtsp://<redacted>@10.0.0.1/a"));
    CHECK_EQ(redactSecrets("no secrets here"), std::string("no secrets here"));
}

TEST("pictureSize reports the picture lines of an I420 matrix") {
    int width = 0;
    int height = 0;
    anpr::cameras::pictureSize(cv::Mat(360, 320, CV_8UC1), anpr::PixelFormat::kI420, width, height);
    CHECK_EQ(width, 320);
    CHECK_EQ(height, 240);
    anpr::cameras::pictureSize(cv::Mat(240, 320, CV_8UC3), anpr::PixelFormat::kBgr, width, height);
    CHECK_EQ(height, 240);
}

TEST("StreamDecoder reads a stream through OpenCV's FFmpeg backend") {
    const anpr::filesystem::path path =
        anpr::filesystem::temp_directory_path() /
        ("kz_anpr_stream_decoder_" + std::to_string(static_cast<long long>(::getpid())) + ".avi");
    {
        cv::VideoWriter writer(path.string(), cv::CAP_FFMPEG,
                               cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 25.0, cv::Size(96, 64));
        if (!writer.isOpened()) {
            // No FFmpeg in this OpenCV build: the planner never offers the attempt then.
            return;
        }
        for (int i = 0; i < 6; ++i) {
            writer.write(cv::Mat(64, 96, CV_8UC3, cv::Scalar(20 * i, 40, 200)));
        }
    }
    anpr::cameras::DecoderAttempt attempt;
    attempt.decoder = anpr::cameras::kDecoderSoftwareFfmpeg;
    attempt.backend = anpr::cameras::kBackendOpenCvFfmpeg;
    attempt.source = path.string();
    anpr::cameras::StreamDecoder decoder;
    std::string error;
    CHECK(decoder.open(attempt, 3000, error) == anpr::cameras::StreamDecoder::Status::kFrame);
    CHECK_EQ(decoder.width(), 96);
    CHECK_EQ(decoder.height(), 64);
    CHECK_NEAR(decoder.streamFps(), 25.0, 0.5);
    cv::Mat image;
    anpr::PixelFormat format = anpr::PixelFormat::kI420;
    int frames = 0;
    anpr::cameras::StreamDecoder::Status status = anpr::cameras::StreamDecoder::Status::kFrame;
    while (frames < 20) {
        status = decoder.read(image, format, 1000, error);
        if (status != anpr::cameras::StreamDecoder::Status::kFrame) {
            break;
        }
        CHECK(format == anpr::PixelFormat::kBgr);
        ++frames;
    }
    CHECK_EQ(frames, 6);
    CHECK(status == anpr::cameras::StreamDecoder::Status::kEndOfStream);
    decoder.close();

    attempt.source = (anpr::filesystem::temp_directory_path() / "kz_anpr_missing.avi").string();
    CHECK(decoder.open(attempt, 1000, error) == anpr::cameras::StreamDecoder::Status::kError);
    CHECK(error.find("FFmpeg") != std::string::npos);
    std::error_code ignored;
    anpr::filesystem::remove(path, ignored);
}

TEST("StreamDecoder rejects an unknown backend") {
    anpr::cameras::DecoderAttempt attempt;
    attempt.decoder = anpr::cameras::kDecoderSoftwareGstreamer;
    attempt.backend = "carrier_pigeon";
    anpr::cameras::StreamDecoder decoder;
    std::string error;
    CHECK(decoder.open(attempt, 100, error) == anpr::cameras::StreamDecoder::Status::kError);
    CHECK(error.find("carrier_pigeon") != std::string::npos);
    CHECK(!decoder.isOpen());
    cv::Mat image;
    anpr::PixelFormat format = anpr::PixelFormat::kBgr;
    CHECK(decoder.read(image, format, 10, error) == anpr::cameras::StreamDecoder::Status::kError);
}

#ifdef KZ_ANPR_WITH_GSTREAMER

namespace {

std::int64_t nowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool near(const cv::Vec3b& pixel, int blue, int green, int red, int tolerance) {
    return std::abs(pixel[0] - blue) <= tolerance && std::abs(pixel[1] - green) <= tolerance &&
           std::abs(pixel[2] - red) <= tolerance;
}

}  // namespace

TEST("GStreamer is available to the native capture") {
    CHECK(anpr::cameras::gstreamerCompiledIn());
    CHECK(anpr::cameras::gstElementAvailable("videotestsrc"));
    CHECK(anpr::cameras::gstElementAvailable("appsink"));
    CHECK(!anpr::cameras::gstElementAvailable("kz_anpr_no_such_element"));
    CHECK(!anpr::cameras::gstElementAvailable(""));
}

TEST("GstCapture delivers I420 frames as one height*3/2 x width matrix") {
    GstCapture capture;
    std::string error;
    const GstCapture::Status opened = capture.open(
        "videotestsrc num-buffers=10 pattern=red ! "
        "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! "
        "appsink name=sink sync=false",
        5000, error);
    CHECK(opened == GstCapture::Status::kFrame);
    CHECK(error.empty());
    CHECK(capture.isOpen());
    CHECK_NEAR(capture.capsFps(), 30.0, 0.01);

    cv::Mat image;
    bool i420 = false;
    CHECK(capture.pull(image, i420, 2000, error) == GstCapture::Status::kFrame);
    CHECK(i420);
    CHECK_EQ(image.type(), CV_8UC1);
    CHECK_EQ(image.rows, 360);
    CHECK_EQ(image.cols, 320);
    CHECK(image.isContinuous());

    // Planes in the right order: red converts back to red, not blue.
    cv::Mat bgr;
    cv::cvtColor(image, bgr, cv::COLOR_YUV2BGR_I420);
    CHECK_EQ(bgr.rows, 240);
    CHECK(near(bgr.at<cv::Vec3b>(10, 10), 0, 0, 255, 6));
    CHECK(near(bgr.at<cv::Vec3b>(239, 319), 0, 0, 255, 6));
    capture.close();
    CHECK(!capture.isOpen());
    capture.close();
}

TEST("GstCapture honours padded plane strides") {
    // 322 is not a multiple of 4: GStreamer pads the I420 rows (Y stride 324, chroma 164) and
    // the BGR rows (stride 968). A copy that ignored the strides would shear the picture.
    for (const char* format : {"I420", "BGR"}) {
        GstCapture capture;
        std::string error;
        const std::string description = std::string("videotestsrc num-buffers=3 pattern=blue ! "
                                                    "video/x-raw,format=") +
                                        format + ",width=322,height=242 ! appsink name=sink "
                                                 "sync=false";
        CHECK(capture.open(description, 5000, error) == GstCapture::Status::kFrame);
        cv::Mat image;
        bool i420 = false;
        CHECK(capture.pull(image, i420, 2000, error) == GstCapture::Status::kFrame);
        cv::Mat bgr;
        if (i420) {
            CHECK_EQ(image.rows, 363);
            CHECK_EQ(image.cols, 322);
            cv::cvtColor(image, bgr, cv::COLOR_YUV2BGR_I420);
        } else {
            CHECK_EQ(std::string(format), std::string("BGR"));
            CHECK_EQ(image.type(), CV_8UC3);
            bgr = image;
        }
        CHECK_EQ(bgr.rows, 242);
        CHECK_EQ(bgr.cols, 322);
        CHECK(near(bgr.at<cv::Vec3b>(0, 0), 255, 0, 0, 6));
        CHECK(near(bgr.at<cv::Vec3b>(241, 321), 255, 0, 0, 6));
        CHECK(near(bgr.at<cv::Vec3b>(120, 161), 255, 0, 0, 6));
    }
}

TEST("GstCapture drops the odd last row and column of an I420 frame") {
    GstCapture capture;
    std::string error;
    CHECK(capture.open("videotestsrc num-buffers=2 ! video/x-raw,format=I420,width=321,height=241 "
                       "! appsink name=sink sync=false",
                       5000, error) == GstCapture::Status::kFrame);
    cv::Mat image;
    bool i420 = false;
    CHECK(capture.pull(image, i420, 2000, error) == GstCapture::Status::kFrame);
    CHECK_EQ(image.cols, 320);
    CHECK_EQ(image.rows, 360);
}

TEST("GstCapture delivers BGR frames") {
    GstCapture capture;
    std::string error;
    CHECK(capture.open("videotestsrc num-buffers=5 pattern=green ! "
                       "video/x-raw,format=BGR,width=320,height=240 ! appsink name=sink sync=false",
                       5000, error) == GstCapture::Status::kFrame);
    cv::Mat image;
    bool i420 = true;
    CHECK(capture.pull(image, i420, 2000, error) == GstCapture::Status::kFrame);
    CHECK(!i420);
    CHECK_EQ(image.type(), CV_8UC3);
    CHECK_EQ(image.rows, 240);
    CHECK_EQ(image.cols, 320);
    CHECK(near(image.at<cv::Vec3b>(100, 100), 0, 255, 0, 6));
    // The second pull reuses the buffer.
    const uchar* data = image.data;
    CHECK(capture.pull(image, i420, 2000, error) == GstCapture::Status::kFrame);
    CHECK(image.data == data);
}

TEST("GstCapture reports a missing element with the parser's message") {
    GstCapture capture;
    std::string error;
    const GstCapture::Status status = capture.open(
        "videotestsrc ! kz_anpr_missing_decoder ! appsink name=sink", 1000, error);
    CHECK(status == GstCapture::Status::kError);
    CHECK(error.find("kz_anpr_missing_decoder") != std::string::npos);
    CHECK(!capture.isOpen());
}

TEST("GstCapture requires an appsink named sink or a single unnamed one") {
    GstCapture capture;
    std::string error;
    CHECK(capture.open("videotestsrc num-buffers=1 ! fakesink", 1000, error) ==
          GstCapture::Status::kError);
    CHECK(error.find("appsink") != std::string::npos);
    CHECK(!capture.isOpen());

    CHECK(capture.open("videotestsrc num-buffers=1 ! tee name=t ! queue ! appsink t. ! queue ! "
                       "appsink",
                       1000, error) == GstCapture::Status::kError);
    CHECK(error.find("appsink") != std::string::npos);

    CHECK(capture.open("videotestsrc num-buffers=2 ! video/x-raw,format=BGR,width=32,height=24 ! "
                       "appsink name=opencvsink sync=false",
                       3000, error) == GstCapture::Status::kFrame);
    cv::Mat image;
    bool i420 = true;
    CHECK(capture.pull(image, i420, 1000, error) == GstCapture::Status::kFrame);
    CHECK_EQ(image.cols, 32);
}

TEST("GstCapture returns the bus error posted by a streaming thread") {
    // identity error-after=N posts an ERROR from the streaming thread at the Nth buffer, as
    // rtspsrc does for a refused login or a broken connection.
    GstCapture capture;
    std::string error;
    const std::int64_t started = nowMs();
    CHECK(capture.open("videotestsrc ! identity error-after=1 ! appsink name=sink", 5000, error) ==
          GstCapture::Status::kError);
    CHECK(error.find("identity0: Failed after iterations as requested.") != std::string::npos);
    // Source locations from the debug text are left out.
    CHECK(error.find("gstidentity.c") == std::string::npos);
    // Seen on the bus, not after the whole timeout.
    CHECK(nowMs() - started < 3000);
    CHECK(!capture.isOpen());

    CHECK(capture.open("videotestsrc ! video/x-raw,format=I420,width=64,height=48 ! "
                       "identity error-after=4 ! appsink name=sink sync=false",
                       5000, error) == GstCapture::Status::kFrame);
    cv::Mat image;
    bool i420 = false;
    int frames = 0;
    GstCapture::Status status = GstCapture::Status::kFrame;
    for (int i = 0; i < 10 && status == GstCapture::Status::kFrame; ++i) {
        status = capture.pull(image, i420, 2000, error);
        frames += status == GstCapture::Status::kFrame ? 1 : 0;
    }
    CHECK_EQ(frames, 3);
    CHECK(status == GstCapture::Status::kError);
    CHECK(error.find("Failed after iterations") != std::string::npos);
}

TEST("GstCapture times out on a pipeline that never produces a frame") {
    GstCapture capture;
    std::string error;
    const std::int64_t started = nowMs();
    const GstCapture::Status status = capture.open(
        "videotestsrc is-live=true ! valve drop=true ! appsink name=sink", 300, error);
    const std::int64_t elapsed = nowMs() - started;
    CHECK(status == GstCapture::Status::kTimeout);
    CHECK(error.find("300 ms") != std::string::npos);
    CHECK(elapsed >= 250);
    CHECK(elapsed < 2000);
    CHECK(!capture.isOpen());
}

TEST("GstCapture and StreamDecoder waits end early when interrupted") {
    std::atomic_bool interrupted{false};
    GstCapture capture;
    capture.setInterruptFlag(&interrupted);
    std::thread interrupter([&interrupted] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        interrupted.store(true);
    });
    std::string error;
    const std::int64_t started = nowMs();
    const GstCapture::Status status = capture.open(
        "videotestsrc is-live=true ! valve drop=true ! appsink name=sink", 5000, error);
    const std::int64_t elapsed = nowMs() - started;
    interrupter.join();
    CHECK(status == GstCapture::Status::kError);
    CHECK_EQ(error, std::string("interrupted"));
    CHECK(elapsed < 1500);
    CHECK(!capture.isOpen());

    anpr::cameras::DecoderAttempt attempt;
    attempt.decoder = anpr::cameras::kDecoderSoftwareGstreamer;
    attempt.backend = anpr::cameras::kBackendGstNative;
    attempt.source = "videotestsrc is-live=true ! valve drop=true ! appsink name=sink";
    anpr::cameras::StreamDecoder decoder;
    std::thread stopper([&decoder] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        decoder.interrupt();
    });
    const std::int64_t decoder_started = nowMs();
    CHECK(decoder.open(attempt, 5000, error) == anpr::cameras::StreamDecoder::Status::kError);
    stopper.join();
    CHECK(nowMs() - decoder_started < 1500);
    CHECK_EQ(error, std::string("interrupted"));
    // Interrupted for good: a later open fails at once.
    CHECK(decoder.open(attempt, 5000, error) == anpr::cameras::StreamDecoder::Status::kError);
}

TEST("GstCapture bounds a read on a stream that goes quiet") {
    GstCapture capture;
    std::string error;
    // One frame per second: the first arrives at once, the next not within 200 ms.
    CHECK(capture.open("videotestsrc is-live=true ! "
                       "video/x-raw,format=I420,width=64,height=48,framerate=1/1 ! "
                       "appsink name=sink drop=true max-buffers=1 sync=false",
                       3000, error) == GstCapture::Status::kFrame);
    cv::Mat image;
    bool i420 = false;
    CHECK(capture.pull(image, i420, 1000, error) == GstCapture::Status::kFrame);
    const std::int64_t started = nowMs();
    CHECK(capture.pull(image, i420, 200, error) == GstCapture::Status::kTimeout);
    const std::int64_t elapsed = nowMs() - started;
    CHECK(elapsed >= 150);
    CHECK(elapsed < 900);
    CHECK(capture.isOpen());
}

TEST("GstCapture reports the end of a finite stream") {
    GstCapture capture;
    std::string error;
    CHECK(capture.open("videotestsrc num-buffers=3 ! video/x-raw,format=I420,width=64,height=48 ! "
                       "appsink name=sink sync=false",
                       5000, error) == GstCapture::Status::kFrame);
    cv::Mat image;
    bool i420 = false;
    int frames = 0;
    GstCapture::Status status = GstCapture::Status::kFrame;
    for (int i = 0; i < 10; ++i) {
        status = capture.pull(image, i420, 2000, error);
        if (status != GstCapture::Status::kFrame) {
            break;
        }
        ++frames;
    }
    CHECK_EQ(frames, 3);
    CHECK(status == GstCapture::Status::kEndOfStream);
    CHECK(!error.empty());
}

TEST("GstCapture can be reopened and pulls fail cleanly when closed") {
    GstCapture capture;
    std::string error;
    cv::Mat image;
    bool i420 = false;
    CHECK(capture.pull(image, i420, 10, error) == GstCapture::Status::kError);
    for (int round = 0; round < 3; ++round) {
        CHECK(capture.open("videotestsrc num-buffers=2 ! "
                           "video/x-raw,format=I420,width=32,height=32 ! appsink name=sink "
                           "sync=false",
                           5000, error) == GstCapture::Status::kFrame);
        CHECK(capture.pull(image, i420, 2000, error) == GstCapture::Status::kFrame);
    }
    capture.close();
    CHECK(capture.pull(image, i420, 10, error) == GstCapture::Status::kError);
}

TEST("StreamDecoder opens a native GStreamer attempt and hands out the first frame first") {
    anpr::cameras::DecoderAttempt attempt;
    attempt.decoder = anpr::cameras::kDecoderSoftwareGstreamer;
    attempt.backend = anpr::cameras::kBackendGstNative;
    attempt.source = "videotestsrc num-buffers=4 ! video/x-raw,format=I420,width=160,height=120,"
                     "framerate=25/1 ! appsink name=sink sync=false";
    anpr::cameras::StreamDecoder decoder;
    std::string error;
    CHECK(decoder.open(attempt, 3000, error) == anpr::cameras::StreamDecoder::Status::kFrame);
    CHECK(decoder.isOpen());
    CHECK_EQ(decoder.width(), 160);
    CHECK_EQ(decoder.height(), 120);
    CHECK_NEAR(decoder.streamFps(), 25.0, 0.01);
    cv::Mat image;
    anpr::PixelFormat format = anpr::PixelFormat::kBgr;
    int frames = 0;
    using DecoderStatus = anpr::cameras::StreamDecoder::Status;
    while (decoder.read(image, format, 2000, error) == DecoderStatus::kFrame) {
        CHECK(format == anpr::PixelFormat::kI420);
        CHECK_EQ(image.rows, 180);
        ++frames;
    }
    CHECK_EQ(frames, 4);
    decoder.close();
    CHECK(!decoder.isOpen());

    attempt.source = "videotestsrc ! no_such_kz_element ! appsink name=sink";
    CHECK(decoder.open(attempt, 1000, error) == anpr::cameras::StreamDecoder::Status::kError);
    CHECK(error.find("no_such_kz_element") != std::string::npos);
    CHECK(!decoder.isOpen());
}

TEST("StreamDecoder opens a planned pipeline through OpenCV's GStreamer backend") {
    // The description as gst_pipeline plans it, with the appsink named for GstCapture: OpenCV
    // only accepts it once the sink is renamed.
    anpr::cameras::DecoderAttempt attempt;
    attempt.decoder = anpr::cameras::kDecoderSoftwareGstreamer;
    attempt.backend = anpr::cameras::kBackendOpenCvGstreamer;
    attempt.source = "videotestsrc num-buffers=30 pattern=blue ! "
                     "video/x-raw,format=BGR,width=160,height=120,framerate=25/1 ! "
                     "appsink name=sink drop=true max-buffers=1 sync=false";
    anpr::cameras::StreamDecoder decoder;
    std::string error;
    CHECK(decoder.open(attempt, 3000, error) == anpr::cameras::StreamDecoder::Status::kFrame);
    CHECK_EQ(decoder.width(), 160);
    CHECK_EQ(decoder.height(), 120);
    cv::Mat image;
    anpr::PixelFormat format = anpr::PixelFormat::kI420;
    CHECK(decoder.read(image, format, 1000, error) == anpr::cameras::StreamDecoder::Status::kFrame);
    CHECK(format == anpr::PixelFormat::kBgr);
    CHECK_EQ(image.type(), CV_8UC3);
    CHECK(near(image.at<cv::Vec3b>(60, 80), 255, 0, 0, 6));
    decoder.close();
    CHECK(!decoder.isOpen());
}

#else

TEST("GStreamer is reported as not compiled in") {
    CHECK(!anpr::cameras::gstreamerCompiledIn());
    CHECK(!anpr::cameras::gstElementAvailable("videotestsrc"));
}

#endif
