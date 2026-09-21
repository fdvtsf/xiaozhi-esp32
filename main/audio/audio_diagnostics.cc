#include "audio_diagnostics.h"

#if CONFIG_AUDIO_DIAGNOSTICS
#include <algorithm>
#include <cinttypes>

#include <esp_log.h>
#include <esp_timer.h>

#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#endif

namespace {
constexpr const char* TAG = "AudioDiag";
uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
constexpr size_t kUdpSamplesPerPacket = 600;

struct __attribute__((packed)) PcmPacketHeader {
    char magic[4];
    uint8_t version;
    uint8_t point;
    uint8_t channels;
    uint8_t flags;
    uint32_t sequence;
    uint32_t rate;
    uint32_t samples;
};

static_assert(sizeof(PcmPacketHeader) == 20);
#endif
}  // namespace

AudioDiagnostics::AudioDiagnostics() {
#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
    const std::string server = CONFIG_AUDIO_DIAGNOSTIC_UDP_SERVER;
    const auto colon = server.rfind(':');
    if (colon == std::string::npos) {
        ESP_LOGE(TAG, "Invalid UDP receiver: %s", server.c_str());
        return;
    }
    char* end = nullptr;
    const long port = std::strtol(server.c_str() + colon + 1, &end, 10);
    if (end == server.c_str() + colon + 1 || *end != '\0' || port < 1 || port > 65535) {
        ESP_LOGE(TAG, "Invalid UDP receiver port: %s", server.c_str());
        return;
    }
    udp_port_ = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, server.substr(0, colon).c_str(), &udp_address_) != 1) {
        ESP_LOGE(TAG, "Invalid UDP receiver address: %s", server.c_str());
        return;
    }
    udp_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_fd_ < 0) {
        ESP_LOGE(TAG, "Cannot create diagnostic UDP socket: %d", errno);
        return;
    }
    const int flags = fcntl(udp_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(udp_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Cannot make diagnostic UDP socket nonblocking: %d", errno);
        close(udp_fd_);
        udp_fd_ = -1;
        return;
    }
    ESP_LOGI(TAG, "PCM UDP receiver: %s", server.c_str());
#endif
}

AudioDiagnostics::~AudioDiagnostics() {
#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
    if (udp_fd_ >= 0) {
        close(udp_fd_);
    }
#endif
}

AudioDiagnostics& AudioDiagnostics::Get() {
    static AudioDiagnostics diagnostics;
    return diagnostics;
}

void AudioDiagnostics::Observe(Point point, const int16_t* pcm, size_t samples, int channels,
                               int rate) {
    if (pcm == nullptr || channels <= 0 || samples < static_cast<size_t>(channels)) {
        return;
    }
    auto& stats = pcm_[point];
    const size_t frames = samples / channels;
    for (int ch = 0; ch < std::min<int>(channels, kMaxPcmChannels); ++ch) {
        uint32_t peak = 0, nonzero = 0, clipped = 0;
        uint64_t magnitude_sum = 0;
        int64_t signed_sum = 0;
        for (size_t i = 0; i < frames; ++i) {
            const int32_t value = pcm[i * channels + ch];
            const uint32_t magnitude = value < 0 ? -value : value;
            peak = std::max(peak, magnitude);
            magnitude_sum += magnitude;
            signed_sum += value;
            nonzero += value != 0;
            clipped += value == -32768 || value == 32767;
        }
        auto& out = stats.channel[ch];
        out.peak.store(peak, std::memory_order_relaxed);
        out.mean_abs.store(magnitude_sum / frames, std::memory_order_relaxed);
        out.dc.store(signed_sum / static_cast<int64_t>(frames), std::memory_order_relaxed);
        out.nonzero.store(nonzero, std::memory_order_relaxed);
        out.clipped.fetch_add(clipped, std::memory_order_relaxed);
        auto previous = out.interval_peak.load(std::memory_order_relaxed);
        while (previous < peak && !out.interval_peak.compare_exchange_weak(
                                      previous, peak, std::memory_order_relaxed)) {
        }
    }
    stats.channels.store(channels, std::memory_order_relaxed);
    stats.rate.store(rate, std::memory_order_relaxed);
    stats.frames.store(frames, std::memory_order_relaxed);
    stats.last_ms.store(NowMs(), std::memory_order_relaxed);
    // Publish the completed block; snapshots are approximate, not audio synchronization.
    stats.blocks.fetch_add(1, std::memory_order_release);
#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
    StreamPcm(point, pcm, samples, channels, rate);
#endif
}

#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
void AudioDiagnostics::StreamPcm(Point point, const int16_t* pcm, size_t samples, int channels,
                                 int rate) {
    if (udp_fd_ < 0 || channels <= 0 || channels > 255 || rate <= 0) {
        return;
    }
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = udp_port_;
    server.sin_addr.s_addr = udp_address_;
    const size_t chunk_limit = (kUdpSamplesPerPacket / channels) * channels;
    for (size_t offset = 0; offset < samples;) {
        const size_t chunk_samples = std::min(chunk_limit, samples - offset);
        PcmPacketHeader header{{'X', 'A', 'D', 'P'},
                               1,
                               static_cast<uint8_t>(point),
                               static_cast<uint8_t>(channels),
                               1,
                               htonl(udp_sequence_[point].fetch_add(1, std::memory_order_relaxed)),
                               htonl(static_cast<uint32_t>(rate)),
                               htonl(static_cast<uint32_t>(chunk_samples))};
        iovec buffers[] = {{&header, sizeof(header)},
                           {const_cast<int16_t*>(pcm + offset), chunk_samples * sizeof(int16_t)}};
        msghdr message{};
        message.msg_name = &server;
        message.msg_namelen = sizeof(server);
        message.msg_iov = buffers;
        message.msg_iovlen = 2;
        const size_t packet_size = sizeof(header) + chunk_samples * sizeof(int16_t);
        if (sendmsg(udp_fd_, &message, MSG_DONTWAIT) == static_cast<ssize_t>(packet_size)) {
            Count(UdpSent);
        } else {
            Count(UdpDropped);
        }
        offset += chunk_samples;
    }
}
#endif

void AudioDiagnostics::AppliedControls(bool wake, bool aec) {
    applied_.store((wake ? 1U : 0U) | (aec ? 2U : 0U), std::memory_order_relaxed);
}

void AudioDiagnostics::FetchResult(int vad, int wake) {
    vad_.store(vad, std::memory_order_relaxed);
    wake_.store(wake, std::memory_order_relaxed);
}

void AudioDiagnostics::PrintIfDue(const char* state, bool service_wake, bool service_voice) {
    const uint32_t now = NowMs();
    const uint32_t interval = now - last_print_ms_;
    if (interval < 5000) {
        return;
    }
    last_print_ms_ = now;
    uint32_t total[CounterCount], delta[CounterCount];
    for (int i = 0; i < CounterCount; ++i) {
        total[i] = counts_[i].load(std::memory_order_relaxed);
        delta[i] = total[i] - previous_[i];
        previous_[i] = total[i];
    }
    const uint32_t applied = applied_.load(std::memory_order_relaxed);
    ESP_LOGI(TAG,
             "state=%s window_ms=%" PRIu32
             " service_wake=%u service_voice=%u"
             " afe=0x%" PRIx32 " applied_wake=%u applied_aec=%u vad=%" PRId32 " wn=%" PRId32
             " wake_total=%" PRIu32,
             state, interval, unsigned(service_wake), unsigned(service_voice), afe_bits_.load(),
             unsigned(applied & 1), unsigned((applied >> 1) & 1), vad_.load(), wake_.load(),
             total[WakeDetected]);
    ESP_LOGI(TAG,
             "delta read=%" PRIu32 "/%" PRIu32 " err=%" PRIu32 " feed=%" PRIu32 "/%" PRIu32
             " fetch=%" PRIu32 "/%" PRIu32 " err=%" PRIu32 " discard=%" PRIu32
             " totals read=%" PRIu32 "/%" PRIu32 " feed=%" PRIu32 "/%" PRIu32 " fetch=%" PRIu32
             "/%" PRIu32,
             delta[ReadStart], delta[ReadDone], delta[ReadError], delta[FeedStart], delta[FeedDone],
             delta[FetchStart], delta[FetchDone], delta[FetchError], delta[FetchDiscard],
             total[ReadStart], total[ReadDone], total[FeedStart], total[FeedDone],
             total[FetchStart], total[FetchDone]);
#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
    ESP_LOGI(TAG, "udp delta sent=%" PRIu32 " dropped=%" PRIu32, delta[UdpSent], delta[UdpDropped]);
#endif
    const char* names[] = {
        "raw_codec",
        "resampled",
        "afe_output",
#if CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS
        "tdm_slots",
#endif
    };
    for (int i = 0; i < PointCount; ++i) {
        auto& stats = pcm_[i];
        const uint32_t blocks = stats.blocks.load(std::memory_order_acquire);
        if (blocks == 0) {
            ESP_LOGI(TAG, "%s: no samples observed", names[i]);
            continue;
        }
        const uint32_t last_ms = stats.last_ms.load();
        const uint32_t age = NowMs() - last_ms;
        const uint32_t frames = stats.frames.load();
        const uint32_t channels = stats.channels.load();
        ESP_LOGI(TAG,
                 "%s: blocks=%" PRIu32 " (+%" PRIu32 ") age_ms=%" PRIu32 " rate=%" PRIu32
                 " channels=%" PRIu32 " last_frames=%" PRIu32,
                 names[i], blocks, blocks - previous_blocks_[i], age, stats.rate.load(), channels,
                 frames);
        previous_blocks_[i] = blocks;
        for (uint32_t ch = 0;
             ch < std::min<uint32_t>(channels, static_cast<uint32_t>(kMaxPcmChannels)); ++ch) {
            auto& c = stats.channel[ch];
            const uint32_t clipped = c.clipped.load();
            ESP_LOGI(TAG,
                     "  ch%u peak=%" PRIu32 " max5s=%" PRIu32 " meanabs=%" PRIu32 " dc=%" PRId32
                     " nonzero=%" PRIu32 "/%" PRIu32 " clip_delta=%" PRIu32,
                     unsigned(ch), c.peak.load(), c.interval_peak.exchange(0), c.mean_abs.load(),
                     c.dc.load(), c.nonzero.load(), frames, clipped - previous_clipped_[i][ch]);
            previous_clipped_[i][ch] = clipped;
        }
    }
}
#endif
