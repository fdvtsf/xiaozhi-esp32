#ifndef AUDIO_DIAGNOSTICS_H
#define AUDIO_DIAGNOSTICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

enum class AudioDiagnosticStream : uint8_t {
    CodecInput = 1,
    AfeOutput = 2,
    UplinkPcm = 3,
    PlaybackPcm = 4,
    Event = 5,
};

enum class AudioDiagnosticEvent : uint16_t {
    AecState = 1,
    VadState = 2,
    WakeWordDetected = 3,
    WakeWordState = 4,
    VoiceProcessingState = 5,
};

class AudioDiagnostics {
public:
    static AudioDiagnostics& GetInstance();

    bool Initialize();
    void Shutdown();

    void CapturePcm(AudioDiagnosticStream stream, const int16_t* data, size_t sample_count,
                    uint32_t sample_rate, uint16_t channels, int64_t timestamp_us = 0);
    void CaptureEvent(AudioDiagnosticEvent event, int32_t value, int64_t timestamp_us = 0);

private:
    AudioDiagnostics() = default;
    ~AudioDiagnostics();
    AudioDiagnostics(const AudioDiagnostics&) = delete;
    AudioDiagnostics& operator=(const AudioDiagnostics&) = delete;

    struct Slot;

    static void SenderTaskEntry(void* context);
    void SenderTask();
    bool QueuePayload(AudioDiagnosticStream stream, uint8_t kind, const void* payload,
                      size_t payload_bytes, uint32_t sample_rate, uint16_t channels,
                      uint32_t samples_per_channel, int64_t timestamp_us);
    void ReleaseResources();

    Slot* slots_ = nullptr;
    QueueHandle_t free_slots_ = nullptr;
    QueueHandle_t ready_slots_ = nullptr;
    TaskHandle_t sender_task_ = nullptr;
    int socket_ = -1;
    uint32_t session_id_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> sender_stopped_{true};
    std::atomic<uint32_t> dropped_packets_{0};
    std::atomic<uint32_t> sequences_[6] = {};
    uint32_t server_ipv4_ = 0;
    uint16_t server_port_ = 0;
};

#endif  // AUDIO_DIAGNOSTICS_H
