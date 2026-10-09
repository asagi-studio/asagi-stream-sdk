# Asagi Stream アプリの作り方（SDK 0.5）

Asagi Stream は、アプリを入れて動かすための土台です。アプリは 1 つのファイル（`.asagiapp`）で配り、
Asagi Stream のウィンドウの「Catalog」（ローカルカタログの「Choose file」か、ウィンドウへのドラッグ＆ドロップ）で入れます。
この SDK には、アプリを作るのに必要なものが入っています。

| 場所 | 中身 |
| --- | --- |
| `sdk/asagi_app.h` | アプリと Asagi Stream のあいだの決まり（C の関数と構造体、ABI 1） |
| `sdk/template/` | ひな形のアプリ「Asagi Sample」。コピーして作り始めます |
| `sdk/tools/pack_app.py` | アプリのフォルダを `.asagiapp` にまとめるツール（チェックもします） |
| `common/Json.hpp` | ひな形が使っている小さな JSON の読み書き（ISC ライセンス） |

## アプリでできること

- **OBS の音を受け取る**：OBS のトラック 1〜6 のうち 1 つを選ぶと、その音が届きます（約 21 ms ごと、32 bit float）。
- **OBS の出来事を知る**：OBS とつながった／切れた、配信・録画の開始と停止、トラックの名前。
- **画面を出す**：`web/` に置いたページが、Asagi Stream のウィンドウの中と、OBS のカスタムブラウザドックの両方に出ます。
- **設定を保存する**：アプリごとのデータフォルダ（`%APPDATA%\AsagiStream\apps\<id>`）。アプリを削除しても残ります。

## フォルダの形

```
sample/                  ← フォルダ名は id と同じにします
  manifest.json          ← アプリの情報（必須）
  sample.dll             ← アプリ本体（必須、64 ビットの Windows DLL）
  web/index.html         ← アプリの画面（/apps/sample/ で表示されます）
  web/...                ← 画面で使う画像やスクリプト
  licenses/...           ← 使っているライブラリやフォントのライセンス（あれば）
```

DLL が別の DLL を使う場合は、同じフォルダに置けば読み込まれます（DLL のあるフォルダと System32 だけが探されます）。

## manifest.json

```json
{
  "id": "sample",
  "name": "Asagi Sample",
  "version": "0.1.0",
  "description": "アプリを作るときのひな形",
  "entry": "sample.dll",
  "abi": 1,
  "dock": { "title": "Asagi Sample", "width": 420, "height": 300 },
  "legacyPaths": []
}
```

| 項目 | 内容 |
| --- | --- |
| `id`（必須） | 英小文字・数字・`-` で 32 文字まで（先頭と末尾に `-` は不可）。アプリを見分ける名前で、URL（`/apps/<id>/`）とフォルダ名にもなります。**一度配ったら変えません**（変えると別のアプリになります） |
| `entry`（必須） | DLL のファイル名（英数字と `. _ -`、`.dll` で終わる） |
| `abi`（必須） | `1`（`ASAGI_APP_ABI_VERSION`）。合わないアプリは追加できません |
| `name` / `version` / `description` | 表示用（100 / 32 / 400 バイトまで） |
| `dock` | OBS のドックにするときのおすすめの名前と大きさ（省略可。今の Asagi Stream では使っていません） |
| `legacyPaths` | 昔の URL をこのアプリへ転送したいとき（例：`["/v2/"]`）。16 個まで。`/api` `/apps` などは使えません |

## DLL（アプリ本体）

DLL は `asagi_app_get` という関数を 1 つだけ書き出し、`AsagiApp`（関数の表）を返します。
C の関数と構造体だけでやりとりするので、MinGW-w64 でも Visual Studio でも作れます。

```cpp
#define ASAGI_BUILDING_APP
#include "asagi_app.h"

static const AsagiApp kApp = { ASAGI_APP_ABI_VERSION, create, destroy, onAudio, onEvent, handleHttp };

extern "C" ASAGI_APP_EXPORT const AsagiApp* asagi_app_get(uint32_t hostAbiVersion)
{
    return hostAbiVersion == ASAGI_APP_ABI_VERSION ? &kApp : nullptr;
}
```

| 関数 | 呼ばれるとき |
| --- | --- |
| `create(host)` | アプリをオンにしたとき。アプリの状態を作って返します（失敗は `NULL`） |
| `destroy(self)` | オフにしたとき・終了するとき。自分で作ったスレッドは**ここで必ず止めて待ちます**（直後に DLL が外されます） |
| `onAudio(self, block)` | 選んだトラックの音が届いたとき。**すぐ戻ってください**（重い処理は別のスレッドへ） |
| `onEvent(self, json)` | OBS の出来事。知らないイベントや項目は無視します（新しい Asagi Stream で増えます） |
| `handleHttp(self, request, responder)` | 画面からの `api/...` の呼び出し。`respond` を 1 回呼んで 1 を返します。知らないパスは 0 |

`host` からは、ログ（`%LOCALAPPDATA%\AsagiStream\logs\platform.log` に出ます）、データフォルダ、
受け取るトラックの指定（`subscribeAudio`、-1 で受け取らない）、Asagi Stream のバージョンが使えます。

スレッドの決まり（詳しくは `asagi_app.h`）：

- `onAudio` と `onEvent` は同じスレッドから、同時には呼ばれません。
- `handleHttp` は別のスレッドから、1 回ずつ呼ばれます。`onAudio` と同時に動くことがあるので、共有する値は atomic や mutex で守ります。
- `create` の直後に、そのときの状態（`link`、`state`、`tracks`）がイベントとして届きます。
- アプリの関数から C++ の例外を外に投げてはいけません（ひな形は `try` / `catch` で止めています）。

イベントの例：

```
{"event":"link","connected":true,"obs":"32.0.1","sampleRate":48000}
{"event":"state","streaming":false,"recording":false}
{"event":"streaming_started"}  "streaming_stopped"  "recording_started"  "recording_stopped"
{"event":"tracks","names":["配信","ゲーム","","","",""]}
```

## 画面（web/）

- `web/index.html` が `http://127.0.0.1:<port>/apps/<id>/` で表示されます。ウィンドウの中では Asagi Stream のメニューの右側に、OBS ではドックのアプリ一覧から選ばれたときにドックいっぱいに出ます（どちらも iframe の中）。幅が狭くても崩れないように作ってください。
- OBS のドックでは、マウスが乗っている間だけ、左下に一覧へ戻るボタン（30 px 四方、端から 8 px）が重なります。左下の角には大事なボタンを置かないでください。
- アプリの API は相対パスで呼びます：`fetch("api/state")`。
- **POST には `X-Asagi-Request: 1` ヘッダーが必要**です（ないと 403。ほかのサイトから勝手に操作されないため）。
- アプリがオフのとき、`api/...` は `503 {"error":"app_disabled"}` を返します。ウィンドウとドックでは、オフのアプリのページは閉じられて Asagi Stream が「Off」を出しますが、ページを直接開いたときのために、503 を見て「オフ」と出すと親切です。
- ファイル名は英数字と `. _ -` だけ（1 つの名前は 64 文字まで、`.` で始まる名前や `CON` `NUL` などは不可）。
- 外部のサイト（CDN など）は使わず、必要なものは `web/` に入れます（オフラインでも動くように）。

## ビルドとパッケージ

ひな形は MinGW-w64 と Python 3 があれば、そのまま作れます：

```sh
make -C sdk/template            # build/sample/ に sample.dll、build/AsagiSample-0.1.0.asagiapp ができます
```

フォルダを別の場所にコピーして使うときは、`asagi_app.h` と `Json.hpp` の場所を教えます：

```sh
make SDK=C:/AsagiStream-SDK/sdk COMMON=C:/AsagiStream-SDK/common
```

自分でビルドした場合は、アプリのフォルダをツールでまとめます：

```sh
python3 sdk/tools/pack_app.py path/to/sample            # → AsagiSample-0.1.0.asagiapp
python3 sdk/tools/pack_app.py path/to/sample -o out.asagiapp
python3 sdk/tools/pack_app.py --check out.asagiapp       # できあがったファイルを確かめる
```

ツールは Asagi Stream が追加のときに確かめることを先に全部確かめ、さらに DLL が 64 ビットで
`asagi_app_get` を書き出しているかも見ます。`.asagiapp` の中身は「圧縮しない ZIP」です。普通の ZIP ツールで
圧縮して作ったものは追加できません（ツールを使ってください）。

## 試す

- **開発中**：`AsagiStream.exe --apps-dir C:\dev\apps` のように起動すると、`C:\dev\apps\<id>\` のアプリが
  最優先で読み込まれます。DLL を作り直すときは、先にウィンドウでアプリをオフにします（読み込み中の DLL は
  上書きできません）。作り直してからオンにすると、新しい DLL が読み込まれます（manifest.json を変えたときは
  Asagi Stream を起動し直します）。このフォルダのアプリはウィンドウから削除・置き換えできず、カタログにも出ません。
- **パッケージ**：ウィンドウの「Catalog」で、ローカルカタログの「Choose file」から `.asagiapp` を選ぶか、
  ウィンドウにドラッグ＆ドロップします。追加したアプリはローカルカタログにカードで並びます。
- Asagi Stream を**管理者として**実行しているあいだは、追加したアプリは読み込まれません（普通に起動してください）。

## 更新と削除

- 同じ `id` のパッケージを追加すると、置き換え（更新）になります。オン／オフの選択とデータフォルダはそのまま引き継がれます。
- 削除すると、アプリのファイルが消えます。データフォルダは残るので、もう一度追加すると元の設定で使えます。

## 制限

| 項目 | 上限 |
| --- | --- |
| パッケージの大きさ | 64 MB |
| ファイルの数 | 2000 |
| ファイル名 | 英数字と `. _ - /`、1 つの名前 64 文字・全体 256 文字まで |

## これから

配布用に、Asagi Studio が発行したアプリだけを入れられるようにする仕組み（署名とライセンスの確認）を
次の段階で加える予定です。それまでは、自分で作ったアプリや信頼できるアプリだけを追加してください
（アプリは Asagi Stream の中でプログラムとして動きます）。
