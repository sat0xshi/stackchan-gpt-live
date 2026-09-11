#ifndef GPT_LIVE_PROTOCOL_H
#define GPT_LIVE_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <web_socket.h>

#include <atomic>
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
    static constexpr size_t kWorkQueueLength = 16;
    static constexpr uint32_t kWorkerPsramStackSize = 2048 * 12;
    static constexpr uint32_t kWorkerInternalStackSize = 2048 * 6;

    enum class WorkType : uint8_t {
        kAudioTx,
        kIncomingEvent,
        kStop,
    };
    struct WorkItem {
        WorkType type;
        void* payload;
    };

    EventGroupHandle_t event_group_handle_ = nullptr;
    QueueHandle_t work_queue_ = nullptr;
    TaskHandle_t worker_task_handle_ = nullptr;
    bool worker_stack_in_psram_ = false;
    esp_timer_handle_t output_idle_timer_ = nullptr;
    std::unique_ptr<WebSocket> websocket_;
    void* opus_decoder_ = nullptr;
    void* opus_encoder_ = nullptr;
    int opus_encoder_frame_size_ = 0;
    int opus_encoder_outbuf_size_ = 0;
    std::mutex codec_mutex_;
    std::mutex output_mutex_;
    mutable std::mutex websocket_mutex_;
    std::vector<int16_t> output_pcm_;
    std::string input_transcript_;
    std::string output_transcript_;
    std::atomic_bool session_started_{false};
    bool output_active_ = false;
    std::atomic_bool discard_output_{false};
    std::atomic_uint32_t dropped_audio_frames_{0};

    bool SendText(const std::string& text) override;
    bool InitializeCodecs();
    bool StartWorkerTask();
    void StopWorkerTask();
    void WorkerTask();
    bool QueueWork(WorkItem item);
    void DrainWorkQueue();
    bool ProcessAudioPacket(std::unique_ptr<AudioStreamPacket> packet);
    void QueueIncomingEvent(const char* data, size_t len);
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
