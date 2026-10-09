/*
 * Asagi Sample — the starting point for an Asagi Stream app
 *
 * Copy the template folder, change "id" and "name" in manifest.json and build on it. It shows the
 * pieces most apps need:
 *   - OBS events: whether Asagi Stream is linked to OBS, streaming / recording and for how long
 *   - audio: the peak level of one OBS track, with the track chosen on the app's page
 *   - a setting kept in the app's own data folder (the chosen track)
 *   - its page (web/index.html) and a small JSON API for it
 *
 * API, below /apps/sample/ (the Platform passes "api/..." requests to handleHttp):
 *   GET  api/state           everything the page shows
 *   POST api/track?n=0..5    the track to watch (0 = Track 1), saved for the next start
 *
 * Threads (see asagi_app.h): onAudio / onEvent run on the Platform's bridge thread, handleHttp on
 * its HTTP threads, one call at a time. Here they share atomics plus one mutex for the strings.
 *
 * Copyright (c) 2026 Asagi Studio. All rights reserved.
 */

#define ASAGI_BUILDING_APP
#include "asagi_app.h"

#include "Json.hpp"  // common/Json.hpp: a small JSON reader (ISC license)

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

using asagi::json::Value;

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr int kTracks = 6;

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

// "key=value" from a raw query string ("" when missing)
std::string queryValue(const char* query, const char* key)
{
    const std::string q = query ? query : "";
    const std::string k = std::string(key) + "=";
    for (size_t pos = 0; pos <= q.size();)
    {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos)
            amp = q.size();
        if (q.compare(pos, k.size(), k) == 0)
            return q.substr(pos + k.size(), amp - pos - k.size());
        pos = amp + 1;
    }
    return "";
}

class SampleApp
{
public:
    explicit SampleApp(const AsagiHost* host) : fHost(host)
    {
        // the data folder is the app's own (%APPDATA%\AsagiStream\apps\<id>); it stays when the
        // app is removed, so a reinstalled app finds its settings again
        fSettingsPath = widen(std::string(host->dataDir(host->host)) + "\\track.txt");
        fTrack.store(loadTrack(), std::memory_order_relaxed);
        fHost->subscribeAudio(fHost->host, fTrack.load(std::memory_order_relaxed));
        log(ASAGI_LOG_INFO, std::string("Asagi Sample ") + kVersion + " started on Asagi Stream " + fHost->platformVersion(fHost->host));
    }

    // ---- bridge thread -------------------------------------------------------------------------

    void onAudio(const AsagiAudioBlock* b)
    {
        if (b == nullptr || b->data == nullptr || b->frames == 0 || b->sampleRate == 0)
            return;
        float peak = 0.0f;
        for (uint32_t c = 0; c < b->channels && c < 8; ++c)
        {
            const float* x = b->data[c];
            if (x == nullptr)
                continue;
            for (uint32_t i = 0; i < b->frames; ++i)
            {
                const float v = std::fabs(x[i]);
                if (v > peak && v < 1e6f)  // NaN fails both tests and is skipped
                    peak = v;
            }
        }
        // a meter that falls 20 dB per second after each peak
        const float fall = float(std::pow(0.1, double(b->frames) / double(b->sampleRate)));
        const float held = fPeak.load(std::memory_order_relaxed) * fall;
        fPeak.store(std::max(peak, held), std::memory_order_relaxed);
        fLastAudio.store(GetTickCount64(), std::memory_order_relaxed);
    }

    void onEvent(const char* text)
    {
        Value ev;
        if (text == nullptr || ! asagi::json::parse(text, ev))
            return;
        const std::string name = ev["event"].str();
        const ULONGLONG now = GetTickCount64();
        if (name == "link")
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fLinked = ev["connected"].boolean();
            fObsVersion = fLinked ? ev["obs"].str() : std::string();
        }
        else if (name == "state")  // the current state: right after the app starts and on every new link
        {
            std::lock_guard<std::mutex> lock(fMutex);
            const bool streaming = ev["streaming"].boolean(), recording = ev["recording"].boolean();
            if (streaming && ! fStreaming)
                fStreamSince = now;  // already running: counted from when this app first saw it
            if (recording && ! fRecording)
                fRecordSince = now;
            fStreaming = streaming;
            fRecording = recording;
        }
        else if (name == "streaming_started" || name == "streaming_stopped")
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fStreaming = name == "streaming_started";
            fStreamSince = now;
        }
        else if (name == "recording_started" || name == "recording_stopped")
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fRecording = name == "recording_started";
            fRecordSince = now;
        }
        else if (name == "tracks")
        {
            std::lock_guard<std::mutex> lock(fMutex);
            for (int i = 0; i < kTracks; ++i)
                fTrackNames[i] = ev["names"][size_t(i)].str();
        }
        // anything else: newer Asagi Stream versions add events; ignore what you do not know
    }

    // ---- HTTP thread ---------------------------------------------------------------------------

    int handleHttp(const AsagiHttpRequest* r, const AsagiHttpResponder* res)
    {
        const std::string path = r->path ? r->path : "";
        const bool post = std::strcmp(r->method, "POST") == 0;
        std::string body;
        int status = 200;
        if (path == "api/state")
        {
            if (post)
                status = 405, body = "{\"ok\":false,\"error\":\"method\"}";
            else
                body = state();
        }
        else if (path == "api/track")
        {
            // POST requests always carry the X-Asagi-Request header: the Platform refuses others, so
            // another web site cannot make the user's browser change settings here
            const std::string n = queryValue(r->query, "n");
            const int track = n.size() == 1 && n[0] >= '0' && n[0] < '0' + kTracks ? n[0] - '0' : -1;
            if (! post)
                status = 405, body = "{\"ok\":false,\"error\":\"method\"}";
            else if (track < 0)
                status = 400, body = "{\"ok\":false,\"error\":\"track\"}";
            else
            {
                if (fTrack.exchange(track) != track)
                {
                    fPeak.store(0.0f, std::memory_order_relaxed);
                    fHost->subscribeAudio(fHost->host, track);
                    saveTrack(track);
                    log(ASAGI_LOG_INFO, "watching track " + std::to_string(track + 1));
                }
                body = "{\"ok\":true,\"track\":" + std::to_string(track) + "}";
            }
        }
        else
            return 0;  // the Platform answers 404
        res->respond(res->ctx, status, "application/json; charset=utf-8", body.data(), body.size());
        return 1;
    }

private:
    void log(const int level, const std::string& message) { fHost->log(fHost->host, level, message.c_str()); }

    std::string state()
    {
        const ULONGLONG now = GetTickCount64();
        const float peak = fPeak.load(std::memory_order_relaxed);
        const ULONGLONG lastAudio = fLastAudio.load(std::memory_order_relaxed);
        const bool audio = lastAudio != 0 && now - lastAudio < 1000;
        char peakText[32] = "null";
        if (audio && peak > 1e-5f)  // above -100 dBFS
            std::snprintf(peakText, sizeof(peakText), "%.1f", 20.0 * std::log10(double(peak)));

        std::lock_guard<std::mutex> lock(fMutex);
        std::string o = "{\"ok\":true,\"version\":\"" + std::string(kVersion) + "\",\"platform\":";
        asagi::json::quote(o, fHost->platformVersion(fHost->host));
        o += ",\"linked\":" + std::string(fLinked ? "true" : "false") + ",\"obs\":";
        asagi::json::quote(o, fObsVersion);
        o += ",\"streaming\":" + std::string(fStreaming ? "true" : "false");
        o += ",\"streamingFor\":" + (fStreaming ? std::to_string((now - fStreamSince) / 1000) : std::string("null"));
        o += ",\"recording\":" + std::string(fRecording ? "true" : "false");
        o += ",\"recordingFor\":" + (fRecording ? std::to_string((now - fRecordSince) / 1000) : std::string("null"));
        o += ",\"track\":" + std::to_string(fTrack.load()) + ",\"audio\":" + (audio ? "true" : "false");
        o += ",\"peakDb\":" + std::string(peakText) + ",\"tracks\":[";
        for (int i = 0; i < kTracks; ++i)
        {
            if (i)
                o += ",";
            asagi::json::quote(o, fTrackNames[i]);
        }
        return o + "]}";
    }

    int loadTrack()
    {
        char buf[16] = {};
        DWORD got = 0;
        HANDLE h = CreateFileW(fSettingsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return 0;
        ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(h);
        const int t = std::atoi(buf);
        return t >= 0 && t < kTracks ? t : 0;
    }

    void saveTrack(const int track)
    {
        // write a new file, then swap it in: a power cut leaves the old or the new one, never half
        const std::wstring tmp = fSettingsPath + L".tmp";
        const std::string text = std::to_string(track) + "\n";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr)
                        && written == text.size() && FlushFileBuffers(h);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (! ok || ! MoveFileExW(tmp.c_str(), fSettingsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            log(ASAGI_LOG_WARNING, "could not save the track setting");
        }
    }

    const AsagiHost* fHost;
    std::wstring fSettingsPath;
    std::atomic<int> fTrack { 0 };
    std::atomic<float> fPeak { 0.0f };
    std::atomic<ULONGLONG> fLastAudio { 0 };

    std::mutex fMutex;  // everything below
    bool fLinked = false;
    std::string fObsVersion;
    bool fStreaming = false, fRecording = false;
    ULONGLONG fStreamSince = 0, fRecordSince = 0;
    std::string fTrackNames[kTracks];
};

// ---- the C functions Asagi Stream calls ------------------------------------------------------------
// Nothing may throw across them: Asagi Stream is not C++ on the other side of the DLL boundary.

void* appCreate(const AsagiHost* host)
{
    try
    {
        return new SampleApp(host);
    }
    catch (...)
    {
        return nullptr;  // Asagi Stream shows "アプリの起動に失敗しました"
    }
}

void appDestroy(void* self)
{
    delete static_cast<SampleApp*>(self);  // stop and join your own threads here, if you started any
}

void appAudio(void* self, const AsagiAudioBlock* block)
{
    static_cast<SampleApp*>(self)->onAudio(block);
}

void appEvent(void* self, const char* json)
{
    try
    {
        static_cast<SampleApp*>(self)->onEvent(json);
    }
    catch (...)
    {
    }
}

int appHttp(void* self, const AsagiHttpRequest* request, const AsagiHttpResponder* responder)
{
    try
    {
        return static_cast<SampleApp*>(self)->handleHttp(request, responder);
    }
    catch (...)
    {
        const char body[] = "{\"ok\":false,\"error\":\"internal\"}";
        responder->respond(responder->ctx, 500, "application/json; charset=utf-8", body, sizeof(body) - 1);
        return 1;
    }
}

const AsagiApp kApp = { ASAGI_APP_ABI_VERSION, appCreate, appDestroy, appAudio, appEvent, appHttp };

} // namespace

extern "C" ASAGI_APP_EXPORT const AsagiApp* asagi_app_get(const uint32_t hostAbiVersion)
{
    return hostAbiVersion == ASAGI_APP_ABI_VERSION ? &kApp : nullptr;
}
