#include "moonlight_wasm.hpp"

#include <emscripten.h>

// g_AudioJitterMsOverride == 0  →  use default of 100 ms.
extern int g_AudioJitterMsOverride;

static size_t s_samplesPerFrame = 0;
static size_t s_channelCount    = 0;
static int    s_sampleRate      = 0;

static OpusMSDecoder* s_OpusDecoder = nullptr;

// Decoded PCM slot pool — rotating slots so the main thread has time to read
// HEAP16 at the pointer before we overwrite it with the next frame.
// 32 slots × frame duration (5/10/20ms) = 160–640ms protection window.
static constexpr int kNumSlots      = 32;
static constexpr int kMaxFrameElems = 4096;  // 480 * 8 ch = 3840, rounded up
static opus_int16    s_frameSlots[kNumSlots][kMaxFrameElems];
static int           s_slotIdx = 0;

int MoonlightInstance::AudDecInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  s_channelCount    = (size_t)opusConfig->channelCount;
  s_samplesPerFrame = (size_t)opusConfig->samplesPerFrame;
  s_sampleRate      = opusConfig->sampleRate;
  s_slotIdx         = 0;

  int rc;
  s_OpusDecoder = opus_multistream_decoder_create(
    opusConfig->sampleRate, opusConfig->channelCount,
    opusConfig->streams, opusConfig->coupledStreams,
    opusConfig->mapping, &rc);
  g_Instance->m_OpusDecoder = s_OpusDecoder;
  if (!s_OpusDecoder) {
    return -1;
  }

  // Publish the UI jitter buffer setting to the JS audio scheduler.
  int targetJitterMs = (g_AudioJitterMsOverride != 0) ? g_AudioJitterMsOverride : 100;
  MAIN_THREAD_ASYNC_EM_ASM({
    window._mlAudioTargetMs = $0;
  }, targetJitterMs);

  return 0;
}

void MoonlightInstance::AudDecCleanup(void) {
  MAIN_THREAD_ASYNC_EM_ASM({
    if (typeof stopAudioScheduler === 'function') stopAudioScheduler();
  });

  if (s_OpusDecoder) {
    opus_multistream_decoder_destroy(s_OpusDecoder);
    s_OpusDecoder = nullptr;
  }
}

// Called by moonlight-common-c on every received audio packet.
// Decodes Opus inline and posts the PCM to the JS Web Audio scheduler.
void MoonlightInstance::AudDecDecodeAndPlaySample(char* sampleData, int sampleLength) {
  if (!s_OpusDecoder) return;

  opus_int16* dst = s_frameSlots[s_slotIdx % kNumSlots];
  int n = opus_multistream_decode(
    s_OpusDecoder, (const unsigned char*)sampleData, sampleLength,
    dst, (int)s_samplesPerFrame, 0);
  if (n <= 0) return;

  int slotPtr = (int)(size_t)dst;
  int spf     = (int)s_samplesPerFrame;
  int ch      = (int)s_channelCount;
  int rate    = s_sampleRate;
  MAIN_THREAD_ASYNC_EM_ASM({
    if (typeof _audReceiveFrame === 'function') _audReceiveFrame($0, $1, $2, $3);
  }, slotPtr, spf, ch, rate);
  s_slotIdx++;
}

AUDIO_RENDERER_CALLBACKS MoonlightInstance::s_ArCallbacks = {
  .init = MoonlightInstance::AudDecInit,
  .cleanup = MoonlightInstance::AudDecCleanup,
  .decodeAndPlaySample = MoonlightInstance::AudDecDecodeAndPlaySample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
