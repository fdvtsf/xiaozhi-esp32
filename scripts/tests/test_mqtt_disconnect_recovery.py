from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
MQTT_PROTOCOL = ROOT / "main/protocols/mqtt_protocol.cc"
MQTT_HEADER = ROOT / "main/protocols/mqtt_protocol.h"
APPLICATION = ROOT / "main/application.cc"


class MqttDisconnectRecoveryTests(unittest.TestCase):
    def test_disconnect_closes_audio_channel_on_application_task(self):
        source = MQTT_PROTOCOL.read_text(encoding="utf-8")
        match = re.search(
            r"void MqttProtocol::HandleMqttDisconnected\(\)\s*\{(?P<body>.*?)\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        body = match.group("body")
        self.assertIn("Application::GetInstance().Schedule", body)
        self.assertIn("CloseAudioChannel(false)", body)
        self.assertIn("ScheduleReconnect()", body)
        self.assertLess(body.index("CloseAudioChannel(false)"),
                        body.index("ScheduleReconnect()"))

    def test_mqtt_callback_uses_disconnect_recovery(self):
        source = MQTT_PROTOCOL.read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"mqtt_->OnDisconnected\(\[this\]\(\)\s*\{\s*"
            r"HandleMqttDisconnected\(\);\s*\}\);",
        )

    def test_reconnect_is_fast_and_retries(self):
        header = MQTT_HEADER.read_text(encoding="utf-8")
        source = MQTT_PROTOCOL.read_text(encoding="utf-8")
        interval = re.search(r"MQTT_RECONNECT_INTERVAL_MS\s+(\d+)", header)
        self.assertIsNotNone(interval)
        self.assertLessEqual(int(interval.group(1)), 3000)
        self.assertIn("if (!protocol->StartMqttClient(false))", source)
        self.assertIn("protocol->ScheduleReconnect();", source)
        self.assertIn("Failed to start MQTT reconnect timer", source)

    def test_application_restores_active_conversation_without_wake_word(self):
        source = APPLICATION.read_text(encoding="utf-8")
        self.assertIn("protocol_->OnDisconnected", source)
        self.assertIn("conversation_recovery_pending_ = true", source)
        self.assertIn("MQTT transport restored; reopening conversation automatically", source)
        self.assertIn("ContinueConversationRecovery", source)
        self.assertIn("SetListeningMode(recovered_mode)", source)

    def test_user_action_cancels_pending_recovery(self):
        source = APPLICATION.read_text(encoding="utf-8")
        wake_handler = re.search(
            r"void Application::BeginWakeWordInvoke\([^)]*\)\s*\{(?P<body>.*?)\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(wake_handler)
        self.assertIn("conversation_recovery_pending_ = false", wake_handler.group("body"))


if __name__ == "__main__":
    unittest.main()
