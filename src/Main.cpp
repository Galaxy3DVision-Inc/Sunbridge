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
std::unique_ptr<principia::ipc::IpcManager> g_ipc;
std::atomic<bool> g_running{false};
std::mutex g_queueMutex;
std::condition_variable g_queueReady;
std::deque<OutboundMessage> g_queue;
std::thread g_sender;
std::atomic<bool> g_loggedFirstVideo{false};
std::atomic<bool> g_loggedFirstVideoSend{false};
std::atomic<bool> g_readySent{false};
std::atomic<bool> g_waitingForIdr{true};
std::atomic<std::uint64_t> g_idrRequests{0};
std::atomic<std::uint64_t> g_idrFrames{0};
std::atomic<std::uint64_t> g_deltasDiscarded{0};
std::chrono::steady_clock::time_point g_lastVideoStop{};
std::chrono::steady_clock::time_point g_bridgeLoadedAt{};

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
    bool waitingForIdrRequest = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (isIdr) {
            // Only a recovery IDR supersedes the damaged delta chain. Normal
            // periodic IDRs remain in capture order; removing valid deltas in
            // front of them creates an unreported visual jump at every GOP.
            if (g_waitingForIdr) {
                std::erase_if(g_queue, [](const OutboundMessage& queued) {
                    return queued.method == "Rtc/VideoFrame";
                });
            }
            g_waitingForIdr = false;
        } else {
            if (g_waitingForIdr) {
                const auto discarded = ++g_deltasDiscarded;
                // Some encoders acknowledge a keyframe request before they
                // actually emit an IDR. Keep asking at a bounded cadence and
                // never expose undecodable deltas to the WebRTC receiver.
                requestIdr = discarded == 1 || discarded % 30 == 0;
                waitingForIdrRequest = requestIdr;
            } else {
                std::size_t videoCount = 0;
                for (const auto& queued : g_queue) {
                    if (queued.method == "Rtc/VideoFrame") ++videoCount;
                }
                if (videoCount >= 16) {
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
        }
        if (isIdr) g_queue.push_back({"Rtc/VideoFrame", std::move(bytes)});
    }
    if (requestIdr && g_host && g_host->RequestIdr) {
        const auto request = ++g_idrRequests;
        BridgeLog(std::string(waitingForIdrRequest
            ? "Waiting for IDR: discarded deltas and requested IDR #"
            : "Video IPC backlog: discarded delta chain and requested IDR #") +
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
        if (g_ipc) {
            const bool video = message.method == "Rtc/VideoFrame";
            const bool keyFrame = video && !message.bytes.empty() && message.bytes[0] != 0;
            bool sent = false;
            int retries = 0;
            do {
                sent = g_ipc->PushMessage(message.method,
                    Cas::Value(message.bytes.empty() ? nullptr : message.bytes.data(),
                               message.bytes.size()),
                    video ? 50 : 100);
                if (sent || !video || !g_running) break;
                // Preserve the encoded frame while CantorFiber drains a short
                // IPC burst. If the producer has already abandoned this delta
                // chain, do not reintroduce an obsolete delta before its IDR.
                if (!keyFrame && g_waitingForIdr) break;
                if (++retries >= 40) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (true);
            if (video && sent && retries > 0) {
                BridgeLog("Video IPC backpressure recovered after " +
                    std::to_string(retries) + " retries");
            }
            if (message.method == "Rtc/VideoFrame" && !g_loggedFirstVideoSend.exchange(true)) {
                BridgeLog(std::string("First video IPC send ") + (sent ? "succeeded" : "failed") +
                    " bytes=" + std::to_string(message.bytes.size()));
            }
            if (video && !sent) {
                bool requestIdr = false;
                {
                    std::lock_guard<std::mutex> lock(g_queueMutex);
                    std::erase_if(g_queue, [](const OutboundMessage& queued) {
                        return queued.method == "Rtc/VideoFrame";
                    });
                    requestIdr = !g_waitingForIdr.exchange(true);
                }
                if (requestIdr && g_host && g_host->RequestIdr) {
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
            // Sunshine's macOS capture teardown completes just after
            // StopVideo returns. Starting a replacement encoder immediately
            // can inherit the retiring display resources and produce no
            // frames. Bound that handoff before reusing the warm process.
            constexpr auto kCaptureRestartDelay = std::chrono::milliseconds(500);
            constexpr auto kInitialCaptureDelay = std::chrono::seconds(1);
            const auto now = std::chrono::steady_clock::now();
            auto readyAt = g_bridgeLoadedAt + kInitialCaptureDelay;
            if (g_lastVideoStop.time_since_epoch().count() != 0) {
                readyAt = (std::max)(readyAt, g_lastVideoStop + kCaptureRestartDelay);
            }
            if (now < readyAt) std::this_thread::sleep_for(readyAt - now);
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
                // Leave measured room for RTP overhead while keeping large
                // IDRs inside the WebRTC pacer. A 4x budget let each keyframe
                // burst at roughly 40 Mbps on a 10 Mbps stream, which caused
                // Wi-Fi packet loss and a lasting congestion-control collapse.
                {"max_bitrate_bps", bitrateKbps * 1250},
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
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            std::erase_if(g_queue, [](const OutboundMessage& queued) {
                return queued.method == "Rtc/VideoFrame";
            });
            g_waitingForIdr = true;
        }
        if (g_host->StopAudio) g_host->StopAudio();
        if (g_host->StopVideo) g_host->StopVideo();
        g_lastVideoStop = std::chrono::steady_clock::now();
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
    if (!hostCallTable || !ipcIn || !*ipcIn) return 2;

    g_host = static_cast<SunshineCallTable*>(hostCallTable);
    g_bridgeLoadedAt = std::chrono::steady_clock::now();
    g_ipc = std::make_unique<principia::ipc::IpcManager>();
    g_ipc->SetMessageCallback(HandleCommand);
    g_ipc->SetConnectionCallback([](bool online) {
        if (!online) {
            g_readySent = false;
            BridgeLog("Duplex IPC disconnected; waiting for CantorFiber reconnect");
        }
    });
    g_running = true;
    const bool ipcStarted = g_ipc->StartClient(ipcIn);
    BridgeLog(std::string("Duplex IPC client=") + (ipcStarted ? "started" : "failed"));
    if (!ipcStarted) {
        g_running = false;
        return 3;
    }

    g_host->OnVideoFrame = [](const std::uint8_t* data, int size, bool isIdr,
                              std::int64_t frameIndex,
                              std::int64_t captureTimestampUs) {
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
        // Versioned, fixed-width metadata keeps Sunshine's capture timeline
        // intact across the component IPC boundary. CantorFiber maps this
        // monotonic timestamp into WebRTC's clock instead of rebuilding frame
        // timing from bursty IPC arrival times.
        constexpr std::size_t kHeaderSize = 24;
        std::vector<std::uint8_t> message(static_cast<std::size_t>(size) + kHeaderSize, 0);
        message[0] = 'C'; message[1] = 'F'; message[2] = 'V'; message[3] = '1';
        message[4] = actualIdr ? 1 : 0;
        const auto writeI64 = [&message](std::size_t offset, std::int64_t value) {
            const auto bits = static_cast<std::uint64_t>(value);
            for (std::size_t byte = 0; byte < 8; ++byte) {
                message[offset + byte] = static_cast<std::uint8_t>(bits >> (byte * 8));
            }
        };
        writeI64(8, frameIndex);
        writeI64(16, captureTimestampUs);
        std::memcpy(message.data() + kHeaderSize, data, static_cast<std::size_t>(size));
        QueueVideoFrame(std::move(message), actualIdr);
    };
    g_host->OnAudioPacket = [](const std::uint8_t* data, int size, std::int64_t pts) {
        if (!data || size <= 0) return;
        std::vector<std::uint8_t> message(sizeof(pts) + static_cast<std::size_t>(size));
        std::memcpy(message.data(), &pts, sizeof(pts));
        std::memcpy(message.data() + sizeof(pts), data, static_cast<std::size_t>(size));
        QueueMessage("AudioFrame", std::move(message), 64);
    };

    g_sender = std::thread(SenderLoop);
    bool loggedInitialWait = false;
    const auto initialReadyDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (g_running) {
        if (!g_readySent && g_ipc->IsConnected() &&
            g_ipc->PushMessage("Ready", Cas::Value(""))) {
            g_readySent = true;
            BridgeLog("Ready handshake sent");
        }
        if (!g_readySent && !loggedInitialWait &&
            std::chrono::steady_clock::now() >= initialReadyDeadline) {
            loggedInitialWait = true;
            BridgeLog("Ready handshake still waiting for CantorFiber");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return 0;
}

extern "C" BRIDGE_EXPORT void UnloadBridge() {
    g_running = false;
    g_queueReady.notify_all();
    if (g_sender.joinable()) g_sender.join();
    if (g_ipc) g_ipc->Stop();
    g_ipc.reset();
    g_host = nullptr;
}
