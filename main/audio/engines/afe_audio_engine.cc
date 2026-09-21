#include "afe_audio_engine.h"
#if CONFIG_AUDIO_DIAGNOSTICS
#include "audio_diagnostics.h"
#endif

#include <cassert>
#include <cstring>
#include <sstream>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vadn_models.h>

#include "audio_service.h"
#include "wake_words/custom_wake_word.h"

#define TAG "AfeAudioEngine"

namespace {
const char* AecModeName(aec_mode_t mode) {
    switch (mode) {
        case AEC_MODE_SR_LOW_COST:
            return "SR_LOW_COST";
        case AEC_MODE_SR_HIGH_PERF:
            return "SR_HIGH_PERF";
        case AEC_MODE_VOIP_LOW_COST:
            return "VOIP_LOW_COST";
        case AEC_MODE_VOIP_HIGH_PERF:
            return "VOIP_HIGH_PERF";
        case AEC_MODE_FD_LOW_COST:
            return "FD_LOW_COST";
        case AEC_MODE_FD_HIGH_PERF:
            return "FD_HIGH_PERF";
        default:
            return "UNKNOWN";
    }
}

const char* AecNlpLevelName(aec_nlp_level_t level) {
    switch (level) {
        case AEC_NLP_LEVEL_NORMAL:
            return "NORMAL";
        case AEC_NLP_LEVEL_AGGR:
            return "AGGR";
        case AEC_NLP_LEVEL_VERYAGGR:
            return "VERYAGGR";
        default:
            return "UNKNOWN";
    }
}
}  // namespace

#if CONFIG_USE_AUDIO_PROCESSOR
static constexpr bool kUseAfeForVoiceProcessing = true;
#else
static constexpr bool kUseAfeForVoiceProcessing = false;
#endif

#if CONFIG_USE_DEVICE_AEC
static constexpr bool kUseDeviceAec = true;
#else
static constexpr bool kUseDeviceAec = false;
#endif

AfeAudioEngine::AfeAudioEngine() {
    event_group_ = xEventGroupCreate();
    xEventGroupSetBits(event_group_, kWakeWordAecEnabled);
}

AfeAudioEngine::~AfeAudioEngine() {
    custom_wake_word_.reset();
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool AfeAudioEngine::Initialize(AudioCodec* codec, int frame_duration_ms,
                                srmodel_list_t* models_list) {
    if (afe_data_ != nullptr || (codec_ != nullptr && !kUseAfeForVoiceProcessing &&
                                 wake_detector_ == WakeDetector::kNone)) {
        return true;
    }
    if (codec == nullptr) {
        ESP_LOGE(TAG, "Codec is null");
        return false;
    }

    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
    } else {
        models_ = models_list;
    }

    char* wakenet_model_name = nullptr;
    char* multinet_model_name = nullptr;
    if (models_ != nullptr && models_->num > 0) {
        wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
        multinet_model_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, nullptr);
        for (int i = 0; i < models_->num; ++i) {
            ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        }
    }

    if (multinet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kMultiNet;
        custom_wake_word_ = std::make_unique<CustomWakeWord>();
        custom_wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            last_detected_wake_word_ = wake_word;
            xEventGroupClearBits(event_group_, kWakeWordEnabled);
            UpdateActiveState();
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(wake_word);
            }
        });
        if (!custom_wake_word_->Initialize(codec_, models_)) {
            ESP_LOGE(TAG, "Failed to initialize MultiNet wake-word detector");
            custom_wake_word_.reset();
            wake_detector_ = WakeDetector::kNone;
            return false;
        }
    } else if (wakenet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kWakeNet;
        auto words = esp_srmodel_get_wake_words(models_, wakenet_model_name);
        if (words != nullptr) {
            std::stringstream stream(words);
            std::string word;
            while (std::getline(stream, word, ';')) {
                wake_words_.push_back(word);
            }
        }
#if CONFIG_SEND_WAKE_WORD_DATA
        if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
            ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
        }
#endif
    }

    const bool needs_afe = kUseAfeForVoiceProcessing || wake_detector_ != WakeDetector::kNone;
    if (!needs_afe) {
        ESP_LOGI(TAG, "Initialized as raw engine because AFE features are disabled");
        return true;
    }

    int ref_num = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; ++i) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; ++i) {
        input_format.push_back('R');
    }
    ESP_LOGI(TAG, "AFE input layout: input_channels=%d, input_reference=%d, input_format=%s",
             codec_->input_channels(), codec_->input_reference(), input_format.c_str());

    char* vad_model_name =
        models_ == nullptr ? nullptr : esp_srmodel_filter(models_, ESP_VADN_PREFIX, nullptr);
    // Waveshare's dual-microphone pipeline uses FD so the second microphone is
    // processed by the AFE's speech-enhancement/BSS stage. Keep the existing VC
    // profile for the single-microphone boards.
    const bool dual_mic_with_reference = input_format == "MMR";
    const afe_type_t afe_type = dual_mic_with_reference ? AFE_TYPE_FD : AFE_TYPE_VC;
    const afe_mode_t afe_mode =
        dual_mic_with_reference ? AFE_MODE_LOW_COST : AFE_MODE_HIGH_PERF;
    ESP_LOGI(TAG, "AFE profile: type=%s, mode=%s", dual_mic_with_reference ? "FD" : "VC",
             dual_mic_with_reference ? "LOW_COST" : "HIGH_PERF");
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, afe_type, afe_mode);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE configuration");
        return false;
    }

    // Keep MMR (Mic1/Mic2/Ref) channel ordering for dual-mic BSS even when the
    // device-AEC feature is compiled out. In that build AEC must not be created,
    // so a later runtime state transition cannot accidentally enable it.
    afe_config->aec_init = codec_->input_reference() && kUseDeviceAec;
    if (dual_mic_with_reference) {
        // MMR A/B test: reduce NLP suppression; retain the FD/SE profile defaults.
        afe_config->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;
    } else {
        afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
        afe_config->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;
    }
    afe_config->ns_init = false;
    afe_config->vad_init = kUseAfeForVoiceProcessing;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }
    afe_config->wakenet_init = wake_detector_ == WakeDetector::kWakeNet;
    afe_config->wakenet_model_name =
        wake_detector_ == WakeDetector::kWakeNet ? wakenet_model_name : nullptr;
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (afe_iface_ != nullptr) {
        afe_data_ = afe_iface_->create_from_config(afe_config);
    }
    if (afe_data_ != nullptr) {
        // Report the configuration object after creation, not a claim about
        // later runtime enable/disable state. print_pipeline reports the graph.
        ESP_LOGI(TAG,
                 "AFE init config: aec_init=%d aec_mode=%s(%d) aec_nlp_level=%s(%d) "
                 "aec_filter_length=%d se_init=%d ns_init=%d agc_init=%d",
                 afe_config->aec_init, AecModeName(afe_config->aec_mode),
                 static_cast<int>(afe_config->aec_mode),
                 AecNlpLevelName(afe_config->aec_nlp_level),
                 static_cast<int>(afe_config->aec_nlp_level), afe_config->aec_filter_length,
                 afe_config->se_init, afe_config->ns_init, afe_config->agc_init);
    }
    afe_config_free(afe_config);

    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create FD AFE instance");
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return false;
    }

    // The engine's interleaved PCM is signed 16-bit at 16 kHz, even when
    // the codec runs at another rate. Reject incompatible AFE instances once
    // at startup; do not add logging or validation to the per-frame path.
    const int feed_channels = afe_iface_->get_feed_channel_num(afe_data_);
    const int feed_rate = afe_iface_->get_samp_rate(afe_data_);
    const int feed_frames = afe_iface_->get_feed_chunksize(afe_data_);
    const int fetch_frames = afe_iface_->get_fetch_chunksize(afe_data_);
    if (feed_channels <= 0 || feed_channels != codec_->input_channels() ||
        feed_rate != 16000 || feed_frames <= 0 || fetch_frames <= 0) {
        ESP_LOGE(TAG,
                 "AFE input contract mismatch: channels=%d expected=%d rate=%d expected=16000 "
                 "feed_frames=%d fetch_frames=%d",
                 feed_channels, codec_->input_channels(), feed_rate, feed_frames, fetch_frames);
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG,
             "AFE input verified: format=%s channels=%d rate=%d PCM=int16/interleaved "
             "feed_frames=%d fetch_frames=%d",
             input_format.c_str(), feed_channels, feed_rate, feed_frames, fetch_frames);

    if (wake_detector_ == WakeDetector::kWakeNet) {
        afe_iface_->disable_wakenet(afe_data_);
    }
    if (codec_->input_reference()) {
        afe_iface_->disable_aec(afe_data_);
    }
    afe_iface_->print_pipeline(afe_data_);

    BaseType_t task_created = xTaskCreate(
        [](void* arg) {
            auto* engine = static_cast<AfeAudioEngine*>(arg);
            engine->ProcessingTask();
            vTaskDelete(nullptr);
        },
        "audio_afe", 4096, this, 3, &processing_task_);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE processing task");
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    const char* detector = wake_detector_ == WakeDetector::kWakeNet
                               ? "WakeNet"
                               : (wake_detector_ == WakeDetector::kMultiNet ? "MultiNet" : "none");
    ESP_LOGI(TAG, "Initialized FD AFE, detector: %s, NS: off, feed: %d, fetch: %d", detector,
             afe_iface_->get_feed_chunksize(afe_data_), afe_iface_->get_fetch_chunksize(afe_data_));
    return true;
}

void AfeAudioEngine::Feed(std::vector<int16_t>&& data) {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if ((bits & kVoiceProcessingEnabled) && !kUseAfeForVoiceProcessing) {
        OutputRawAudio(data);
    }
    if (afe_data_ == nullptr || (bits & kAfeActive) == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
#if CONFIG_AUDIO_DIAGNOSTICS
        AudioDiagnostics::Get().Count(AudioDiagnostics::FeedStart);
#endif
        afe_iface_->feed(afe_data_, input_buffer_.data());
#if CONFIG_AUDIO_DIAGNOSTICS
        AudioDiagnostics::Get().Count(AudioDiagnostics::FeedDone);
#endif
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

void AfeAudioEngine::EnableWakeWordDetection(bool enable) {
    if (!HasWakeWord()) {
        return;
    }

    // WakeNet enable/disable on the AFE instance is applied by ProcessingTask
    // (see ApplyAfeControls), driven by the kWakeWordEnabled bit.
    if (enable) {
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Start();
        }
        xEventGroupSetBits(event_group_, kWakeWordEnabled);
    } else {
        xEventGroupClearBits(event_group_, kWakeWordEnabled);
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Stop();
        }
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableVoiceProcessing(bool enable) {
    if (enable) {
        xEventGroupSetBits(event_group_, kVoiceProcessingEnabled);
    } else {
        xEventGroupClearBits(event_group_, kVoiceProcessingEnabled);
        is_speaking_ = false;
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableDeviceAec(bool enable) {
    device_aec_enabled_.store(enable && kUseDeviceAec);
    if (enable && !kUseDeviceAec) {
        ESP_LOGW(TAG, "Device AEC is compiled out");
        return;
    }
    if (enable && (codec_ == nullptr || !codec_->input_reference())) {
        ESP_LOGW(TAG, "Device AEC requires a playback reference channel");
    }
    UpdateAecState();
}

void AfeAudioEngine::EnableWakeWordAec(bool enable) {
    if (enable) {
        xEventGroupSetBits(event_group_, kWakeWordAecEnabled);
    } else {
        xEventGroupClearBits(event_group_, kWakeWordAecEnabled);
    }
    UpdateAecState();
}

bool AfeAudioEngine::HasWakeWord() const { return wake_detector_ != WakeDetector::kNone; }

bool AfeAudioEngine::IsWakeWordDetectionEnabled() const {
    return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & kWakeWordEnabled) != 0;
}

bool AfeAudioEngine::IsVoiceProcessingEnabled() const {
    return event_group_ != nullptr &&
           (xEventGroupGetBits(event_group_) & kVoiceProcessingEnabled) != 0;
}

size_t AfeAudioEngine::GetFeedSize() const {
    return afe_data_ == nullptr ? frame_samples_ : afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioEngine::OnWakeWordDetected(
    std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = std::move(callback);
}

void AfeAudioEngine::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void AfeAudioEngine::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

void AfeAudioEngine::UpdateActiveState() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    const bool afe_active =
        afe_data_ != nullptr && ((bits & kWakeWordEnabled) ||
                                 (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)));
    if (afe_active) {
        xEventGroupSetBits(event_group_, kAfeActive);
    } else {
        xEventGroupClearBits(event_group_, kAfeActive);
        control_generation_.fetch_add(1);
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
        if (afe_data_ != nullptr) {
            // Don't call reset_buffer() here: this runs in the main task while
            // ProcessingTask may be inside fetch_with_delay() on the same AFE
            // instance, and a concurrent reset corrupts the ring buffer state.
            // Defer the reset to ProcessingTask, which owns the fetch side.
            reset_pending_ = true;
        }
    }
    if ((bits & kVoiceProcessingEnabled) == 0) {
        // output_buffer_ is owned by the task that produces output frames,
        // so let that task clear it instead of racing with it here.
        output_reset_pending_ = true;
    }
    afe_control_dirty_ = true;
#if CONFIG_AUDIO_DIAGNOSTICS
    AudioDiagnostics::Get().AfeBits(xEventGroupGetBits(event_group_));
#endif
}

void AfeAudioEngine::UpdateAecState() {
    if (afe_data_ == nullptr || codec_ == nullptr || !codec_->input_reference()) {
        return;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::ApplyAfeControls() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if (wake_detector_ == WakeDetector::kWakeNet) {
        if (bits & kWakeWordEnabled) {
            afe_iface_->enable_wakenet(afe_data_);
        } else {
            afe_iface_->disable_wakenet(afe_data_);
        }
    }
    if (codec_->input_reference() && kUseDeviceAec) {
        const bool enable_aec = ((bits & kWakeWordEnabled) && (bits & kWakeWordAecEnabled)) ||
                                (device_aec_enabled_.load() && (bits & kVoiceProcessingEnabled));
        if (enable_aec) {
            afe_iface_->enable_aec(afe_data_);
        } else {
            afe_iface_->disable_aec(afe_data_);
        }
    }
#if CONFIG_AUDIO_DIAGNOSTICS
    AudioDiagnostics::Get().AppliedControls(
        wake_detector_ == WakeDetector::kWakeNet && (bits & kWakeWordEnabled),
        codec_->input_reference() && kUseDeviceAec &&
            (((bits & kWakeWordEnabled) && (bits & kWakeWordAecEnabled)) ||
             (device_aec_enabled_.load() && (bits & kVoiceProcessingEnabled))));
#endif
}

void AfeAudioEngine::ApplyPendingReset() {
    if (!reset_pending_.exchange(false)) {
        return;
    }
    // Discard audio recorded before (re)activation. Holding input_buffer_mutex_
    // serializes the reset against Feed(); fetch/reset both run in this task.
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
    afe_iface_->reset_buffer(afe_data_);
}

void AfeAudioEngine::ProcessingTask() {
    while (true) {
        xEventGroupWaitBits(event_group_, kAfeActive, pdFALSE, pdTRUE, portMAX_DELAY);
        ApplyPendingReset();
        if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            continue;
        }
        if (afe_control_dirty_.exchange(false)) {
            // WakeNet/AEC toggles are not safe against a concurrent fetch,
            // so they are applied here, in the task that owns the fetch side.
            ApplyAfeControls();
        }
        const uint32_t generation = control_generation_.load();
#if CONFIG_AUDIO_DIAGNOSTICS
        AudioDiagnostics::Get().Count(AudioDiagnostics::FetchStart);
#endif
        auto* result = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
#if CONFIG_AUDIO_DIAGNOSTICS
        AudioDiagnostics::Get().Count(AudioDiagnostics::FetchDone);
#endif
        if (generation != control_generation_.load() ||
            (xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            // A disable/re-enable may make an old blocked fetch return after the
            // AFE is active again. Reset immediately and never process that frame.
#if CONFIG_AUDIO_DIAGNOSTICS
            AudioDiagnostics::Get().Count(AudioDiagnostics::FetchDiscard);
#endif
            ApplyPendingReset();
            continue;
        }
        if (result == nullptr || result->ret_value == ESP_FAIL) {
#if CONFIG_AUDIO_DIAGNOSTICS
            AudioDiagnostics::Get().Count(AudioDiagnostics::FetchError);
#endif
            if (result != nullptr) {
                ESP_LOGW(TAG, "AFE fetch failed: %d", result->ret_value);
            }
            continue;
        }

#if CONFIG_AUDIO_DIAGNOSTICS
        AudioDiagnostics::Get().FetchResult(result->vad_state, result->wakeup_state);
        AudioDiagnostics::Get().Observe(AudioDiagnostics::Afe, result->data,
                                        result->data_size / sizeof(int16_t), 1, 16000);
#endif
        EventBits_t bits = xEventGroupGetBits(event_group_);
        if (bits & kWakeWordEnabled) {
            HandleWakeWordResult(result);
        }
        if (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)) {
            HandleVoiceResult(result);
        }
    }
}

void AfeAudioEngine::HandleWakeWordResult(const afe_fetch_result_t* result) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->FeedMono(result->data, result->data_size / sizeof(int16_t));
        return;
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    wake_word_audio_cache_.Store(result->data, result->data_size / sizeof(int16_t));
#endif
    if (result->wakeup_state != WAKENET_DETECTED) {
        return;
    }

    int model_index = result->wakenet_model_index - 1;
    if (model_index < 0 || model_index >= static_cast<int>(wake_words_.size())) {
        ESP_LOGE(TAG, "Invalid WakeNet model index: %d", result->wakenet_model_index);
        return;
    }

    last_detected_wake_word_ = wake_words_[model_index];
#if CONFIG_AUDIO_DIAGNOSTICS
    AudioDiagnostics::Get().Count(AudioDiagnostics::WakeDetected);
#endif
    xEventGroupClearBits(event_group_, kWakeWordEnabled);
    // UpdateActiveState marks the AFE controls dirty; the next loop iteration
    // of ProcessingTask disables WakeNet via ApplyAfeControls.
    UpdateActiveState();
    if (wake_word_detected_callback_) {
        wake_word_detected_callback_(last_detected_wake_word_);
    }
}

void AfeAudioEngine::HandleVoiceResult(const afe_fetch_result_t* result) {
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    if (vad_state_change_callback_) {
        if (result->vad_state == VAD_SPEECH && !is_speaking_) {
            is_speaking_ = true;
            vad_state_change_callback_(true);
        } else if (result->vad_state == VAD_SILENCE && is_speaking_) {
            is_speaking_ = false;
            vad_state_change_callback_(false);
        }
    }
    if (!output_callback_) {
        return;
    }

    size_t samples = result->data_size / sizeof(int16_t);
    output_buffer_.insert(output_buffer_.end(), result->data, result->data + samples);
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(output_buffer_.begin(),
                                                  output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

void AfeAudioEngine::OutputRawAudio(const std::vector<int16_t>& data) {
    if (!output_callback_ || codec_ == nullptr) {
        return;
    }
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    const size_t channels = codec_->input_channels();
    if (channels <= 1) {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    } else {
        for (size_t i = 0; i < data.size(); i += channels) {
            output_buffer_.push_back(data[i]);
        }
    }
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        output_callback_(
            std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
        output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
    }
}

void AfeAudioEngine::EncodeWakeWordData() {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->EncodeWakeWordData();
        return;
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return;
    }

    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ =
            static_cast<StackType_t*>(heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic(
        [](void* arg) {
            auto* engine = static_cast<AfeAudioEngine*>(arg);
            auto start_time = esp_timer_get_time();
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret =
                esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create wake-word encoder: %d", ret);
                engine->wake_word_audio_cache_.Clear();
                {
                    std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                    engine->wake_word_opus_.emplace_back();
                    engine->wake_word_cv_.notify_all();
                }
                vTaskDelete(nullptr);
                return;
            }

            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size /= sizeof(int16_t);
            int packets = 0;
            std::vector<int16_t> input(frame_size);
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};

            const size_t cached_samples = engine->wake_word_audio_cache_.Size();
            for (size_t offset = 0; offset + static_cast<size_t>(frame_size) <= cached_samples;
                 offset += frame_size) {
                if (engine->wake_word_audio_cache_.Read(offset, input.data(), frame_size) !=
                    static_cast<size_t>(frame_size)) {
                    break;
                }
                std::vector<uint8_t> opus_buf(outbuf_size);
                in.buffer = reinterpret_cast<uint8_t*>(input.data());
                in.len = frame_size * sizeof(int16_t);
                out.buffer = opus_buf.data();
                out.len = outbuf_size;
                out.encoded_bytes = 0;
                ret = esp_opus_enc_process(encoder_handle, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                    engine->wake_word_opus_.emplace_back(opus_buf.data(),
                                                         opus_buf.data() + out.encoded_bytes);
                    engine->wake_word_cv_.notify_all();
                    ++packets;
                } else {
                    ESP_LOGE(TAG, "Failed to encode wake-word audio: %d", ret);
                }
            }

            engine->wake_word_audio_cache_.Clear();
            esp_opus_enc_close(encoder_handle);
            ESP_LOGI(TAG, "Encoded wake word into %d packets in %ld ms", packets,
                     static_cast<long>((esp_timer_get_time() - start_time) / 1000));
            {
                std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                engine->wake_word_opus_.emplace_back();
                engine->wake_word_cv_.notify_all();
            }
            vTaskDelete(nullptr);
        },
        "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_,
        wake_word_encode_task_buffer_);
}

bool AfeAudioEngine::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        return custom_wake_word_->GetWakeWordOpus(opus);
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return false;
    }
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
