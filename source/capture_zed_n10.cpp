// Official ZED SDK acquisition: one grab supplies rectified RGB, depth and confidence.
// No OpenCV stereo matching, software colour correction or depth hole filling.
#include <sl/Camera.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <csignal>
#include <chrono>
#include <future>
#include <memory>
#include <array>
#include "camera_transition_lock.h"
#include "live_preview.h"

static volatile std::sig_atomic_t stopping = 0;
static void stop_capture(int) { stopping = 1; }

namespace fs = std::filesystem;
static void check(sl::ERROR_CODE code, const char* operation) {
    if (code != sl::ERROR_CODE::SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + sl::toString(code).c_str());
}
// NPY v1, little-endian float32, row-major. Preserve SDK NaN and +/-Inf verbatim.
static void save_float(const fs::path& path, const sl::Mat& mat) {
    if (mat.getDataType() != sl::MAT_TYPE::F32_C1)
        throw std::runtime_error("Expected SDK F32_C1 measure");
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" +
        std::to_string(mat.getHeight()) + ", " + std::to_string(mat.getWidth()) + "), }";
    header.append((64 - ((10 + header.size() + 1) % 64)) % 64, ' ');
    header += '\n';
    const unsigned char prefix[] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 255),
        static_cast<unsigned char>(header.size() >> 8)};
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    out.write(header.data(), header.size());
    auto* data = reinterpret_cast<const char*>(mat.getPtr<sl::float1>(sl::MEM::CPU));
    for (size_t y = 0; y < mat.getHeight(); ++y)
        out.write(data + y * mat.getStepBytes(sl::MEM::CPU), mat.getWidth() * sizeof(float));
    if (!out) throw std::runtime_error("Writing measure failed: " + path.string());
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 7) {
        std::cerr << "Usage: capture_zed_n10 NEW_OUTPUT_DIR FRAMES WARMUP [SAVE_EVERY [NEURAL|NEURAL_LIGHT [STABILIZATION]]]\n";
        return 2;
    }
    sl::Camera camera;
    fs::path output(argv[1]);
    CameraTransitionLock transition;
    try {
        LivePreview preview(output);
        int count = std::stoi(argv[2]), warmup = std::stoi(argv[3]);
        int save_every = argc >= 5 ? std::stoi(argv[4]) : 1;
        const std::string depth_mode = argc >= 6 ? argv[5] : "NEURAL";
        int stabilization = argc == 7 ? std::stoi(argv[6]) : 30;
        if (stabilization < 0 || stabilization > 100) throw std::runtime_error("Invalid stabilization");
        if (depth_mode != "NEURAL" && depth_mode != "NEURAL_LIGHT") throw std::runtime_error("Invalid depth mode");
        if (save_every < 1) throw std::runtime_error("Invalid save interval");
        std::signal(SIGTERM, stop_capture);
        std::signal(SIGINT, stop_capture);
        if (count < (preview.enabled ? 0 : 1) || count > 10000 || warmup < 30 || warmup > 10000)
            throw std::runtime_error("Invalid frame counts");
        if (!fs::create_directory(output)) throw std::runtime_error("Output already exists");
        sl::InitParameters init;
        init.input.setFromSerialNumber(53004639);
        init.camera_resolution = sl::RESOLUTION::HD1200;
        init.camera_fps = 30;
        init.depth_mode = depth_mode == "NEURAL" ? sl::DEPTH_MODE::NEURAL : sl::DEPTH_MODE::NEURAL_LIGHT;
        init.depth_stabilization = stabilization;
        init.coordinate_units = sl::UNIT::METER;
        init.depth_minimum_distance = 0.2f;
        init.depth_maximum_distance = 4.0f;
        init.sensors_required = false;
        init.open_timeout_sec = 10;
        init.sdk_verbose = 1;
        sl::RuntimeParameters runtime;
        runtime.enable_fill_mode = false;
        runtime.confidence_threshold = 95;
        runtime.texture_confidence_threshold = 100;
        runtime.remove_saturated_areas = false;
        if (!init.save((output/"init.json").string().c_str()) ||
            !runtime.save((output/"runtime.json").string().c_str()))
            throw std::runtime_error("Could not save SDK parameters");
        std::cout << "ZED SDK " << sl::Camera::getSDKVersion() << std::endl;
        check(camera.open(init), "open camera");
        auto info = camera.getCameraInformation();
        const auto calib = info.camera_configuration.calibration_parameters;
        std::ofstream calibration(output/"calibration.json");
        calibration << "{\"fx\":" << calib.left_cam.fx << ",\"fy\":" << calib.left_cam.fy
                    << ",\"cx\":" << calib.left_cam.cx << ",\"cy\":" << calib.left_cam.cy
                    << ",\"baseline_m\":" << calib.getCameraBaseline() << "}\n";
        if (info.serial_number != 53004639 || info.camera_model != sl::MODEL::ZED_XM)
            throw std::runtime_error("Unexpected camera identity");
        check(camera.setCameraSettings(sl::VIDEO_SETTINGS::AEC_AGC, 1), "enable automatic exposure/gain");
        check(camera.setCameraSettings(sl::VIDEO_SETTINGS::WHITEBALANCE_AUTO, 1), "enable automatic white balance");
        sl::RuntimeParameters warmup_runtime = runtime;
        warmup_runtime.enable_depth = false;
        for (int i = 0; i < warmup; ++i) check(camera.grab(warmup_runtime), "warmup grab");
        transition.unlock();
        std::ofstream frames;
        if (!preview.enabled) {
            frames.open(output/"frames.csv");
            frames << "index,timestamp_ns,width,height,exposure_us,analog_gain_mdb,whitebalance_k,host_monotonic_ns,saved\n";
        }
        sl::Mat left, right, depth, confidence, depth_view;
        sl::Mat preview_rgb, preview_depth;
        const sl::Resolution preview_size(preview.width, preview.height(1920,1200));
        std::future<void> writer;
        unsigned long long acquired = 0;
        for (unsigned long long i = 0; (preview.enabled || i < static_cast<unsigned>(count)) && !stopping; ++i) {
            check(camera.grab(runtime), "grab RGB-D");
            const auto timestamp = camera.getTimestamp(sl::TIME_REFERENCE::IMAGE).getNanoseconds();
            const auto host = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (preview.due()) {
                check(camera.retrieveImage(preview_rgb, sl::VIEW::LEFT, sl::MEM::CPU, preview_size), "retrieve preview RGB");
                check(camera.retrieveImage(preview_depth, sl::VIEW::DEPTH, sl::MEM::CPU, preview_size), "retrieve preview depth");
                check(preview_rgb.write((output/"preview-rgb.jpg").string().c_str()), "encode preview RGB");
                check(preview_depth.write((output/"preview-depth.jpg").string().c_str()), "encode preview depth");
                preview.publish({output/"preview-rgb.jpg", output/"preview-depth.jpg"}, timestamp, host, i+1);
            }
            const bool saved = !preview.enabled && i % save_every == 0;
            if (saved) {
            if (writer.valid()) writer.get();
            check(camera.retrieveImage(left, sl::VIEW::LEFT), "retrieve rectified left");
            check(camera.retrieveImage(right, sl::VIEW::RIGHT), "retrieve rectified right");
            check(camera.retrieveMeasure(depth, sl::MEASURE::DEPTH), "retrieve metric depth");
            check(camera.retrieveMeasure(confidence, sl::MEASURE::CONFIDENCE), "retrieve confidence");
            check(camera.retrieveImage(depth_view, sl::VIEW::DEPTH), "retrieve SDK depth display");
            std::string stem = std::to_string(i);
            // Bounded writer: next sampled frame waits if storage falls behind.
            // Full-rate SDK grabbing continues while the previous sample saves.
            auto images = std::make_shared<std::array<sl::Mat,5>>();
            check(left.copyTo((*images)[0]), "copy left");
            check(right.copyTo((*images)[1]), "copy right");
            check(depth_view.copyTo((*images)[2]), "copy depth display");
            check(depth.copyTo((*images)[3]), "copy metric depth");
            check(confidence.copyTo((*images)[4]), "copy confidence");
            writer = std::async(std::launch::async, [images, output, stem]() {
            check((*images)[0].write((output/(stem+"-left.png")).string().c_str()), "write left PNG");
            check((*images)[1].write((output/(stem+"-right.png")).string().c_str()), "write right PNG");
            check((*images)[2].write((output/(stem+"-sdk-depth-view.png")).string().c_str()), "write SDK depth display");
            save_float(output/(stem+"-depth-m.npy"), (*images)[3]);
            save_float(output/(stem+"-confidence.npy"), (*images)[4]);
            check((*images)[0].write((output/"latest-tmp.jpg").string().c_str()), "write live preview");
            fs::rename(output/"latest-tmp.jpg", output/"latest.jpg");
            check((*images)[2].write((output/"latest-depth-tmp.jpg").string().c_str()), "write live depth preview");
            fs::rename(output/"latest-depth-tmp.jpg", output/"latest-depth.jpg");
            });
            }
            if (!preview.enabled) {
            int exposure = 0, gain = 0, whitebalance = 0;
            check(camera.getCameraSettings(sl::VIDEO_SETTINGS::EXPOSURE_TIME, exposure), "read exposure");
            check(camera.getCameraSettings(sl::VIDEO_SETTINGS::ANALOG_GAIN, gain), "read analog gain");
            check(camera.getCameraSettings(sl::VIDEO_SETTINGS::WHITEBALANCE_TEMPERATURE, whitebalance), "read white balance");
            frames << i << ',' << timestamp << ',' << info.camera_configuration.resolution.width << ',' << info.camera_configuration.resolution.height
                   << ',' << exposure << ',' << gain << ',' << whitebalance << ',' << host << ',' << saved << '\n';
            frames.flush();
            if (!frames) throw std::runtime_error("Cannot write frame metadata");
            }
            ++acquired;
            if (i == 0) { std::ofstream ready(output/"ready"); ready << timestamp << '\n'; }
        }
        if (writer.valid()) writer.get();
        transition.lock();
        camera.close();
        transition.unlock();
        std::ofstream status(output/"complete.json");
        status << "{\"backend\":\"zed-sdk\",\"sdk_version\":\"" << sl::Camera::getSDKVersion()
               << "\",\"serial\":53004639,\"model\":\"ZED X Mini\",\"frames\":" << acquired
               << ",\"depth_unit\":\"metre\",\"depth_mode\":\"" << depth_mode << "\"}\n";
        if (!status) throw std::runtime_error("Cannot write completion metadata");
        return 0;
    } catch (const std::exception& error) {
        transition.lock();
        camera.close();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
