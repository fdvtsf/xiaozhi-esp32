#include "audio_diagnostics.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include "sdkconfig.h"

#define TAG "AudioDiagnostics"

namespace {

constexpr uint32_t kPacketMagic = 0x47444158;  // "XADG" in little endian.
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kPacketKindPcm = 1;
constexpr uint8_t kPacketKindEvent = 2;
constexpr uint16_t kPcmBitsPerSample = 16;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t kind;
    uint8_t stream;
    uint8_t flags;
    uint32_t session_id;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t samples_per_channel;
    uint32_t payload_bytes;
};

struct EventPayload {
    uint16_t event;
    uint16_t reserved;
    int32_t value;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 40, "Unexpected audio diagnostic packet header size");
static_assert(sizeof(EventPayload) == 8, "Unexpected audio diagnostic event size");

}  // namespace

struct AudioDiagnostics::Slot {
    PacketHeader header;
    uint8_t payload[CONFIG_AUDIO_DIAGNOSTICS_MAX_PAYLOAD_BYTES];
};

AudioDiagnostics& AudioDiagnostics::GetInstance() {
    static AudioDiagnostics instance;
    return instance;
}

AudioDiagnostics::~AudioDiagnostics() {
    Shutdown();
}

bool AudioDiagnostics::Initialize() {
    if (running_.load()) {
        return true;
    }

    std::string server = CONFIG_AUDIO_DIAGNOSTICS_UDP_SERVER;
    auto colon = server.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == server.size() - 1) {
        ESP_LOGE(TAG, "Invalid UDP target '%s'; expected IPv4:port",
                 CONFIG_AUDIO_DIAGNOSTICS_UDP_SERVER);
        return false;
    }

    char* port_end = nullptr;
    long port = std::strtol(server.c_str() + colon + 1, &port_end, 10);
    if (port_end == server.c_str() + colon + 1 || *port_end != '\0' || port <= 0 || port > 65535) {
        ESP_LOGE(TAG, "Invalid UDP port in '%s'", CONFIG_AUDIO_DIAGNOSTICS_UDP_SERVER);
        return false;
    }

    in_addr server_address = {};
    if (inet_pton(AF_INET, server.substr(0, colon).c_str(), &server_address) != 1) {
        ESP_LOGE(TAG, "Invalid IPv4 address in '%s'", CONFIG_AUDIO_DIAGNOSTICS_UDP_SERVER);
        return false;
    }
    server_ipv4_ = server_address.s_addr;
    server_port_ = static_cast<uint16_t>(port);

    slots_ = static_cast<Slot*>(heap_caps_calloc(
        CONFIG_AUDIO_DIAGNOSTICS_SLOT_COUNT, sizeof(Slot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (slots_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u diagnostic slots in PSRAM",
                 static_cast<unsigned>(CONFIG_AUDIO_DIAGNOSTICS_SLOT_COUNT));
        return false;
    }

    free_slots_ = xQueueCreate(CONFIG_AUDIO_DIAGNOSTICS_SLOT_COUNT, sizeof(uint16_t));
    ready_slots_ = xQueueCreate(CONFIG_AUDIO_DIAGNOSTICS_SLOT_COUNT, sizeof(uint16_t));
    if (free_slots_ == nullptr || ready_slots_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create diagnostic queues");
        ReleaseResources();
        return false;
    }
    for (uint16_t i = 0; i < CONFIG_AUDIO_DIAGNOSTICS_SLOT_COUNT; ++i) {
        xQueueSend(free_slots_, &i, 0);
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_ < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: %d", errno);
        ReleaseResources();
        return false;
    }

    session_id_ = esp_random();
    dropped_packets_.store(0);
    for (auto& sequence : sequences_) {
        sequence.store(0);
    }
    running_.store(true);
    sender_stopped_.store(false);

    if (xTaskCreate(SenderTaskEntry, "audio_diag", 4096, this, 1, &sender_task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create diagnostic sender task");
        running_.store(false);
        sender_stopped_.store(true);
        ReleaseResources();
        return false;
    }

    ESP_LOGI(TAG, "Streaming session %lu to %s (%u slots x %u bytes in PSRAM)",
             static_cast<unsigned long>(session_id_), CONFIG_AUDIO_DIAGNOSTICS_UDP_SERVER,
             static_cast<unsigned>(CONFIG_AUDIO_DIAGNOSTICS_SLOT_COUNT),
             static_cast<unsigned>(CONFIG_AUDIO_DIAGNOSTICS_MAX_PAYLOAD_BYTES));
    return true;
}

void AudioDiagnostics::Shutdown() {
    if (!running_.exchange(false)) {
        return;
    }

    if (sender_task_ != nullptr) {
        xTaskNotifyGive(sender_task_);
        while (!sender_stopped_.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        sender_task_ = nullptr;
    }
    ReleaseResources();
}

void AudioDiagnostics::CapturePcm(AudioDiagnosticStream stream, const int16_t* data,
                                  size_t sample_count, uint32_t sample_rate, uint16_t channels,
                                  int64_t timestamp_us) {
    if (!running_.load(std::memory_order_relaxed) || data == nullptr || sample_count == 0 ||
        sample_rate == 0 || channels == 0 || sample_count < channels) {
        return;
    }

    const size_t total_frames = sample_count / channels;
    const size_t max_frames = CONFIG_AUDIO_DIAGNOSTICS_MAX_PAYLOAD_BYTES /
                              (sizeof(int16_t) * channels);
    if (max_frames == 0) {
        return;
    }
    if (timestamp_us == 0) {
        timestamp_us = esp_timer_get_time();
    }

    size_t frame_offset = 0;
    while (frame_offset < total_frames) {
        size_t frames = std::min(max_frames, total_frames - frame_offset);
        size_t values = frames * channels;
        int64_t packet_timestamp = timestamp_us +
            static_cast<int64_t>(frame_offset) * 1000000 / sample_rate;
        if (!QueuePayload(stream, kPacketKindPcm, data + frame_offset * channels,
                          values * sizeof(int16_t), sample_rate, channels, frames,
                          packet_timestamp)) {
            return;
        }
        frame_offset += frames;
    }
}

void AudioDiagnostics::CaptureEvent(AudioDiagnosticEvent event, int32_t value,
                                    int64_t timestamp_us) {
    EventPayload payload = {
        .event = static_cast<uint16_t>(event),
        .reserved = 0,
        .value = value,
    };
    QueuePayload(AudioDiagnosticStream::Event, kPacketKindEvent, &payload, sizeof(payload), 0, 0,
                 0, timestamp_us == 0 ? esp_timer_get_time() : timestamp_us);
}

bool AudioDiagnostics::QueuePayload(AudioDiagnosticStream stream, uint8_t kind,
                                    const void* payload, size_t payload_bytes,
                                    uint32_t sample_rate, uint16_t channels,
                                    uint32_t samples_per_channel, int64_t timestamp_us) {
    if (!running_.load(std::memory_order_relaxed) || payload == nullptr ||
        payload_bytes > CONFIG_AUDIO_DIAGNOSTICS_MAX_PAYLOAD_BYTES) {
        return false;
    }

    uint16_t index = 0;
    if (xQueueReceive(free_slots_, &index, 0) != pdTRUE) {
        dropped_packets_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Slot& slot = slots_[index];
    uint8_t stream_index = static_cast<uint8_t>(stream);
    slot.header = {
        .magic = kPacketMagic,
        .version = kProtocolVersion,
        .kind = kind,
        .stream = stream_index,
        .flags = 0,
        .session_id = session_id_,
        .sequence = sequences_[stream_index].fetch_add(1, std::memory_order_relaxed),
        .timestamp_us = static_cast<uint64_t>(timestamp_us),
        .sample_rate = sample_rate,
        .channels = channels,
        .bits_per_sample = kind == kPacketKindPcm ? kPcmBitsPerSample : uint16_t{0},
        .samples_per_channel = samples_per_channel,
        .payload_bytes = static_cast<uint32_t>(payload_bytes),
    };
    std::memcpy(slot.payload, payload, payload_bytes);

    if (xQueueSend(ready_slots_, &index, 0) != pdTRUE) {
        dropped_packets_.fetch_add(1, std::memory_order_relaxed);
        xQueueSend(free_slots_, &index, 0);
        return false;
    }
    return true;
}

void AudioDiagnostics::SenderTaskEntry(void* context) {
    static_cast<AudioDiagnostics*>(context)->SenderTask();
}

void AudioDiagnostics::SenderTask() {
    sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_port_);
    server_address.sin_addr.s_addr = server_ipv4_;

    uint32_t last_reported_drops = 0;
    while (running_.load()) {
        uint16_t index = 0;
        if (xQueueReceive(ready_slots_, &index, pdMS_TO_TICKS(250)) == pdTRUE) {
            Slot& slot = slots_[index];
            size_t packet_bytes = sizeof(PacketHeader) + slot.header.payload_bytes;
            ssize_t sent = sendto(socket_, &slot, packet_bytes, 0,
                                  reinterpret_cast<const sockaddr*>(&server_address),
                                  sizeof(server_address));
            if (sent != static_cast<ssize_t>(packet_bytes)) {
                dropped_packets_.fetch_add(1, std::memory_order_relaxed);
            }
            xQueueSend(free_slots_, &index, portMAX_DELAY);
        }

        uint32_t drops = dropped_packets_.load(std::memory_order_relaxed);
        if (drops != last_reported_drops) {
            ESP_LOGW(TAG, "Dropped %lu diagnostic packets",
                     static_cast<unsigned long>(drops));
            last_reported_drops = drops;
        }
    }

    uint16_t index = 0;
    while (xQueueReceive(ready_slots_, &index, 0) == pdTRUE) {
        xQueueSend(free_slots_, &index, 0);
    }
    sender_stopped_.store(true);
    vTaskDelete(nullptr);
}

void AudioDiagnostics::ReleaseResources() {
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    if (free_slots_ != nullptr) {
        vQueueDelete(free_slots_);
        free_slots_ = nullptr;
    }
    if (ready_slots_ != nullptr) {
        vQueueDelete(ready_slots_);
        ready_slots_ = nullptr;
    }
    if (slots_ != nullptr) {
        heap_caps_free(slots_);
        slots_ = nullptr;
    }
}
