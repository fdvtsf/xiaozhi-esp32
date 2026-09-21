"""Source regression checks for the startup-only AFE contract.

Firmware compilation checks the real ESP-SR API; these tests guard ordering,
cleanup, and the absence of new work in the steady-state audio path.
"""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text(encoding='utf-8')
INIT = SOURCE.split('bool AfeAudioEngine::Initialize(', 1)[1].split(
    'void AfeAudioEngine::Feed(', 1)[0]


class AfeInitializationTests(unittest.TestCase):
    def test_mmr_normal_override_keeps_other_profile_settings(self):
        branches = INIT.split('if (dual_mic_with_reference) {', 1)[1]
        mmr, remaining = branches.split('} else {', 1)
        self.assertIn('afe_config->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;', mmr)
        self.assertNotIn('afe_config->aec_mode =', mmr)
        self.assertNotIn('afe_config->se_init =', mmr)
        other = remaining.split('}', 1)[0]
        self.assertIn('afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;', other)
        self.assertIn('afe_config->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;', other)

    def test_checks_instance_contract_before_starting_task(self):
        for api in ('get_feed_channel_num', 'get_samp_rate',
                    'get_feed_chunksize', 'get_fetch_chunksize'):
            self.assertIn(f'afe_iface_->{api}(afe_data_)', INIT)
        for condition in ('feed_channels <= 0',
                          'feed_channels != codec_->input_channels()',
                          'feed_rate != 16000', 'feed_frames <= 0', 'fetch_frames <= 0'):
            self.assertIn(condition, INIT)
        self.assertLess(INIT.index('AFE input contract mismatch'), INIT.index('xTaskCreate('))
        self.assertLess(INIT.index('Failed to create FD AFE instance'),
                        INIT.index('const int feed_channels'))

    def test_mismatch_releases_instance_and_returns_failure(self):
        failure = INIT.split('AFE input contract mismatch', 1)[1].split(
            'AFE input verified', 1)[0]
        self.assertIn('afe_iface_->destroy(afe_data_);', failure)
        self.assertIn('afe_data_ = nullptr;', failure)
        self.assertIn('afe_iface_ = nullptr;', failure)
        self.assertIn('return false;', failure)

    def test_logs_config_after_create_before_free(self):
        self.assertLess(INIT.index('create_from_config(afe_config)'),
                        INIT.index('AFE init config'))
        self.assertLess(INIT.index('AFE init config'), INIT.index('afe_config_free(afe_config)'))
        for field in ('aec_mode', 'aec_nlp_level', 'aec_filter_length',
                      'se_init', 'ns_init', 'agc_init'):
            self.assertIn(f'afe_config->{field}', INIT)
        self.assertIn('afe_iface_->print_pipeline(afe_data_);', INIT)

    def test_new_checks_and_logs_only_occur_at_startup(self):
        rest = SOURCE.split('void AfeAudioEngine::Feed(', 1)[1]
        for text in ('AFE init config', 'AFE input verified',
                     'AFE input contract mismatch', 'get_feed_channel_num', 'get_samp_rate'):
            self.assertNotIn(text, rest)


if __name__ == '__main__':
    unittest.main()
