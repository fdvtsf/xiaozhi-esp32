# Wiring reminder

- For this board, DI/DO connector labels use the ESP32 development board's perspective, not the audio codec's: DI = GPIO7 = microphone input; DO = GPIO9 = playback output.
- When explaining wiring or flashing this board, explicitly remind the user that this direction is easy to reverse. Do not revert to the earlier DIN=9 / DOUT=7 assumption.
- Keep `config.h`, the wiring table in `README.md`, and `scripts/tests/test_holomate_waveshare.py` consistent. Leave the original HoloMate board's pins unchanged.
