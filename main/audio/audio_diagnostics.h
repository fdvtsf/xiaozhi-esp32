#pragma once

#include <sdkconfig.h>

#if CONFIG_AUDIO_DIAGNOSTICS
#include <atomic>
#include <cstddef>
#include <cstdint>

// Observers only: never acquire audio locks or change PCM, codec or AFE controls.
// Each PCM point has a single producer; the application task prints snapshots.
class AudioDiagnostics {
public:
    enum Point {
        Raw,
        Resampled,
        Afe,
#if CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS
        TdmSlots,
#endif
        PointCount
    };
    enum Counter {
        ReadStart,
        ReadDone,
        ReadError,
        FeedStart,
        FeedDone,
        FetchStart,
        FetchDone,
        FetchError,
        FetchDiscard,
        WakeDetected,
#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
        UdpSent,
        UdpDropped,
#endif
        CounterCount
    };

    static AudioDiagnostics& Get();
    void Count(Counter counter) { counts_[counter].fetch_add(1, std::memory_order_relaxed); }
    void Observe(Point point, const int16_t* pcm, size_t samples, int channels, int rate);
    void AfeBits(uint32_t bits) { afe_bits_.store(bits, std::memory_order_relaxed); }
    void AppliedControls(bool wake, bool aec);
    void FetchResult(int vad, int wake);
    void PrintIfDue(const char* state, bool service_wake, bool service_voice);

private:
    AudioDiagnostics();
    ~AudioDiagnostics();

    struct Channel {
        std::atomic<uint32_t> peak{0}, interval_peak{0}, mean_abs{0}, nonzero{0}, clipped{0};
        std::atomic<int32_t> dc{0};
    };
    static constexpr size_t kMaxPcmChannels =
#if CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS
        4;
#else
        2;
#endif
    struct Pcm {
        Channel channel[kMaxPcmChannels];
        std::atomic<uint32_t> blocks{0}, frames{0}, last_ms{0}, channels{0}, rate{0};
    };
    // Use the same 32-bit atomic primitives as the engine's existing controls.
    // Some S3 toolchain configurations implement these with short critical sections.
    static_assert(sizeof(uint32_t) == 4);
    Pcm pcm_[PointCount];
    std::atomic<uint32_t> counts_[CounterCount]{};
    std::atomic<uint32_t> afe_bits_{0}, applied_{0};
    std::atomic<int32_t> vad_{-1}, wake_{-1};
    // Only the application task accesses these print cursors.
    uint32_t previous_[CounterCount]{};
    uint32_t previous_blocks_[PointCount]{};
    uint32_t previous_clipped_[PointCount][kMaxPcmChannels]{};
    uint32_t last_print_ms_ = 0;

#if CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP
    int udp_fd_ = -1;
    uint32_t udp_address_ = 0;
    uint16_t udp_port_ = 0;
    std::atomic<uint32_t> udp_sequence_[PointCount]{};
    void StreamPcm(Point point, const int16_t* pcm, size_t samples, int channels, int rate);
#endif
};
#endif
