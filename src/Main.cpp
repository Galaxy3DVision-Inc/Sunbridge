#include "Bridge.h"
#include "IpcManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define BRIDGE_EXPORT __declspec(dllexport)
#else
#define BRIDGE_EXPORT __attribute__((visibility("default")))
#endif

namespace {

struct OutboundMessage {
    std::string method;
    std::vector<std::uint8_t> bytes;
};

SunshineCallTable* g_host = nullptr;
std::unique_ptr<principia::ipc::IpcManager> g_commands;
std::unique_ptr<principia::ipc::IpcManager> g_frames;
std::atomic<bool> g_running{false};
std::mutex g_queueMutex;
std::condition_variable g_queueReady;
std::deque<OutboundMessage> g_queue;
std::thread g_sender;
std::atomic<bool> g_loggedFirstVideo{false};
std::atomic<bool> g_loggedFirstVideoSend{false};
std::atomic<bool> g_waitingForIdr{false};
std::atomic<std::uint64_t> g_idrRequests{0};
std::atomic<std::uint64_t> g_idrFrames{0};
std::atomic<std::uint64_t> g_deltasDiscarded{0};

struct H264AccessUnitInfo {
    bool hasAnnexBStartCode = false;
    bool hasIdr = false;
    bool hasSps = false;
    bool hasPps = false;
    std::string profileLevelId;
};

char HexDigit(std::uint8_t value) {
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

H264AccessUnitInfo InspectH264AccessUnit(const std::uint8_t* data, std::size_t size) {
    H264AccessUnitInfo info;
    if (!data || size < 4) return info;

    for (std::size_t i = 0; i + 3 < size;) {
        std::size_t prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            prefix = 3;
        } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            prefix = 4;
        }
        if (!prefix) {
            ++i;
            continue;
        }

        info.hasAnnexBStartCode = true;
        const std::size_t nalOffset = i + prefix;
        if (nalOffset < size) {
            switch (data[nalOffset] & 0x1f) {
            case 5: info.hasIdr = true; break;
            case 7:
                info.hasSps = true;
                if (nalOffset + 3 < size) {
                    info.profileLevelId.reserve(6);
                    for (std::size_t byte = nalOffset + 1; byte <= nalOffset + 3; ++byte) {
                        info.profileLevelId.push_back(HexDigit(data[byte] >> 4));
                        info.profileLevelId.push_back(HexDigit(data[byte] & 0x0f));
                    }
                }
                break;
            case 8: info.hasPps = true; break;
            default: break;
            }
        }
        i = nalOffset + 1;
    }
    return info;
}

void BridgeLog(const std::string& message) {
    std::ofstream output("sunbridge.log", std::ios::app | std::ios::binary);
    output << message << '\n';
}

void QueueMessage(std::string method, std::vector<std::uint8_t> bytes, std::size_t limit) {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        while (g_queue.size() >= limit) g_queue.pop_front();
        g_queue.push_back({std::move(method), std::move(bytes)});
    }
    g_queueReady.notify_one();
}

void QueueVideoFrame(std::vector<std::uint8_t> bytes, bool isIdr) {
    bool requestIdr = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (isIdr) {
            // Never leave a recovery keyframe behind older delta frames. A PLI
            // or periodic IDR must become the next video message delivered to
            // CantorFiber so Chromium can immediately rebuild its decoder.
            std::erase_if(g_queue, [](const OutboundMessage& queued) {
                return queued.method == "Rtc/VideoFrame";
            });
            g_waitingForIdr = false;
        } else {
            if (g_waitingForIdr) {
                ++g_deltasDiscarded;
                return;
            }
            std::size_t videoCount = 0;
            for (const auto& queued : g_queue) {
                if (queued.method == "Rtc/VideoFrame") ++videoCount;
            }
            if (videoCount >= 4) {
                // H.264 deltas form a dependency chain. Dropping one queued
                // delta and sending later deltas corrupts the decoder. Drop
                // the entire chain and wait for a fresh IDR instead.
                std::erase_if(g_queue, [](const OutboundMessage& queued) {
                    return queued.method == "Rtc/VideoFrame";
                });
                g_waitingForIdr = true;
                requestIdr = true;
            } else {
                g_queue.push_back({"Rtc/VideoFrame", std::move(bytes)});
            }
        }
        if (isIdr) g_queue.push_back({"Rtc/VideoFrame", std::move(bytes)});
    }
    if (requestIdr && g_host && g_host->RequestIdr) {
        const auto request = ++g_idrRequests;
        BridgeLog("Video IPC backlog: discarded delta chain and requested IDR #" +
            std::to_string(request));
        g_host->RequestIdr();
    }
    g_queueReady.notify_one();
}

void SenderLoop() {
    while (g_running) {
        OutboundMessage message;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueReady.wait(lock, [] { return !g_running || !g_queue.empty(); });
            if (!g_running && g_queue.empty()) return;
            message = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (g_frames) {
            const bool sent = g_frames->PushMessage(message.method,
                Cas::Value(message.bytes.empty() ? nullptr : message.bytes.data(),
                           message.bytes.size()));
            if (message.method == "Rtc/VideoFrame" && !g_loggedFirstVideoSend.exchange(true)) {
                BridgeLog(std::string("First video IPC send ") + (sent ? "succeeded" : "failed") +
                    " bytes=" + std::to_string(message.bytes.size()));
            }
            if (message.method == "Rtc/VideoFrame" && !sent) {
                {
                    std::lock_guard<std::mutex> lock(g_queueMutex);
                    std::erase_if(g_queue, [](const OutboundMessage& queued) {
                        return queued.method == "Rtc/VideoFrame";
                    });
                    g_waitingForIdr = true;
                }
                if (g_host && g_host->RequestIdr) {
                    const auto request = ++g_idrRequests;
                    BridgeLog("Video IPC send failed; requested recovery IDR #" +
                        std::to_string(request));
                    g_host->RequestIdr();
                }
            }
        }
    }
}

Cas::Value HandleCommand(const std::string& method, const Cas::Value& payload) {
    if (!g_host) return Cas::Value("HOST_UNAVAILABLE");
    if (method == "Start") {
        try {
            const auto config = nlohmann::json::parse(payload.GetString());
            const auto display = config.value("display", "");
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                std::erase_if(g_queue, [](const OutboundMessage& queued) {
                    return queued.method == "Rtc/VideoFrame";
                });
                g_waitingForIdr = true;
            }
            g_idrRequests = 0;
            g_idrFrames = 0;
            g_deltasDiscarded = 0;
            const int fps = config.value("fps", 60);
            const int bitrateKbps = config.value("bitrate_kbps", 10000);
            const auto rtcConfig = nlohmann::json{
                {"max_bitrate_bps", bitrateKbps * 1000},
                {"max_framerate", fps}
            }.dump();
            QueueMessage("Rtc/VideoConfig",
                std::vector<std::uint8_t>(rtcConfig.begin(), rtcConfig.end()), 16);
            const bool videoStarted = g_host->StartVideo && g_host->StartVideo(
                display.c_str(), config.value("width", 1920), config.value("height", 1080),
                fps, bitrateKbps);
            if (videoStarted && g_host->RequestIdr) {
                ++g_idrRequests;
                g_host->RequestIdr();
            }
            BridgeLog(std::string("Start video display=") + display +
                (videoStarted ? " succeeded" : " failed"));
            if (config.value("audio", true) && g_host->StartAudio) g_host->StartAudio("");
            return Cas::Value(videoStarted ? "OK" : "VIDEO_START_FAILED");
        } catch (...) {
            return Cas::Value("INVALID_START_CONFIG");
        }
    }
    if (method == "Stop") {
        if (g_host->StopAudio) g_host->StopAudio();
        if (g_host->StopVideo) g_host->StopVideo();
        return Cas::Value("OK");
    }
    if (method == "RequestIdr") {
        if (g_host->RequestIdr) {
            const auto request = ++g_idrRequests;
            BridgeLog("CantorFiber requested IDR #" + std::to_string(request));
            g_host->RequestIdr();
        }
        return Cas::Value("OK");
    }
    if (method == "Input" && payload.IsBin()) {
        const auto bytes = payload.GetBin();
        if (!bytes || !g_host->InjectInput) return Cas::Value("INPUT_UNAVAILABLE");
        return Cas::Value(g_host->InjectInput(bytes->data(), static_cast<int>(bytes->size())) == 0
            ? "OK" : "INPUT_FAILED");
    }
    return Cas::Value("UNSUPPORTED_COMMAND");
}

} // namespace

extern "C" BRIDGE_EXPORT int LoadBridge(void* hostCallTable, const char*, int) {
    const char* ipcIn = std::getenv("CANTORFIBER_INSTANCE_IN");
    const char* ipcOut = std::getenv("CANTORFIBER_INSTANCE_OUT");
    BridgeLog(std::string("LoadBridge host=") + (hostCallTable ? "yes" : "no") +
        " ipcIn=" + (ipcIn && *ipcIn ? ipcIn : "<missing>") +
        " ipcOut=" + (ipcOut && *ipcOut ? ipcOut : "<missing>"));
    if (!hostCallTable || !ipcIn || !*ipcIn || !ipcOut || !*ipcOut) return 2;

    g_host = static_cast<SunshineCallTable*>(hostCallTable);
    g_commands = std::make_unique<principia::ipc::IpcManager>();
    g_frames = std::make_unique<principia::ipc::IpcManager>();
    g_frames->m_disableAsyncReads = true;
    g_commands->SetMessageCallback(HandleCommand);
    const bool commandsStarted = g_commands->StartClient(ipcIn);
    const bool framesStarted = g_frames->StartClient(ipcOut);
    BridgeLog(std::string("IPC clients commands=") + (commandsStarted ? "started" : "failed") +
        " frames=" + (framesStarted ? "started" : "failed"));
    if (!commandsStarted || !framesStarted) return 3;

    g_host->OnVideoFrame = [](const std::uint8_t* data, int size, bool isIdr, std::int64_t frameIndex) {
        if (!data || size <= 0) return;
        const auto nal = InspectH264AccessUnit(data, static_cast<std::size_t>(size));
        const bool actualIdr = nal.hasAnnexBStartCode ? nal.hasIdr : isIdr;
        if (!g_loggedFirstVideo.exchange(true)) {
            BridgeLog("First encoded video callback bytes=" + std::to_string(size) +
                " sunshine_idr=" + (isIdr ? std::string("yes") : std::string("no")) +
                " nal_idr=" + (nal.hasIdr ? std::string("yes") : std::string("no")) +
                " annex_b=" + (nal.hasAnnexBStartCode ? std::string("yes") : std::string("no")));
        }
        if (actualIdr) {
            const auto idr = ++g_idrFrames;
            BridgeLog("H264 IDR #" + std::to_string(idr) +
                " frame=" + std::to_string(frameIndex) +
                " bytes=" + std::to_string(size) +
                " sps=" + (nal.hasSps ? std::string("yes") : std::string("no")) +
                " pps=" + (nal.hasPps ? std::string("yes") : std::string("no")) +
                " profile_level_id=" + (nal.profileLevelId.empty() ? std::string("unknown") : nal.profileLevelId) +
                " discarded_deltas=" + std::to_string(g_deltasDiscarded.load()));
        }
        std::vector<std::uint8_t> message(static_cast<std::size_t>(size) + 1);
        message[0] = actualIdr ? 1 : 0;
        std::memcpy(message.data() + 1, data, static_cast<std::size_t>(size));
        QueueVideoFrame(std::move(message), actualIdr);
    };
    g_host->OnAudioPacket = [](const std::uint8_t* data, int size, std::int64_t pts) {
        if (!data || size <= 0) return;
        std::vector<std::uint8_t> message(sizeof(pts) + static_cast<std::size_t>(size));
        std::memcpy(message.data(), &pts, sizeof(pts));
        std::memcpy(message.data() + sizeof(pts), data, static_cast<std::size_t>(size));
        QueueMessage("AudioFrame", std::move(message), 64);
    };

    g_running = true;
    g_sender = std::thread(SenderLoop);
    bool readySent = false;
    for (int attempt = 0; attempt < 50 && g_running; ++attempt) {
        if (g_commands->IsConnected() && g_frames->IsConnected() &&
            g_frames->PushMessage("Ready", Cas::Value(""))) {
            readySent = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BridgeLog(std::string("Ready handshake ") + (readySent ? "sent" : "timed out"));
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}

extern "C" BRIDGE_EXPORT void UnloadBridge() {
    g_running = false;
    g_queueReady.notify_all();
    if (g_sender.joinable()) g_sender.join();
    if (g_commands) g_commands->Stop();
    if (g_frames) g_frames->Stop();
    g_commands.reset();
    g_frames.reset();
    g_host = nullptr;
}
