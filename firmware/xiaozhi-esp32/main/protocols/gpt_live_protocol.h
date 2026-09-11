#ifndef GPT_LIVE_PROTOCOL_H
#define GPT_LIVE_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <web_socket.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * Direct GPT-Live transport for StackChan.
 *
 * AudioService remains unchanged and continues to exchange 16 kHz/60 ms Opus
 * packets with Protocol. This class is the only codec boundary: it decodes mic
 * Opus to PCM16 for GPT-Live and encodes GPT-Live PCM16 back to Opus for the
 * existing speaker path.
 */
class GptLiveProtocol : public Protocol {
public:
    GptLiveProtocol();
    ~GptLiveProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;

private:
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameDurationMs = 60;
    static constexpr size_t kPcmSamplesPerFrame =
        kSampleRate * kFrameDurationMs / 1000;

    EventGroupHandle_t event_group_handle_ = nullptr;
    esp_timer_handle_t output_idle_timer_ = nullptr;
    std::unique_ptr<WebSocket> websocket_;
    void* opus_decoder_ = nullptr;
    void* opus_encoder_ = nullptr;
    int opus_encoder_frame_size_ = 0;
    int opus_encoder_outbuf_size_ = 0;
    std::mutex codec_mutex_;
    std::vector<int16_t> output_pcm_;
    std::string input_transcript_;
    std::string output_transcript_;
    bool session_started_ = false;
    bool output_active_ = false;
    bool discard_output_ = false;

    bool SendText(const std::string& text) override;
    bool InitializeCodecs();
    void HandleEvent(const char* data, size_t len);
    void HandleOutputAudio(const char* base64);
    void ArmOutputIdleTimer();
    void FinishOutput();
    void EmitUiEvent(cJSON* event);
    void EmitTtsState(const char* state);
    void EmitTranscript(const char* role, const std::string& text);
    void EmitEmotion(const std::string& text);
    std::string GetSessionStartMessage() const;
    static std::string Base64Encode(const uint8_t* data, size_t len);
    static bool Base64Decode(const char* encoded, std::vector<uint8_t>& decoded);
};

#endif
