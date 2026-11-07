#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

// One atomic snapshot per camera, on tmpfs. Readers never block acquisition.
// RGB and depth from one ZED grab are published together in the same snapshot.
class LivePreview {
    using Clock = std::chrono::steady_clock;
    std::filesystem::path output;
    Clock::time_point next = Clock::now();
    long long first_host = 0, first_publish = 0;
    unsigned long long published = 0;
public:
    bool enabled;
    int width = 960, fps = 10;
    explicit LivePreview(const std::filesystem::path& folder) : output(folder),
        enabled(std::getenv("N10_LIVE") != nullptr) {
        if (const char* value = std::getenv("N10_PREVIEW_WIDTH")) width = std::stoi(value);
        if (const char* value = std::getenv("N10_PREVIEW_FPS")) fps = std::stoi(value);
        if (width < 320 || width > 1920 || width % 2 || fps < 1 || fps > 30)
            throw std::runtime_error("Invalid live preview size/rate");
    }
    static long long now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
    bool due() {
        const auto now = Clock::now();
        if (!enabled || now < next) return false;
        const auto period = std::chrono::nanoseconds(1000000000LL / fps);
        next += period;
        if (next < now) next = now + period;
        return true;
    }
    int height(int native_width, int native_height) const {
        return (width * native_height / native_width / 2) * 2;
    }
    void publish(const std::vector<std::filesystem::path>& images,
                 unsigned long long timestamp, long long host, unsigned long long acquired) {
        std::vector<std::string> payloads;
        for (const auto& image : images) {
            std::ifstream file(image, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot read preview JPEG");
            payloads.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        publish_bytes(payloads, timestamp, host, acquired);
    }
    void publish_bytes(const std::vector<std::string>& payloads,
                       unsigned long long timestamp, long long host, unsigned long long acquired) {
        const auto now = now_ns();
        if (!first_host) first_host = host;
        if (!first_publish) first_publish = now;
        ++published;
        std::ofstream file(output / "preview-tmp.bin", std::ios::binary);
        file << "{\"sequence\":" << published << ",\"timestamp_ns\":" << timestamp
             << ",\"host_monotonic_ns\":" << host << ",\"published_monotonic_ns\":" << now
             << ",\"frames_received\":" << acquired
             << ",\"capture_fps\":" << (host > first_host ? (acquired-1)*1e9/(host-first_host) : 0)
             << ",\"preview_fps\":" << (now > first_publish ? (published-1)*1e9/(now-first_publish) : 0)
             << ",\"sizes\":[";
        for (size_t i = 0; i < payloads.size(); ++i) file << (i ? "," : "") << payloads[i].size();
        file << "]}\n";
        for (const auto& data : payloads) file.write(data.data(), data.size());
        file.close();
        if (!file) throw std::runtime_error("Cannot publish live frame");
        std::filesystem::rename(output / "preview-tmp.bin", output / "preview.bin");
    }
};
