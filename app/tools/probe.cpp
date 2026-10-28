// Headless probe / recorder for the Glint payload.
// Usage:
//   probe http://<ps5-ip>:7231
//   probe http://<ps5-ip>:7231 --record out.h264 --seconds 5
//   probe http://<ps5-ip>:7231 --record out.h264 --audio out.pcm --seconds 5
//   probe http://<ps5-ip>:7231 --stop
#include "glint/payload_client.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

void print_config(const glint::StreamConfig& config) {
    std::printf("platform=%s %dx%d@%d codec=%s\n", config.platform.c_str(),
                config.width, config.height, config.fps, config.codec.c_str());
    std::printf("sps=%s\npps=%s\n", config.sps.c_str(), config.pps.c_str());
    std::printf("resolutions=");
    for (size_t i = 0; i < config.resolutions.size(); i++)
        std::printf("%s%d", i ? "," : "", config.resolutions[i]);
    std::printf(" requested=%d changing=%d error=%d\n", config.requested_height,
                config.profile_changing ? 1 : 0, config.profile_error ? 1 : 0);
    if (config.has_audio)
        std::printf("audio=%s %dch %dHz %dbit\n", config.audio_codec.c_str(),
                    config.audio_channels, config.audio_rate, config.audio_bits);
    else
        std::printf("audio=none\n");
}

void print_status(const glint::StreamStatus& status) {
    std::printf("status backend=%s %dx%d frames=%llu bytes=%llu fps=%.1f kbps=%.1f clients=%d\n",
                status.backend.c_str(), status.width, status.height,
                status.frames, status.bytes, status.fps, status.kbps, status.clients);
    std::printf("record=%d name=%s frames=%llu bytes=%llu seconds=%.1f error=%d\n",
                status.recording ? 1 : 0, status.recording_name.c_str(),
                status.recording_frames, status.recording_bytes,
                status.recording_seconds, status.recording_error ? 1 : 0);
    std::printf("audio chunks=%llu bytes=%llu kbps=%.1f codec=%s\n",
                status.audio_chunks, status.audio_bytes, status.audio_kbps,
                status.audio_codec.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s BASE_URL [--record FILE] [--audio FILE] [--seconds N] [--stop]\n",
                     argv[0]);
        return 2;
    }

    std::string base = argv[1];
    std::string record_path;
    std::string audio_path;
    int seconds = 5;
    bool stop_after = false;

    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) record_path = argv[++i];
        else if (std::strcmp(argv[i], "--audio") == 0 && i + 1 < argc) audio_path = argv[++i];
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--stop") == 0) stop_after = true;
    }

    glint::PayloadClient client(base);
    std::string error;

    glint::StreamConfig config;
    if (!client.fetch_config(config, error)) {
        std::fprintf(stderr, "config: %s\n", error.c_str());
        return 1;
    }
    print_config(config);

    glint::StreamStatus status;
    if (client.fetch_status(status, error)) print_status(status);
    else std::fprintf(stderr, "status: %s\n", error.c_str());

    if (!record_path.empty() || !audio_path.empty()) {
        std::atomic_bool stop{false};
        std::thread timer;
        std::thread audio_thread;

        FILE* video = nullptr;
        FILE* audio = nullptr;
        if (!record_path.empty()) {
            video = std::fopen(record_path.c_str(), "wb");
            if (!video) {
                std::fprintf(stderr, "cannot open %s\n", record_path.c_str());
                return 1;
            }
        }
        if (!audio_path.empty()) {
            audio = std::fopen(audio_path.c_str(), "wb");
            if (!audio) {
                std::fprintf(stderr, "cannot open %s\n", audio_path.c_str());
                if (video) std::fclose(video);
                return 1;
            }
            audio_thread = std::thread([&] {
                std::string audio_error;
                glint::PayloadClient audio_client(base);
                if (!audio_client.stream_audio(
                        [&](const uint8_t* pcm, size_t len, uint32_t) {
                            if (audio) std::fwrite(pcm, 1, len, audio);
                            return !stop.load();
                        },
                        stop, audio_error))
                    std::fprintf(stderr, "audio: %s\n", audio_error.c_str());
            });
        }

        if (seconds > 0) {
            timer = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                stop.store(true);
            });
        }

        if (video) {
            std::string key_error;
            client.post("/video/keyframe", nullptr, key_error);

            std::string stream_error;
            unsigned long long bytes = 0;
            bool ok = client.stream_frames(
                [&](const uint8_t* annexb, size_t len, uint32_t) {
                    std::fwrite(annexb, 1, len, video);
                    bytes += len;
                    return !stop.load();
                },
                stop, stream_error);
            if (!ok && !stop.load())
                std::fprintf(stderr, "video: %s\n", stream_error.c_str());
            std::printf("recorded %llu bytes to %s\n", bytes, record_path.c_str());
            std::fclose(video);
        } else {
            while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        stop.store(true);
        if (timer.joinable()) timer.join();
        if (audio_thread.joinable()) audio_thread.join();
        if (audio) std::fclose(audio);
    }

    if (stop_after) {
        if (client.post("/stop", nullptr, error)) std::printf("stopped\n");
        else std::fprintf(stderr, "stop: %s\n", error.c_str());
    }
    return 0;
}

