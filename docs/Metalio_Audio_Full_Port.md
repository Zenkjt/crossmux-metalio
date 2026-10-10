# Metalio E-INK4 — Full Audio Port

## Scope

This implementation follows the agreed architecture:

- **Layer 1:** Metalio hardware/HAL remains the hardware source of truth from CloudZao.
- **Layer 2:** full audio infrastructure is ported into CrossMux:
  - hardware codec
  - input task
  - output task
  - Opus encode/decode task
  - PCM/Opus queues
  - lifecycle and power ownership
- **Layer 3:** audio-only applications:
  - Ogg Opus music playback from SD
  - 10-second recorder to Ogg Opus on SD

No STT, LLM, TTS, wake word, or XiaoZhi cloud/session layer is included.

## Format decision

Layer 3 uses **Ogg Opus** rather than MP3.

Reason:
1. CloudZao Metalio already uses Ogg Opus for recordings.
2. The existing Zao playback path parses Ogg pages and feeds Opus packets into the Layer-2 decoder.
3. The same codec therefore exercises the complete Layer-2 path instead of introducing a second unrelated decoder.
4. Opus is suitable for both interactive audio and music; the implementation uses `OPUS_APPLICATION_AUDIO`.

Source references:
- CloudZao `audio_service.h/.cc`
- CloudZao `sd_paths.h`
- CloudZao README recording layout

## Runtime path

### Playback

SD Ogg Opus
→ Ogg page parser
→ Opus packet
→ Layer-2 Opus decoder
→ playback queue
→ Layer-2 output task
→ MetalioAudioCodec
→ Metalio I2S/amplifier
→ speaker

### Recording

Metalio microphone
→ Layer-2 input task
→ PCM frame
→ Layer-2 Opus encoder
→ Ogg page writer
→ `/sdcard/metalio/e-ink/recordings/recording_XXXX.ogg`

## Test UI

The existing Metalio audio activity exposes:

1. `PLAY MUSIC.OGG`
2. `RECORD 10 SEC`
3. `1 KHZ SPEAKER TEST`

Music test file:

`/sdcard/metalio/e-ink/music/music.ogg`

The recorder writes Ogg Opus files under:

`/sdcard/metalio/e-ink/recordings/`

## Dependency

The local `MetalioAudio` library declares `pschatzmann/codec-opus` 1.6.1 as its external Opus dependency through `library.json`.

No global AudioManager path is used for Metalio because the generic manager configures I2S as master/16-bit, while Metalio requires I2S0 slave/32-bit stereo at 16 kHz with the external module supplying clocks.

## Verification status

Implementation prepared; **not a checkpoint**.

A checkpoint must only be created after:
1. successful CI build,
2. full firmware image creation,
3. flash to real Metalio E-INK4,
4. speaker test,
5. microphone recording test,
6. recorded Ogg playback test.
