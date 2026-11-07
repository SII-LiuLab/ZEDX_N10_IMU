/* Official ZED depth check from an existing SVO; no live camera is opened. */
#include <sl/Camera.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static void check(sl::ERROR_CODE status, const char* operation) {
    if (status != sl::ERROR_CODE::SUCCESS)
        throw std::runtime_error(std::string(operation)+": "+sl::toString(status).c_str());
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: analyze_zed_svo_n10 INPUT.svo2 NEW_OUTPUT_DIR\n";
        return 2;
    }
    sl::Camera camera;
    try {
        fs::path output(argv[2]);
        if (!fs::create_directories(output))
            throw std::runtime_error("Output directory already exists");
        sl::InitParameters init;
        init.input.setFromSVOFile(argv[1]);
        init.svo_real_time_mode = false;
        init.depth_mode = sl::DEPTH_MODE::NEURAL;
        init.depth_stabilization = 0;
        init.coordinate_units = sl::UNIT::METER;
        init.depth_minimum_distance = .2f;
        init.depth_maximum_distance = 4.f;
        check(camera.open(init), "open SVO");
        auto calibration = camera.getCameraInformation().camera_configuration.calibration_parameters;
        std::ofstream metrics(output/"metrics.csv");
        metrics << "sdk,frame,valid_fraction,fx,baseline\n";
        sl::RuntimeParameters runtime;
        runtime.confidence_threshold = 95;
        runtime.texture_confidence_threshold = 100;
        runtime.enable_fill_mode = false;
        runtime.remove_saturated_areas = false;
        sl::Mat depth, left, right, view;
        for (int index = 0; index < 120; ++index) {
            check(camera.grab(runtime), "grab SVO");
            if (index % 30) continue;
            check(camera.retrieveMeasure(depth, sl::MEASURE::DEPTH), "retrieve depth");
            check(camera.retrieveImage(left, sl::VIEW::LEFT), "retrieve left");
            check(camera.retrieveImage(right, sl::VIEW::RIGHT), "retrieve right");
            check(camera.retrieveImage(view, sl::VIEW::DEPTH), "retrieve depth view");
            unsigned valid = 0;
            for (unsigned y = 0; y < depth.getHeight(); ++y) {
                for (unsigned x = 0; x < depth.getWidth(); ++x) {
                    float value;
                    check(depth.getValue(x, y, &value), "read depth value");
                    valid += std::isfinite(value) && value > 0;
                }
            }
            metrics << camera.getSDKVersion() << ',' << index << ','
                    << double(valid)/(depth.getWidth()*depth.getHeight()) << ','
                    << calibration.left_cam.fx << ',' << calibration.getCameraBaseline() << '\n';
            metrics.flush();
            if (!metrics) throw std::runtime_error("Cannot write SVO metrics");
            const auto stem = std::to_string(index);
            check(left.write((output/(stem+"-left.png")).c_str()), "write left");
            check(right.write((output/(stem+"-right.png")).c_str()), "write right");
            check(view.write((output/(stem+"-view.png")).c_str()), "write depth view");
        }
        camera.close();
        return 0;
    } catch (const std::exception& error) {
        camera.close();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
