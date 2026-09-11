# stackchan-gpt-live

M5Stack Stack-chan for CoreS3, with the official face, head motion, UI,
microphones, and speaker path preserved. Only the XiaoZhi agent transport is
replaced by a direct OpenAI GPT-Live WebSocket connection. No Mac or server
bridge is used.

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
  Japanese-only, no-delegation instructions.
- The unchanged XiaoZhi `AudioService` still produces/consumes 16 kHz Opus.
  Conversion between Opus and PCM happens only inside `GptLiveProtocol`, on
  one serialized 24 KiB PSRAM-backed worker rather than the UI/main or SSL
  receive task.
- GPT-Live transcript/audio events are mapped to the existing `tts`, `stt`, and
  `llm` display events. Stack-chan speaking animation and conservative
  neutral/happy/doubtful/sad emotion mapping remain active.
- CoreS3 is half-duplex in the default no-AEC configuration: existing XiaoZhi
  state handling pauses mic processing while the speaker is active and resumes
  it when playback drains. Touch/wake interruption discards stale output.
- Avatar, motion, `cores3_audio_codec`, `stackchan_display`, `hal_mcp`, and
  phone-avatar WebSocket code are unchanged.

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
経路を維持し、XiaoZhiのエージェント通信だけをOpenAI GPT-Liveの直接WSS接続
に置き換えています。Macや中継サーバーは不要です。

- 接続先: `wss://api.openai.com/v1/live/sessions`
- モデル: `gpt-live-1`、音声: `marin`
- 音声形式: mono PCM16 16 kHz
- 日本語の短い応答、外部委任・ツール呼び出しなし
- 既存AudioServiceとの境界だけでOpus↔PCM変換（PSRAM上の専用24 KiBタスクで実行）
- GPT-Liveの文字起こし・音声イベントを既存の`tts`/`stt`/`llm`表示イベントへ変換
- 標準のAECなし設定では、発話中にマイク処理を止め、再生完了後に再開

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
