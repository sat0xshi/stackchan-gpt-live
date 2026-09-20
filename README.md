# stackchan-gpt-live

M5Stack Stack-chan for CoreS3, with the official face, head motion, UI,
microphones, and speaker hardware. The agent uses a direct OpenAI GPT-Live
WebSocket connection with hosted GPT-5.6 Terra reasoning and optional web search.
No Mac or server bridge is used during conversation.

Based on [`m5stack/StackChan`](https://github.com/m5stack/StackChan) commit
`1b5765599fba8aaad1811d9a79358ccc7051f5f3`. XiaoZhi v2.2.4 is vendored under
`firmware/xiaozhi-esp32`; its exact boundary is documented in
[`VENDORED.md`](firmware/xiaozhi-esp32/VENDORED.md).

> Direct device authentication stores a long-lived OpenAI key in ESP32 NVS.
> Anyone with physical flash access may be able to extract it. Use a restricted,
> revocable project key and rotate it if the device is lost. The key is never
> compiled into or committed to this repository.

## English

### What changed

- `GptLiveProtocol` connects to
  `wss://api.openai.com/v1/live/sessions` with `gpt-live-1`.
- Session audio is mono signed PCM16 at 16 kHz, using the `marin` voice and
  Japanese conversation instructions. Complex questions can delegate to hosted
  `gpt-5.6-terra` with `reasoning.effort=low`, a 1,536-token output limit, and
  conditional `web_search`. Casual conversation is instructed not to delegate.
  The limit applies per backend response, not to total session charges.
- The XiaoZhi `AudioService` still produces/consumes 16 kHz Opus.
  Conversion between Opus and PCM happens only inside `GptLiveProtocol`, on
  one serialized 24 KiB PSRAM-backed worker rather than the UI/main or SSL
  receive task.
- GPT-Live transcript/audio events are mapped to the existing `tts`, `stt`, and
  `llm` display events. Stack-chan speaking animation and conservative
  neutral/happy/doubtful/sad emotion mapping remain active.
- Default conversations keep microphone processing active while output plays,
  preserving queued playback across speaking/listening transitions.
- The current diagnostic implementation bypasses conversation AFE processing
  and writes the mono speaker signal to both I2S slots. Echo cancellation and
  longer-session queue saturation remain areas to investigate.
- Avatar, motion, display, MCP HAL, and phone-avatar WebSocket code are preserved.
- See the [2026-09-13 development record](docs/2026-09-13-live-terra-validation.md)
  for measured results, remaining issues, and the tested image identity. Existing
  committed distribution images predate these changes; build this source to use them.

OpenAI's current Live WebSocket contract is documented in
[WebSockets | OpenAI API](https://developers.openai.com/api/docs/guides/voice-websockets).

### Build (ESP-IDF 5.5.4)

Requirements: Python 3, Git, CMake/Ninja, and
[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/).

```bash
cd firmware
. "$HOME/esp/esp-idf/export.sh"       # adjust to your IDF installation
python3 fetch_repos.py                # fetches non-vendored components
idf.py set-target esp32s3
idf.py build
```

The committed defaults select `CONFIG_BOARD_TYPE_M5STACK_STACK_CHAN=y`, 16 MB
flash, PSRAM, and the CoreS3 ESP32-S3 target. `set-target` creates an ignored
local `sdkconfig`.

### Unit Glass2 usage display

Connect an M5Stack Unit Glass2 to the **red** Grove/HY2.0 connector on this
Stack-chan body. That connector is electrically CoreS3 Port A (SDA GPIO2, SCL
GPIO1); the black Grove connector is not Port A. The firmware accepts Glass2
address `0x3C` or solder-selected `0x3D`; the internal GPIO12/GPIO11 bus and its
power, touch, audio, IMU, RTC, and camera devices are unchanged.

At boot the 128x64 monochrome SSD1309 display shows the MVP dummy value
`Grok 73%`. Serial reports either `glass2: OK ...` or `glass2: not found`.
On detection failure it also logs every responding Port A I2C address.
A missing display is non-fatal and does not delay AI.AGENT startup beyond the
short I2C probe. The boot value is not scraped from Grok and is not real usage
data. The display retains up to three service slots and renders compact `G`,
`C`, and `X` labels for Grok, Claude, and Codex; other IDs use their first
letter when a slot is available.

After the Wi-Fi station connects, nanami can replace the boot dummy over the
LAN-only, unauthenticated endpoint at
`http://<stackchan-ip>:8767/usage`. The backward-compatible payload updates
Grok only:

```bash
curl -i -X POST "http://<stackchan-ip>:8767/usage" \
  -H "Content-Type: application/json" \
  -d '{"percent":61,"updatedAt":1758336000}'
```

The multi-service payload updates one to three supplied IDs atomically; omitted
IDs retain their previous values:

```bash
curl -i -X POST "http://<stackchan-ip>:8767/usage" \
  -H "Content-Type: application/json" \
  -d '{"items":[{"id":"grok","percent":7},{"id":"claude","percent":12},{"id":"codex","percent":3}],"updatedAt":1758335000}'
```

Percentages must be integers from 0 through 100. `updatedAt` is a Unix-seconds
integer; `updated_at` is also accepted. Success returns `200 {"ok":true}`;
malformed or out-of-range input returns 400, and a missing/unavailable Glass2
returns 503. Serial logs each accepted service and timestamp. Port 8767 binds
only after station connectivity; there is no authentication in this MVP, so
expose it only to a trusted LAN.

Autonomous physical servo motion is disabled. At avatar startup and each
transition into standby, Stack-chan commands yaw and pitch to their calibrated
home positions once, locks out servo-moving idle/head-pet/IMU modifiers, and
does not enable speaking head motion. Face breathing, blink, idle-expression,
and speaking-mouth animations remain enabled on the main display.

### Flash, key provisioning, and Wi-Fi

Connect the CoreS3 USB data port and find its serial port (for example,
`/dev/ttyACM0` or `/dev/cu.usbmodem*`):

```bash
cd firmware
. "$HOME/esp/esp-idf/export.sh"
idf.py -p /dev/ttyACM0 flash
python3 tools/provision_openai_key.py --port /dev/ttyACM0
idf.py -p /dev/ttyACM0 monitor
```

The provisioning tool prompts with terminal echo disabled, creates only
mode-0600 temporary files, writes namespace `openai`, key `api_key`, then
deletes those files. For automation, put the secret in an environment variable
and pass its *name*, not its value:

```bash
read -rsp "OpenAI API key: " OPENAI_API_KEY; export OPENAI_API_KEY; echo
python3 tools/provision_openai_key.py --port /dev/ttyACM0 --key OPENAI_API_KEY
unset OPENAI_API_KEY
```

Provisioning replaces the 16 KiB NVS partition, including existing Wi-Fi
credentials and settings. Configure Wi-Fi afterward through Stack-chan's
normal on-device **SETUP → Wi-Fi → Change Wi-Fi** flow and the M5Stack mobile
app. Then launch **AI.AGENT**.

### Restore caveats

- Back up settings you care about before flashing.
- To remove the OpenAI key completely, erase flash with
  `idf.py -p PORT erase-flash`; flashing another application alone may preserve
  NVS.
- Reflash the official M5Stack image/source to restore factory behavior. A full
  erase also removes Wi-Fi, calibration, account, and other NVS settings.
- Do not force the powered servos by hand.

## 日本語

### 概要と変更点

CoreS3版公式Stack-chanの顔アニメーション、首振り、UI、マイク・スピーカー
ハードウェアを使用し、OpenAI GPT-Liveへ直接WSS接続します。
相談・調べ物はクラウドのGPT-5.6 Terraへ委任します。会話時にMacや中継サーバーは不要です。

- 接続先: `wss://api.openai.com/v1/live/sessions`
- モデル: `gpt-live-1`、音声: `marin`
- 音声形式: mono PCM16 16 kHz
- 日本語の応答。雑談はGPT Live、相談・調べ物はGPT-5.6 Terraへ委任
- Terraは推論量`low`、1回の出力上限1,536トークン、通常料金枠。必要時だけWeb検索
- 上限は推論トークンを含む1回分であり、月額やセッション全体の課金上限ではありません
- 既存AudioServiceとの境界だけでOpus↔PCM変換（PSRAM上の専用24 KiBタスクで実行）
- GPT-Liveの文字起こし・音声イベントを既存の`tts`/`stt`/`llm`表示イベントへ変換
- 発話中も入力を継続し、通常の状態遷移で再生待ち音声を消さない
- 診断のため会話用AFEを迂回し、スピーカーの左右I2Sスロットへ同じ音声を出力
- [実機検証・残課題の記録](docs/2026-09-13-live-terra-validation.md)。既存の配布バイナリは
  今回の変更前のものです。現在の実装を使う場合はソースからビルドしてください。

APIキーはソースや設定例へ記載せず、端末のNVSだけに保存します。ただし、
端末を物理的に取得した第三者がFlashから抽出できる可能性があります。
権限を制限した失効可能なプロジェクトキーを使用してください。

### ビルド

ESP-IDF v5.5.4をインストール後:

```bash
cd firmware
. "$HOME/esp/esp-idf/export.sh"
python3 fetch_repos.py
idf.py set-target esp32s3
idf.py build
```

コミット済みの`firmware/sdkconfig.defaults`はCoreS3/Stack-chan、
ESP32-S3、16 MB Flash、PSRAMを選択済みです。

### 書き込み・キー・Wi-Fi

```bash
cd firmware
. "$HOME/esp/esp-idf/export.sh"
idf.py -p /dev/ttyACM0 flash
python3 tools/provision_openai_key.py --port /dev/ttyACM0
idf.py -p /dev/ttyACM0 monitor
```

キー入力は画面に表示されません。ツールはNVSの`openai/api_key`へ保存し、
一時ファイルを削除します。NVS全体を書き換えるため、既存Wi-Fi設定などは
消去されます。キーを書き込んだ後、通常の
**SETUP → Wi-Fi → Change Wi-Fi** とM5StackモバイルアプリでWi-Fiを設定し、
**AI.AGENT**を起動してください。

公式状態へ戻す場合は、必要な設定を控えた上で
`idf.py -p PORT erase-flash`を実行し、M5Stack公式Firmwareを書き戻します。
全消去によりAPIキーだけでなくWi-Fi、サーボ調整、アカウント設定も消えます。

## Upstream and license

Original StackChan resources, remote firmware, apps, and server remain in this
tree for provenance. See the upstream project and its MIT license for credits
and terms.
