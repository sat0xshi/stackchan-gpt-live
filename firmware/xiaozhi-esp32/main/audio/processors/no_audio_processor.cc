#include "no_audio_processor.h"
#include <esp_log.h>

#define TAG "NoAudioProcessor"

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    if (!is_running_ || !output_callback_) {
        return;
    }

    // Convert stereo to mono if needed
    if (codec_->input_channels() == 2) {
        for (size_t i = 0, j = 0; i < data.size() / 2; ++i, j += 2) {
            output_buffer_.push_back(data[j]);
        }
    } else {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    }

    // Release the buffer lock before enqueueing: the callback may wait for
    // the application to drain audio, and the application may call Stop().
    while (is_running_ && output_buffer_.size() >= (size_t)frame_samples_) {
        std::vector<int16_t> frame(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        lock.unlock();
        output_callback_(std::move(frame));
        lock.lock();
    }
}

void NoAudioProcessor::Start() {
    is_running_ = true;
}

void NoAudioProcessor::Stop() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    is_running_ = false;
    output_buffer_.clear();
}

bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}
