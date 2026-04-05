#include "xhost.h"
#include "xpackage.h"
#include "Bridge.h"
#include "xload.h"
#include "xlang.h"
#include <iostream>
#include <string>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>

#if (WIN32)
#include <windows.h>
#define BRIDGE_EXPORT __declspec(dllexport) 
#else
#define BRIDGE_EXPORT
#endif

// Global State
X::XLoad g_xload;
X::Config g_config;

struct VideoFrameData {
    std::vector<uint8_t> data;
    bool isIdr;
    int64_t frameIndex;
};

struct AudioPacketData {
    std::vector<uint8_t> data;
    int64_t pts;
};

std::mutex g_vidQMutex;
std::condition_variable g_vidQCv;
std::queue<VideoFrameData> g_vidQ;

std::mutex g_audQMutex;
std::condition_variable g_audQCv;
std::queue<AudioPacketData> g_audQ;

// XLang package class wrapping bridge commands
class SunshineAPI
{
    SunshineCallTable* mHostApis = nullptr;
public:
    BEGIN_PACKAGE(SunshineAPI)
        APISET().AddFunc<5>("StartVideo", &SunshineAPI::StartVideo);
        APISET().AddFunc<0>("StopVideo", &SunshineAPI::StopVideo);
        APISET().AddFunc<1>("StartAudio", &SunshineAPI::StartAudio);
        APISET().AddFunc<0>("StopAudio", &SunshineAPI::StopAudio);
        APISET().AddFunc<0>("StopProcessing", &SunshineAPI::StopProcessing);
        APISET().AddFunc<1>("InjectInput", &SunshineAPI::InjectInput);
        APISET().AddFunc<0>("RequestIdr", &SunshineAPI::RequestIdr);
        APISET().AddFunc<1>("FetchEncodedFrame", &SunshineAPI::FetchEncodedFrame);
        APISET().AddFunc<1>("FetchAudioPacket", &SunshineAPI::FetchAudioPacket);
    END_PACKAGE

    void SetHostApis(SunshineCallTable* hostApis)
    {
        mHostApis = hostApis;
    }

    bool StartVideo(std::string display, int width, int height, int fps, int bitrate)
    {
        if (mHostApis && mHostApis->StartVideo)
        {
            return mHostApis->StartVideo(display.c_str(), width, height, fps, bitrate);
        }
        return false;
    }

    bool StartAudio(std::string audioSink)
    {
        if (mHostApis && mHostApis->StartAudio)
        {
            return mHostApis->StartAudio(audioSink.c_str());
        }
        return false;
    }

    void StopVideo()
    {
        if (mHostApis && mHostApis->StopVideo)
        {
            mHostApis->StopVideo();
        }
    }

    void StopAudio()
    {
        if (mHostApis && mHostApis->StopAudio)
        {
            mHostApis->StopAudio();
        }
    }

    void StopProcessing()
    {
        if (mHostApis && mHostApis->StopProcessing)
        {
            mHostApis->StopProcessing();
        }
    }

    void RequestIdr()
    {
        if (mHostApis && mHostApis->RequestIdr)
        {
            mHostApis->RequestIdr();
        }
    }

    int InjectInput(std::string binaryData)
    {
        if (mHostApis && mHostApis->InjectInput)
        {
            return mHostApis->InjectInput((const uint8_t*)binaryData.data(), (int)binaryData.size());
        }
        return -1;
    }

    X::Value FetchEncodedFrame(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(g_vidQMutex);
        if (!g_vidQCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), []{ return !g_vidQ.empty(); })) {
            return X::Value(); // Return null if timeout
        }
        
        auto frame = g_vidQ.front();
        g_vidQ.pop();
        lock.unlock();

        X::List list;
        X::Bin binFrame((char*)frame.data.data(), frame.data.size(), true);
        list += binFrame;
        list += X::Value(frame.isIdr);
        list += X::Value((long long)frame.frameIndex);
        
        X::Value retVal(list);
        return retVal;
    }

    X::Value FetchAudioPacket(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(g_audQMutex);
        if (!g_audQCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), []{ return !g_audQ.empty(); })) {
            return X::Value(); // Return null if timeout
        }
        
        auto pkt = g_audQ.front();
        g_audQ.pop();
        lock.unlock();

        X::List list;
        X::Bin binPacket((char*)pkt.data.data(), pkt.data.size(), true);
        list += binPacket;
        list += X::Value((long long)pkt.pts);
        
        X::Value retVal(list);
        return retVal;
    }
};

SunshineAPI g_api;



static bool GetCurLibInfo(void* EntryFuncName, std::string& strFullPath,
    std::string& strFolderPath, std::string& strLibName)
{
#if (WIN32)
    HMODULE hModule = NULL;
    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCTSTR)EntryFuncName,
        &hModule);
    char path[MAX_PATH];
    GetModuleFileName(hModule, path, MAX_PATH);
    std::string strPath(path);
    strFullPath = strPath;
    auto pos = strPath.rfind("\\");
    if (pos != std::string::npos)
    {
        strFolderPath = strPath.substr(0, pos);
        strLibName = strPath.substr(pos + 1);
    }
#endif
    // Remove extension
    auto pos2 = strLibName.rfind(".");
    if (pos2 != std::string::npos)
    {
        strLibName = strLibName.substr(0, pos2);
    }
    return true;
}

// ----------------------------------------------------
// Public Plugin C API Exports for Sunshine
// ----------------------------------------------------

extern "C" BRIDGE_EXPORT int LoadBridge(void* hostCallTable, const char* hostPath, int lrpcPort)
{
    SunshineCallTable* pTab = (SunshineCallTable*)hostCallTable;
    
    // Wire up Inbound callbacks to push data natively to XLang scripts
    pTab->OnVideoFrame = (f_OnVideoFrame)([](const uint8_t* data, int size, bool isIdr, int64_t frameIndex)
    {
        {
            std::lock_guard<std::mutex> lock(g_vidQMutex);
            if (g_vidQ.size() > 300) {
                g_vidQ.pop(); // Drop oldest frame to avoid blocking or OOM
            }
            g_vidQ.push({std::vector<uint8_t>(data, data + size), isIdr, frameIndex});
        }
        g_vidQCv.notify_one();
    });

    pTab->OnAudioPacket = (f_OnAudioPacket)([](const uint8_t* data, int size, int64_t pts)
    {
        {
            std::lock_guard<std::mutex> lock(g_audQMutex);
            if (g_audQ.size() > 100) {
                g_audQ.pop(); // Drop oldest audio packet to avoid blocking or OOM
            }
            g_audQ.push({std::vector<uint8_t>(data, data + size), pts});
        }
        g_audQCv.notify_one();
    });

    g_api.SetHostApis(pTab);

    std::string strFullPath, strFolderPath, strLibName;
    GetCurLibInfo((void*)LoadBridge, strFullPath, strFolderPath, strLibName);

    g_config.appPath = new char[strFolderPath.length() + 1];
    memcpy((char*)g_config.appPath, strFolderPath.data(), strFolderPath.length() + 1);
    g_config.dbg = false;
    g_config.runEventLoopInThread = true;
    g_config.enterEventLoop = true;

    // Load and Run XLang
    int retCode = g_xload.Load(&g_config);
    std::cout << "[SunshineBridge] XLang Init RetCode: " << retCode << std::endl;
    
    if (retCode == 0) 
    {
        g_xload.Run();
        X::RegisterPackage<SunshineAPI>(hostPath, "sunshine", &g_api);
        X::g_pXHost->Lrpc_Listen(lrpcPort, false);
    }
    return retCode;
}

extern "C" BRIDGE_EXPORT void UnloadBridge()
{
    g_api.SetHostApis(nullptr);
    g_xload.Unload();
}
