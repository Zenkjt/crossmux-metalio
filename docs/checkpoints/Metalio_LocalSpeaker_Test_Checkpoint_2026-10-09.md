Metalio E-Ink 4 — Local Speaker Test Checkpoint
Date: 2026-10-09
Branch: metalio/sc7a20h
Repository: Zenkjt/crossmux-metalio
Decision
Test the Metalio E-Ink 4 local external speaker path first, without Bluetooth pairing and without integrating generic CrossMux SoundFeedback.
Source basis
- CrossMux branch already has a Metalio target:
  platformio.ini defines metalio_eink4_hardware with
  FREEINK_DEVICE_METALIO_EINK4=1.
- The pinned freeink-sdk gitlink is commit
  96de1be6ce08eb732909e6e8149af8f892b9a2c5.
- The SDK already contains MetalioAudio.h, which implements the Metalio-specific
  audio bus:
  - UART2 at 115200 on RX=47 / TX=48.
  - AT+RX=2, 700 ms delay, then AT+MODE=1.
  - I2S0 in slave mode.
  - 16 kHz audio.
  - BCLK / WS / DOUT / DIN are taken from the Metalio board profile.
- The SDK's MetalioEink4Board.h defines the TCA9555 amplifier routing:
  PIN_AMP_SELECT=1, PIN_AMP_ENABLE=4.
  setAmplifier(false) selects the ESP32/I2S path and disables the amplifier;
  setAmplifier(true) enables the local amplifier.
- MetalioPcm.h preserves the vendor's squared software-volume conversion and
  32-bit I2S sample representation.
Why this route
Do not duplicate the vendor I2S/UART implementation in CrossMux. Reuse the
existing SDK Metalio audio abstraction and add only a one-shot test consumer.
Do not use generic HalAudioOutput yet. The current CrossMux implementation is
gated by CROSSPOINT_CAP_SOUND_FEEDBACK, while the Metalio target does not opt
into that capability and the generic AudioManager path is designed around
other codec types. A dedicated Metalio bring-up test is the smaller and safer
change.
Files
Create:
- src/metalio/MetalioSpeakerTest.h
- src/metalio/MetalioSpeakerTest.cpp
Modify src/main.cpp with two small conditional include/call blocks (each under
5 changed lines).
Test behavior
At boot, after activityManager.begin():
1. Acquire the Metalio speaker audio bus.
2. Put the external audio module into the vendor mode-1 clock/audio state.
3. Start I2S TX as a slave at 16 kHz.
4. Route the amplifier to the ESP32/I2S path and enable it.
5. Play a ~500 ms, 1 kHz square-wave tone.
6. Disable the amplifier and release the audio bus.
The test uses a static 160-frame stereo buffer and no heap allocation.
Acceptance
Pending physical test.
Expected successful serial sequence:
- METALIO-AUDIO: Starting local speaker hardware test
- METALIO-AUDIO: Speaker path active: ...
- METALIO-AUDIO: Local speaker test completed
Physical acceptance:
- [ ] Audible ~500 ms tone from the Metalio external speaker.
- [ ] No crash/panic.
- [ ] No persistent amplifier-on state after the test.
- [ ] No Bluetooth pairing required.
If the write times out, inspect the UART/I2S clock behavior before changing the
CrossMux audio architecture. Because the I2S controller is configured as a
slave, the external Metalio audio module must actually provide the I2S clock.
Working rules applied
The project source-of-truth requires evidence-based changes, HAL/SDK layering,
resource justification, verification instructions, and checkpoint documentation.
No patch is used; the changes are intended for GitHub Web copy/create + user
commit.
