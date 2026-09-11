#include "gpt_live_protocol.h"

#include "assets/lang_config.h"
#include "board.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_audio_types.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <cstring>
#include <new>

#define TAG "GPTLive"
#define GPT_LIVE_SESSION_STARTED_EVENT (1 << 0)
#define GPT_LIVE_SESSION_CLOSED_EVENT (1 << 1)

namespace {

esp_opus_dec_frame_duration_t OpusFrameDuration(int duration_ms) {
    switch (duration_ms) {
        case 20:
            return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        case 40:
            return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        case 60:
        default:
            return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    }
}

esp_opus_enc_frame_duration_t OpusEncoderFrameDuration(int duration_ms) {
    switch (duration_ms) {
        case 20:
            return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
        case 40:
            return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
        case 60:
        default:
            return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    }
}

}  // namespace

GptLiveProtocol::GptLiveProtocol() {
    event_group_handle_ = xEventGroupCreate();
    server_sample_rate_ = kSampleRate;
    server_frame_duration_ = kFrameDurationMs;
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* argument) {
            static_cast<GptLiveProtocol*>(argument)->FinishOutput();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "gpt_output_idle",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &output_idle_timer_) != ESP_OK) {
        output_idle_timer_ = nullptr;
    }
}

GptLiveProtocol::~GptLiveProtocol() {
    StopAudioTxTask();
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket_.reset();
    }
    StopEventTask();
    if (output_idle_timer_ != nullptr) {
        esp_timer_stop(output_idle_timer_);
        esp_timer_delete(output_idle_timer_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool GptLiveProtocol::InitializeCodecs() {
    esp_opus_dec_cfg_t decoder_config = {
        .sample_rate = kSampleRate,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = OpusFrameDuration(kFrameDurationMs),
        .self_delimited = false,
    };
    auto result = esp_opus_dec_open(
        &decoder_config, sizeof(decoder_config), &opus_decoder_);
    if (result != ESP_AUDIO_ERR_OK || opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize protocol Opus decoder: %d", result);
        return false;
    }

    esp_opus_enc_config_t encoder_config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = OpusEncoderFrameDuration(kFrameDurationMs),
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = true,
    };
    result = esp_opus_enc_open(
        &encoder_config, sizeof(encoder_config), &opus_encoder_);
    if (result != ESP_AUDIO_ERR_OK || opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize protocol Opus encoder: %d", result);
        return false;
    }

    esp_opus_enc_get_frame_size(
        opus_encoder_, &opus_encoder_frame_size_, &opus_encoder_outbuf_size_);
    opus_encoder_frame_size_ /= sizeof(int16_t);
    return opus_encoder_frame_size_ == static_cast<int>(kPcmSamplesPerFrame);
}

bool GptLiveProtocol::Start() {
    if (!InitializeCodecs() || !StartAudioTxTask()) {
        return false;
    }
    if (!StartEventTask()) {
        StopAudioTxTask();
        return false;
    }
    return true;
}

bool GptLiveProtocol::StartAudioTxTask() {
    audio_tx_queue_ =
        xQueueCreate(kAudioTxQueueLength, sizeof(AudioStreamPacket*));
    if (audio_tx_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create GPT-Live audio TX queue");
        return false;
    }

    const BaseType_t result = xTaskCreate(
        [](void* argument) {
            static_cast<GptLiveProtocol*>(argument)->AudioTxTask();
        },
        "gpt_audio_tx", kAudioTxTaskStackSize, this, 2,
        &audio_tx_task_handle_);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPT-Live audio TX task");
        vQueueDelete(audio_tx_queue_);
        audio_tx_queue_ = nullptr;
        return false;
    }
    return true;
}

void GptLiveProtocol::StopAudioTxTask() {
    if (audio_tx_task_handle_ != nullptr && audio_tx_queue_ != nullptr) {
        AudioStreamPacket* sentinel = nullptr;
        xQueueSend(audio_tx_queue_, &sentinel, pdMS_TO_TICKS(100));
        for (int i = 0; i < 20 && audio_tx_task_handle_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (audio_tx_task_handle_ != nullptr) {
            vTaskDelete(audio_tx_task_handle_);
            audio_tx_task_handle_ = nullptr;
        }
    }
    DrainAudioTxQueue();
    if (audio_tx_queue_ != nullptr) {
        vQueueDelete(audio_tx_queue_);
        audio_tx_queue_ = nullptr;
    }
}

void GptLiveProtocol::AudioTxTask() {
    while (true) {
        AudioStreamPacket* raw_packet = nullptr;
        if (xQueueReceive(audio_tx_queue_, &raw_packet, portMAX_DELAY) !=
            pdTRUE) {
            continue;
        }
        if (raw_packet == nullptr) {
            break;
        }

        auto packet = std::unique_ptr<AudioStreamPacket>(raw_packet);
        if (session_started_) {
            ProcessAudioPacket(std::move(packet));
        }
    }
    audio_tx_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void GptLiveProtocol::DrainAudioTxQueue() {
    if (audio_tx_queue_ == nullptr) {
        return;
    }
    AudioStreamPacket* raw_packet = nullptr;
    while (xQueueReceive(audio_tx_queue_, &raw_packet, 0) == pdTRUE) {
        delete raw_packet;
    }
}

bool GptLiveProtocol::StartEventTask() {
    event_queue_ = xQueueCreate(kEventQueueLength, sizeof(std::string*));
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create GPT-Live event queue");
        return false;
    }

    const BaseType_t result = xTaskCreate(
        [](void* argument) {
            static_cast<GptLiveProtocol*>(argument)->EventTask();
        },
        "gpt_event", kEventTaskStackSize, this, 2, &event_task_handle_);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPT-Live event task");
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return false;
    }
    return true;
}

void GptLiveProtocol::StopEventTask() {
    if (event_task_handle_ != nullptr && event_queue_ != nullptr) {
        std::string* sentinel = nullptr;
        xQueueSend(event_queue_, &sentinel, pdMS_TO_TICKS(100));
        for (int i = 0; i < 20 && event_task_handle_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (event_task_handle_ != nullptr) {
            vTaskDelete(event_task_handle_);
            event_task_handle_ = nullptr;
        }
    }
    DrainEventQueue();
    if (event_queue_ != nullptr) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
}

void GptLiveProtocol::EventTask() {
    while (true) {
        std::string* raw_event = nullptr;
        if (xQueueReceive(event_queue_, &raw_event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (raw_event == nullptr) {
            break;
        }
        auto event = std::unique_ptr<std::string>(raw_event);
        HandleEvent(event->data(), event->size());
        last_incoming_time_ = std::chrono::steady_clock::now();
    }
    event_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void GptLiveProtocol::QueueIncomingEvent(const char* data, size_t len) {
    if (event_queue_ == nullptr) {
        return;
    }
    auto event = std::unique_ptr<std::string>(
        new (std::nothrow) std::string(data, len));
    if (event == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate GPT-Live event");
        return;
    }
    std::string* raw_event = event.get();
    if (xQueueSend(event_queue_, &raw_event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "GPT-Live event queue full; dropping event");
        return;
    }
    event.release();
}

void GptLiveProtocol::DrainEventQueue() {
    if (event_queue_ == nullptr) {
        return;
    }
    std::string* raw_event = nullptr;
    while (xQueueReceive(event_queue_, &raw_event, 0) == pdTRUE) {
        delete raw_event;
    }
}

bool GptLiveProtocol::OpenAudioChannel() {
    Settings settings("openai", false);
    const std::string api_key = settings.GetString("api_key");
    if (api_key.empty()) {
        ESP_LOGE(TAG, "OpenAI API key is not provisioned in NVS");
        SetError("OPENAI key missing; provision NVS namespace openai/api_key");
        return false;
    }
    if (opus_decoder_ == nullptr || opus_encoder_ == nullptr) {
        SetError("GPT-Live audio codecs are unavailable");
        return false;
    }

    error_occurred_ = false;
    session_started_ = false;
    output_active_ = false;
    discard_output_ = false;
    input_transcript_.clear();
    output_transcript_.clear();
    output_pcm_.clear();
    xEventGroupClearBits(event_group_handle_,
                         GPT_LIVE_SESSION_STARTED_EVENT |
                             GPT_LIVE_SESSION_CLOSED_EVENT);

    websocket_ = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    const std::string authorization = "Bearer " + api_key;
    websocket_->SetHeader("Authorization", authorization.c_str());
    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            ESP_LOGW(TAG, "Ignoring unexpected binary GPT-Live frame");
            return;
        }
        // EspSsl invokes this callback on its 4 KiB receive task. Copy only;
        // JSON parsing, base64 conversion, and Opus work run on gpt_event.
        QueueIncomingEvent(data, len);
    });
    websocket_->OnDisconnected([this]() {
        session_started_ = false;
        if (on_disconnected_) {
            on_disconnected_();
        }
        if (on_audio_channel_closed_) {
            on_audio_channel_closed_();
        }
    });

    constexpr const char* kUrl = "wss://api.openai.com/v1/live/sessions";
    ESP_LOGI(TAG, "Connecting directly to GPT-Live");
    if (!websocket_->Connect(kUrl)) {
        ESP_LOGE(TAG, "GPT-Live WebSocket connection failed: %d",
                 websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        websocket_.reset();
        return false;
    }
    if (on_connected_) {
        on_connected_();
    }

    if (!SendText(GetSessionStartMessage())) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_, GPT_LIVE_SESSION_STARTED_EVENT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(10000));
    if (!(bits & GPT_LIVE_SESSION_STARTED_EVENT)) {
        SetError(Lang::Strings::SERVER_TIMEOUT);
        websocket_.reset();
        return false;
    }

    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void GptLiveProtocol::CloseAudioChannel(bool send_goodbye) {
    if (websocket_ != nullptr && websocket_->IsConnected() && send_goodbye &&
        session_started_) {
        SendText("{\"type\":\"session.close\"}");
        xEventGroupWaitBits(event_group_handle_, GPT_LIVE_SESSION_CLOSED_EVENT,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(1500));
    }
    session_started_ = false;
    DrainAudioTxQueue();
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket_.reset();
    }
}

bool GptLiveProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(websocket_mutex_);
    return websocket_ != nullptr && websocket_->IsConnected() &&
           session_started_ && !error_occurred_ && !IsTimeout();
}

bool GptLiveProtocol::SendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(websocket_mutex_);
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send GPT-Live event");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool GptLiveProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!session_started_ || packet == nullptr || packet->payload.empty()) {
        return false;
    }
    if (audio_tx_queue_ == nullptr) {
        return false;
    }

    AudioStreamPacket* raw_packet = packet.get();
    if (xQueueSend(audio_tx_queue_, &raw_packet, 0) != pdTRUE) {
        // Keep the real-time producer moving rather than doing codec work or
        // blocking the main application task. A later 60 ms frame is preferable
        // to triggering the watchdog with stale queued microphone audio.
        const uint32_t dropped = ++dropped_audio_frames_;
        if (dropped == 1 || dropped % 16 == 0) {
            ESP_LOGW(TAG, "GPT-Live audio TX queue full; dropped %lu frames",
                     static_cast<unsigned long>(dropped));
        }
        return true;
    }
    packet.release();
    return true;
}

bool GptLiveProtocol::ProcessAudioPacket(
    std::unique_ptr<AudioStreamPacket> packet) {
    std::vector<int16_t> pcm(kPcmSamplesPerFrame);
    uint32_t decoded_size = 0;
    {
        std::lock_guard<std::mutex> lock(codec_mutex_);
        esp_audio_dec_in_raw_t input = {
            .buffer = packet->payload.data(),
            .len = static_cast<uint32_t>(packet->payload.size()),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = reinterpret_cast<uint8_t*>(pcm.data()),
            .len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t)),
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};
        const auto result =
            esp_opus_dec_decode(opus_decoder_, &input, &output, &info);
        if (result != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Failed to decode microphone Opus packet: %d", result);
            return false;
        }
        decoded_size = output.decoded_size;
    }

    const std::string audio =
        Base64Encode(reinterpret_cast<const uint8_t*>(pcm.data()), decoded_size);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.input_audio.append");
    cJSON_AddStringToObject(root, "audio", audio.c_str());
    char* serialized = cJSON_PrintUnformatted(root);
    const bool sent = serialized != nullptr && SendText(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);
    return sent;
}

void GptLiveProtocol::HandleEvent(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid JSON event from GPT-Live");
        return;
    }

    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (std::strcmp(type->valuestring, "session.started") == 0) {
        const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session");
        const cJSON* id =
            session ? cJSON_GetObjectItemCaseSensitive(session, "id") : nullptr;
        if (cJSON_IsString(id)) {
            session_id_ = id->valuestring;
        }
        session_started_ = true;
        last_incoming_time_ = std::chrono::steady_clock::now();
        xEventGroupSetBits(event_group_handle_,
                           GPT_LIVE_SESSION_STARTED_EVENT);
    } else if (std::strcmp(type->valuestring,
                           "session.output_audio.delta") == 0) {
        const cJSON* delta =
            cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(delta) && !discard_output_) {
            {
                std::lock_guard<std::mutex> lock(output_mutex_);
                if (!output_active_) {
                    output_active_ = true;
                    output_transcript_.clear();
                    EmitTtsState("start");
                }
                HandleOutputAudio(delta->valuestring);
            }
            ArmOutputIdleTimer();
        }
    } else if (std::strcmp(type->valuestring,
                           "session.input_transcript.delta") == 0) {
        const cJSON* delta =
            cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(delta)) {
            discard_output_ = false;
            input_transcript_ += delta->valuestring;
            EmitTranscript("user", input_transcript_);
        }
    } else if (std::strcmp(type->valuestring,
                           "session.input_transcript.done") == 0) {
        input_transcript_.clear();
    } else if (std::strcmp(type->valuestring,
                           "session.output_transcript.delta") == 0) {
        const cJSON* delta =
            cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(delta) && !discard_output_) {
            std::lock_guard<std::mutex> lock(output_mutex_);
            if (!output_active_) {
                output_active_ = true;
                EmitTtsState("start");
            }
            output_transcript_ += delta->valuestring;
            EmitTranscript("assistant", output_transcript_);
        }
    } else if (std::strcmp(type->valuestring,
                           "session.output_transcript.done") == 0) {
        FinishOutput();
    } else if (std::strcmp(type->valuestring, "session.closed") == 0) {
        session_started_ = false;
        xEventGroupSetBits(event_group_handle_, GPT_LIVE_SESSION_CLOSED_EVENT);
    } else if (std::strcmp(type->valuestring, "error") == 0) {
        const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* message =
            error ? cJSON_GetObjectItemCaseSensitive(error, "message") : nullptr;
        SetError(cJSON_IsString(message) ? message->valuestring
                                        : Lang::Strings::SERVER_ERROR);
    }
    cJSON_Delete(root);
}

void GptLiveProtocol::HandleOutputAudio(const char* base64) {
    std::vector<uint8_t> bytes;
    if (!Base64Decode(base64, bytes) || bytes.size() % sizeof(int16_t) != 0) {
        ESP_LOGW(TAG, "Invalid PCM delta from GPT-Live");
        return;
    }

    const size_t old_size = output_pcm_.size();
    output_pcm_.resize(old_size + bytes.size() / sizeof(int16_t));
    std::memcpy(output_pcm_.data() + old_size, bytes.data(), bytes.size());

    while (output_pcm_.size() >= kPcmSamplesPerFrame) {
        std::vector<uint8_t> opus(opus_encoder_outbuf_size_);
        uint32_t encoded_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(codec_mutex_);
            esp_audio_enc_in_frame_t input = {
                .buffer = reinterpret_cast<uint8_t*>(output_pcm_.data()),
                .len = static_cast<uint32_t>(kPcmSamplesPerFrame *
                                             sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t output = {
                .buffer = opus.data(),
                .len = static_cast<uint32_t>(opus.size()),
                .encoded_bytes = 0,
            };
            const auto result =
                esp_opus_enc_process(opus_encoder_, &input, &output);
            if (result != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "Failed to encode GPT-Live PCM packet: %d",
                         result);
                output_pcm_.erase(
                    output_pcm_.begin(),
                    output_pcm_.begin() + kPcmSamplesPerFrame);
                continue;
            }
            encoded_bytes = output.encoded_bytes;
        }

        opus.resize(encoded_bytes);
        output_pcm_.erase(output_pcm_.begin(),
                          output_pcm_.begin() + kPcmSamplesPerFrame);
        if (on_incoming_audio_) {
            on_incoming_audio_(
                std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = kSampleRate,
                    .frame_duration = kFrameDurationMs,
                    .timestamp = 0,
                    .payload = std::move(opus),
                }));
        }
    }
}

void GptLiveProtocol::ArmOutputIdleTimer() {
    if (output_idle_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(output_idle_timer_);
    // GPT-Live has no output-audio-done event. A quiet period marks the end
    // while the application separately waits for its playback queue to drain.
    esp_timer_start_once(output_idle_timer_, 1200 * 1000);
}

void GptLiveProtocol::FinishOutput() {
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (!output_active_) {
        return;
    }
    if (output_idle_timer_ != nullptr) {
        esp_timer_stop(output_idle_timer_);
    }
    EmitEmotion(output_transcript_);
    EmitTtsState("stop");
    output_active_ = false;
    output_transcript_.clear();
    output_pcm_.clear();
}

void GptLiveProtocol::EmitUiEvent(cJSON* event) {
    if (on_incoming_json_) {
        on_incoming_json_(event);
    }
    cJSON_Delete(event);
}

void GptLiveProtocol::EmitTtsState(const char* state) {
    cJSON* event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", "tts");
    cJSON_AddStringToObject(event, "state", state);
    EmitUiEvent(event);
}

void GptLiveProtocol::EmitTranscript(const char* role,
                                     const std::string& text) {
    cJSON* event = cJSON_CreateObject();
    if (std::strcmp(role, "user") == 0) {
        cJSON_AddStringToObject(event, "type", "stt");
    } else {
        cJSON_AddStringToObject(event, "type", "tts");
        cJSON_AddStringToObject(event, "state", "sentence_start");
    }
    cJSON_AddStringToObject(event, "text", text.c_str());
    EmitUiEvent(event);
}

void GptLiveProtocol::EmitEmotion(const std::string& text) {
    const char* emotion = "neutral";
    if (text.find('?') != std::string::npos ||
        text.find("？") != std::string::npos) {
        emotion = "doubtful";
    } else if (text.find("ごめん") != std::string::npos ||
               text.find("残念") != std::string::npos ||
               text.find("悲し") != std::string::npos) {
        emotion = "sad";
    } else if (text.find('!') != std::string::npos ||
               text.find("！") != std::string::npos ||
               text.find("ありがとう") != std::string::npos) {
        emotion = "happy";
    }
    cJSON* event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", "llm");
    cJSON_AddStringToObject(event, "emotion", emotion);
    EmitUiEvent(event);
}

std::string GptLiveProtocol::GetSessionStartMessage() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.start");
    cJSON_AddStringToObject(root, "event_id", "stackchan_session_start");
    cJSON* session = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(session, "model", "gpt-live-1");
    cJSON_AddStringToObject(
        session, "instructions",
        "あなたはスタックチャン本人です。自然で短い日本語で会話してください。"
        "外部エージェントへの委任やツール呼び出しは行わず、"
        "この音声セッション内だけで回答してください。");
    cJSON_AddBoolToObject(session, "store", false);
    cJSON* audio = cJSON_AddObjectToObject(session, "audio");
    cJSON* format = cJSON_AddObjectToObject(audio, "format");
    cJSON_AddStringToObject(format, "type", "audio/pcm");
    cJSON_AddNumberToObject(format, "rate", kSampleRate);
    cJSON* output = cJSON_AddObjectToObject(audio, "output");
    cJSON_AddStringToObject(output, "voice", "marin");

    char* serialized = cJSON_PrintUnformatted(root);
    std::string message = serialized ? serialized : "";
    cJSON_free(serialized);
    cJSON_Delete(root);
    return message;
}

std::string GptLiveProtocol::Base64Encode(const uint8_t* data, size_t len) {
    size_t required = 0;
    mbedtls_base64_encode(nullptr, 0, &required, data, len);
    std::string encoded(required, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
            &written, data, len) != 0) {
        return {};
    }
    encoded.resize(written);
    return encoded;
}

bool GptLiveProtocol::Base64Decode(const char* encoded,
                                   std::vector<uint8_t>& decoded) {
    const size_t input_len = std::strlen(encoded);
    size_t required = 0;
    mbedtls_base64_decode(nullptr, 0, &required,
                          reinterpret_cast<const unsigned char*>(encoded),
                          input_len);
    decoded.resize(required);
    size_t written = 0;
    const int result = mbedtls_base64_decode(
        decoded.data(), decoded.size(), &written,
        reinterpret_cast<const unsigned char*>(encoded), input_len);
    if (result != 0) {
        decoded.clear();
        return false;
    }
    decoded.resize(written);
    return true;
}

void GptLiveProtocol::SendWakeWordDetected(const std::string& wake_word) {
    (void)wake_word;
}

void GptLiveProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    discard_output_ = false;
    SendText("{\"type\":\"session.input_audio.unmute\"}");
}

void GptLiveProtocol::SendStopListening() {
    SendText("{\"type\":\"session.input_audio.mute\"}");
}

void GptLiveProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    std::lock_guard<std::mutex> lock(output_mutex_);
    discard_output_ = true;
    if (output_idle_timer_ != nullptr) {
        esp_timer_stop(output_idle_timer_);
    }
    output_active_ = false;
    output_pcm_.clear();
    SendText("{\"type\":\"session.input_audio.unmute\"}");
}

void GptLiveProtocol::SendMcpMessage(const std::string& message) {
    (void)message;
    ESP_LOGD(TAG, "Ignoring MCP message: GPT-Live delegation is disabled");
}
