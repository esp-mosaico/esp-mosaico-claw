# Air Voice (whisper capture) service

Test service for close-talk whisper capture (3–5 cm, often with the mouth
covered by a hand). While active it owns the microphone, runs the whisper
enhancement chain, records a processed and a raw track to
`<DATA>/air_voice/`, and plays recordings back through the audio mixer.

Consumers:

- `components/http_server/http_server_air_voice_api.c`: `/api/airvoice/*`,
  used by the web admin page (sidebar entry "Air Voice" / 气声拾音).
- `components/mosaic_ui/apps/air_voice`: the on-device screen.

```text
air_voice_service.c   worker (mic, DSP, playback) and writer (file I/O) tasks
air_voice_dsp.c       enhancement chain, portable C without ESP-IDF
air_voice_config.c    presets and persisted settings (config.json)
air_voice_store.c     recording files, streaming WAV headers, .part recovery
air_voice_player.c    WAV playback into the audio mixer
air_voice_log.c       log ring shown on the web page
air_voice_json.c      JSON for the HTTP API
test/host/            host harness and evaluation scripts
docs/                 research notes and measurements (Chinese)
```

The host harness runs the same DSP code over recorded WAV files:

```bash
cd test/host
cc -O2 -std=c11 -I../.. -o av_dsp_host air_voice_dsp_host.c ../../air_voice_dsp.c -lm
./av_dsp_host take_raw.wav out.wav [key=value ...]
python3 eval_takes.py take_raw.wav out.wav
```

See `docs/whisper_capture_research.md` for the research, the measured
3–15 cm series, the algorithm design and the test method.
