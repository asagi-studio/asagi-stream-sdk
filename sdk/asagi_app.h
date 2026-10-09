/*
 * Asagi Stream — app ABI (version 1)
 *
 * An app is a DLL that exports one function, asagi_app_get(). Everything crosses the DLL
 * boundary as plain C functions and structs, so an app may use any compiler or C++ runtime.
 * Memory is never freed on the other side of the boundary: the platform copies HTTP responses
 * immediately, and strings the platform hands out stay valid for the life of the app instance.
 *
 * Threading
 *   - create() and destroy() run while no other call into the app is in flight. destroy() must
 *     stop and join every thread the app started: the DLL is unloaded right after it returns.
 *   - onAudio() and onEvent() run on the platform's bridge thread, never at the same time as each
 *     other. Keep onAudio() quick: it runs once per OBS audio block (about every 21 ms).
 *   - The events replayed right after create() (see below) arrive on the thread that turned the
 *     app on, before any onAudio(); the bridge thread is held back until they are done.
 *   - handleHttp() runs on one of the platform's HTTP threads. Two handleHttp() calls never run
 *     at the same time (one at a time, in the order the requests finished arriving), but they may
 *     come from different threads and may overlap onAudio()/onEvent().
 *   - Host functions may be called from any of the callbacks, including create().
 *
 * HTTP
 *   - method is "GET" or "POST". A HEAD request reaches the app as "GET"; the body is dropped.
 *   - The request and the responder are valid only during the handleHttp() call: respond()
 *     before returning, never later from another thread.
 *
 * Events (onEvent JSON)
 *   {"event":"link","connected":true,"obs":"32.0.1","sampleRate":48000}   OBS bridge (dis)connected
 *   {"event":"state","streaming":false,"recording":false}                current OBS output state
 *   {"event":"streaming_started"} / "streaming_stopped" / "recording_started" / "recording_stopped"
 *   {"event":"tracks","names":["","","","","",""]}                       OBS track names (may be "")
 *   Right after create() an app receives the current link and state events, plus tracks once OBS
 *   has reported its track names.
 *   Unknown events and unknown fields are to be ignored (newer platforms add them).
 *
 * Copyright (c) 2026 Asagi Studio. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-AsagiStudio-Proprietary
 */

#ifndef ASAGI_APP_H
#define ASAGI_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASAGI_APP_ABI_VERSION 1u

#if defined(_WIN32)
# define ASAGI_APP_EXPORT __declspec(dllexport)
#else
# define ASAGI_APP_EXPORT __attribute__((visibility("default")))
#endif

enum
{
    ASAGI_LOG_ERROR = 1,
    ASAGI_LOG_WARNING = 2,
    ASAGI_LOG_INFO = 3,
    ASAGI_LOG_DEBUG = 4
};

typedef struct AsagiAudioBlock
{
    uint32_t track;            /* OBS track index, 0 = Track 1 */
    uint32_t channels;         /* 1..8 (OBS bridge sends stereo) */
    uint32_t sampleRate;       /* Hz */
    uint32_t frames;           /* samples per channel */
    uint64_t timestampNs;      /* OBS audio timestamp */
    const float* const* data;  /* data[channel][frame], valid during the call only */
} AsagiAudioBlock;

typedef struct AsagiHttpRequest
{
    const char* method;  /* "GET" or "POST" (POST always carries the X-Asagi-Request header) */
    const char* path;    /* below the app root, e.g. "api/state" (no leading slash) */
    const char* query;   /* raw query string without '?', "" when none */
    const char* body;    /* request body, may be empty */
    size_t bodySize;
} AsagiHttpRequest;

typedef struct AsagiHttpResponder
{
    void* ctx;
    /* Call at most once; the platform copies body before returning. */
    void (*respond)(void* ctx, int status, const char* contentType, const void* body, size_t size);
} AsagiHttpResponder;

typedef struct AsagiHost
{
    uint32_t abiVersion;
    void* host;  /* pass back as the first argument */
    void (*log)(void* host, int level, const char* message);
    const char* (*dataDir)(void* host);          /* this app's own folder (UTF-8); created by the
                                                    platform, but writes may still fail (disk full) */
    void (*subscribeAudio)(void* host, int track);  /* OBS track to receive, -1 = none */
    const char* (*platformVersion)(void* host);
} AsagiHost;

typedef struct AsagiApp
{
    uint32_t abiVersion;  /* ASAGI_APP_ABI_VERSION the app was built against */
    void* (*create)(const AsagiHost* host);  /* NULL = failed */
    void (*destroy)(void* self);
    void (*onAudio)(void* self, const AsagiAudioBlock* block);
    void (*onEvent)(void* self, const char* json);
    /* Return 1 when handled (respond called), 0 for "not found". */
    int (*handleHttp)(void* self, const AsagiHttpRequest* request, const AsagiHttpResponder* responder);
} AsagiApp;

typedef const AsagiApp* (*AsagiAppGetFn)(uint32_t hostAbiVersion);

#ifdef ASAGI_BUILDING_APP
ASAGI_APP_EXPORT const AsagiApp* asagi_app_get(uint32_t hostAbiVersion);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASAGI_APP_H */
