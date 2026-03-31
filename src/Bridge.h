#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    // ==========================================
    // Outbound (XLang Context -> Sunshine Host)
    // ==========================================

    /// Tell Sunshine to start the video capture and encoding thread
    typedef bool (*f_StartVideo)(const char* display, int width, int height, int fps, int bitrate);
    
    /// Halt the video capture thread dynamically without killing the host
    typedef void (*f_StopVideo)();

    /// Tell Sunshine to start the audio capture and encoding thread
    typedef bool (*f_StartAudio)(const char* audioSink);
    
    /// Halt the audio capture thread dynamically without killing the host
    typedef void (*f_StopAudio)();
    
    /// Completely halt the entire Sunshine background processor
    typedef void (*f_StopProcessing)();
    
    /// Inject a packet of gamepad/mouse/keyboard Input strictly over the local API
    typedef int (*f_InjectInput)(const uint8_t* pEventData, int cbSize);

    // ==========================================
    // Inbound (Sunshine Host -> XLang Context)
    // ==========================================

    /// Callback whenever a new encoded Video NAL Frame is ready
    typedef void (*f_OnVideoFrame)(const uint8_t* data, int size, bool isIdr, int64_t frameIndex);
    
    typedef void (*f_OnAudioPacket)(const uint8_t* data, int size, int64_t pts);
    
    /// Callback from XLang to request a new IDR/SPS slice immediately
    typedef void (*f_RequestIdr)();

#ifdef __cplusplus
}
#endif

// The cross-abi function table passed securely into LoadBridge()
struct SunshineCallTable
{
    // Outbound hooks provided by Sunshine
    f_StartVideo StartVideo;
    f_StopVideo StopVideo;
    f_StartAudio StartAudio;
    f_StopAudio StopAudio;
    f_StopProcessing StopProcessing;
    f_InjectInput InjectInput;

    // Inbound listener hooks provided by the DLL plugin to Sunshine
    f_OnVideoFrame OnVideoFrame;
    f_OnAudioPacket OnAudioPacket;
    f_RequestIdr RequestIdr;
};
