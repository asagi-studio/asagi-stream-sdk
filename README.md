# Making apps for Asagi Stream (SDK 0.5)

日本語: [README_ja.md](README_ja.md)

Asagi Stream is a host that runs apps for streaming. An app is distributed as one file (`.asagiapp`) and added in the
Asagi Stream window under **Catalog** (**Choose file** in the local catalog, or drag and drop onto the window).
This SDK has what you need to make an app.

| Path | What it is |
| --- | --- |
| `sdk/asagi_app.h` | The contract between an app and Asagi Stream (C functions and structs, ABI 1) |
| `sdk/template/` | A template app, "Asagi Sample". Copy it to start |
| `sdk/tools/pack_app.py` | Packs an app folder into an `.asagiapp` (and checks it) |
| `common/Json.hpp` | The small JSON reader and writer the template uses (ISC license) |

## What an app can do

- **Receive OBS audio**: choose one of OBS's tracks 1–6 and its audio arrives (about every 21 ms, 32-bit float).
- **Know what happens in OBS**: connected or disconnected, streaming and recording started and stopped, track names.
- **Show a page**: the page in `web/` appears both inside the Asagi Stream window and in an OBS custom browser dock.
- **Save settings**: each app has a data folder (`%APPDATA%\AsagiStream\apps\<id>`). It stays when the app is removed.

## Folder layout

```
sample/                  ← the folder name is the id
  manifest.json          ← about the app (required)
  sample.dll             ← the app itself (required, a 64-bit Windows DLL)
  web/index.html         ← the app's page (shown at /apps/sample/)
  web/...                ← images and scripts the page uses
  licenses/...           ← licenses of the libraries and fonts you use (if any)
```

A DLL that your DLL uses is loaded when it sits in the same folder (only the DLL's folder and System32 are searched).

## manifest.json

```json
{
  "id": "sample",
  "name": "Asagi Sample",
  "version": "0.1.0",
  "description": "A template for making apps",
  "entry": "sample.dll",
  "abi": 1,
  "dock": { "title": "Asagi Sample", "width": 420, "height": 300 },
  "legacyPaths": []
}
```

| Field | Meaning |
| --- | --- |
| `id` (required) | Lowercase letters, digits and `-`, up to 32 characters (no `-` at the start or end). It tells apps apart and is also the URL (`/apps/<id>/`) and the folder name. **Never change it once released** (a new id makes a different app) |
| `entry` (required) | The DLL's file name (letters, digits and `. _ -`, ending in `.dll`) |
| `abi` (required) | `1` (`ASAGI_APP_ABI_VERSION`). An app with another value can't be added |
| `name` / `version` / `description` | Shown to people (up to 100 / 32 / 400 bytes) |
| `dock` | Suggested name and size for an OBS dock (optional; the current Asagi Stream doesn't use it) |
| `legacyPaths` | Old URLs to forward to this app (for example `["/v2/"]`). Up to 16. `/api`, `/apps` and the like can't be used |

## The DLL

The DLL exports one function, `asagi_app_get`, which returns an `AsagiApp` (a table of functions).
Only C functions and structs cross the boundary, so you can build with MinGW-w64 or Visual Studio.

```cpp
#define ASAGI_BUILDING_APP
#include "asagi_app.h"

static const AsagiApp kApp = { ASAGI_APP_ABI_VERSION, create, destroy, onAudio, onEvent, handleHttp };

extern "C" ASAGI_APP_EXPORT const AsagiApp* asagi_app_get(uint32_t hostAbiVersion)
{
    return hostAbiVersion == ASAGI_APP_ABI_VERSION ? &kApp : nullptr;
}
```

| Function | Called when |
| --- | --- |
| `create(host)` | The app is turned on. Create the app's state and return it (`NULL` on failure) |
| `destroy(self)` | The app is turned off, or Asagi Stream quits. **Stop and wait for the threads you started here** (the DLL is unloaded right after) |
| `onAudio(self, block)` | Audio of the chosen track arrives. **Return quickly** (move heavy work to another thread) |
| `onEvent(self, json)` | Something happened in OBS. Ignore events and fields you don't know (newer versions of Asagi Stream add more) |
| `handleHttp(self, request, responder)` | The page calls `api/...`. Call `respond` once and return 1; return 0 for paths you don't know |

From `host` you get logging (written to `%LOCALAPPDATA%\AsagiStream\logs\platform.log`), the data folder,
the choice of track to receive (`subscribeAudio`, -1 for none) and the version of Asagi Stream.

Threading rules (details in `asagi_app.h`):

- `onAudio` and `onEvent` are called from the same thread, never at the same time.
- `handleHttp` is called from another thread, one call at a time. It can run while `onAudio` does, so protect shared values with atomics or a mutex.
- Right after `create`, the current state (`link`, `state`, `tracks`) arrives as events.
- Never let a C++ exception escape an app function (the template stops them with `try` / `catch`).

Example events:

```
{"event":"link","connected":true,"obs":"32.0.1","sampleRate":48000}
{"event":"state","streaming":false,"recording":false}
{"event":"streaming_started"}  "streaming_stopped"  "recording_started"  "recording_stopped"
{"event":"tracks","names":["Stream","Game","","","",""]}
```

## The page (web/)

- `web/index.html` is served at `http://127.0.0.1:<port>/apps/<id>/`. In the window it appears to the right of Asagi Stream's menu; in OBS it fills the dock when it's chosen from the dock's list of apps (in an iframe in both cases). Make it work in narrow widths too.
- In an OBS dock, while the pointer is over it, a button back to the list (30 px square, 8 px from the edges) sits over the bottom-left corner. Don't put important buttons in that corner.
- Call your app's API with relative paths: `fetch("api/state")`.
- **A POST needs the header `X-Asagi-Request: 1`** (without it the answer is 403, so other sites can't drive your app).
- While the app is off, `api/...` answers `503 {"error":"app_disabled"}`. The window and the dock close the page of an app that is off and Asagi Stream shows "Off" instead, but for a page opened directly it's kind to show "Off" on a 503.
- File names use only letters, digits and `. _ -` (up to 64 characters per name; no names that start with `.`, and no `CON`, `NUL` and the like).
- Don't use outside sites (CDNs and so on); put everything you need in `web/` so that it works offline.

## Build and pack

With MinGW-w64 and Python 3, the template builds as it is:

```sh
make -C sdk/template            # makes build/sample/ with sample.dll, and build/AsagiSample-0.1.0.asagiapp
```

When you copy the folder somewhere else, tell it where `asagi_app.h` and `Json.hpp` are:

```sh
make SDK=C:/AsagiStream-SDK/sdk COMMON=C:/AsagiStream-SDK/common
```

If you build your own way, pack the app folder with the tool:

```sh
python3 sdk/tools/pack_app.py path/to/sample            # → AsagiSample-0.1.0.asagiapp
python3 sdk/tools/pack_app.py path/to/sample -o out.asagiapp
python3 sdk/tools/pack_app.py --check out.asagiapp       # check a finished file
```

The tool checks everything that Asagi Stream checks when a package is added, and also that the DLL is 64-bit and
exports `asagi_app_get`. An `.asagiapp` is an uncompressed ZIP. A ZIP made with an ordinary (compressing) ZIP tool
can't be added, so use the tool.

## Try it

- **While developing**: start Asagi Stream as `AsagiStream.exe --apps-dir C:\dev\apps` and the apps in `C:\dev\apps\<id>\`
  load first. Before rebuilding the DLL, turn the app off in the window (a loaded DLL can't be overwritten). Turn it on
  after rebuilding and the new DLL loads (restart Asagi Stream when you change manifest.json). Apps in this folder can't
  be removed or replaced from the window, and they don't appear in the catalog.
- **As a package**: in the window's **Catalog**, choose the `.asagiapp` with **Choose file** in the local catalog, or
  drag and drop it onto the window. Added apps show as cards in the local catalog.
- While Asagi Stream runs **as administrator**, added apps don't load (start it normally).

## Updating and removing

- Adding a package with the same `id` replaces the app (an update). The on/off choice and the data folder carry over.
- Removing an app deletes its files. The data folder stays, so adding it again brings its settings back.

## Limits

| What | Limit |
| --- | --- |
| Package size | 64 MB |
| Number of files | 2000 |
| File names | Letters, digits and `. _ - /`; up to 64 characters per name and 256 in all |

## What's next

A way to accept only apps published by Asagi Studio (signatures and license checks) is planned for the next stage.
Until then, add only apps you made or trust (an app runs as a program inside Asagi Stream).

## License

Copyright (c) 2026 Asagi Studio. All rights reserved. Only `common/Json.hpp` is under the ISC license (see `LICENSE-ISC.txt`).
