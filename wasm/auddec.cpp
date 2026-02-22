#include "moonlight_wasm.hpp"

#include <vector>

#include <AL/al.h>
#include <AL/alc.h>

static ALCdevice*  s_AlDevice  = nullptr;
static ALCcontext* s_AlContext = nullptr;
static ALuint      s_AlSource  = 0;
static std::vector<ALuint> s_AlBuffers;

static size_t  s_samplesPerFrame = 0;
static size_t  s_channelCount    = 0;
static ALenum  s_alFormat        = 0;
static ALsizei s_sampleRate      = 0;

static std::vector<opus_int16> s_DecodeBuffer;

// Target jitter protection in wall-clock ms. Both the jitter queue depth and the AL
// buffer pool size are computed from this at AudDecInit time so they scale correctly
// regardless of the negotiated AudioPacketDuration (5 / 10 / 20 ms frames).
// Overridden at session start by g_AudioJitterMsOverride (0 = use default of 100 ms).
extern int g_AudioJitterMsOverride;
static int s_jitterFrames = 0;  // = ceil(targetJitterMs / frameDurationMs), set at init
static int s_numBuffers   = 0;  // = s_jitterFrames (AL pool matches jitter depth)

// Fixed-capacity ring buffer: one flat allocation of (s_jitterFrames + 1) frames.
// Eliminates per-frame heap allocation; head/tail indices wrap modulo s_ringCap.
//
// Memory layout (s_ringCap = 5 slots shown as example):
//
//   slot:   0     1     2     3     4
//         +-----+-----+-----+-----+-----+
//         |     | F0  | F1  | F2  |     |
//         +-----+-----+-----+-----+-----+
//                  ^                 ^
//              s_ringHead        s_ringTail
//           (oldest, next        (next empty
//            to submit)           write slot)
//              s_ringSize = 3
//
// Indices always wrap modulo s_ringCap so the buffer is used circularly.
// s_ringTail always points to the next EMPTY slot; the most-recently-written
// frame lives at (s_ringTail - 1 + s_ringCap) % s_ringCap.
static std::vector<opus_int16> s_ringBuffer;
static size_t s_frameElems = 0;  // elements per frame = samplesPerFrame * channelCount
static int    s_ringHead   = 0;
static int    s_ringTail   = 0;
static int    s_ringSize   = 0;
static int    s_ringCap    = 0;

// Copy one frame into the slot at s_ringTail, then advance tail.
//
//   Before: [ F0 | F1 | F2 |    |    ]   size=3
//                              ^tail
//   After:  [ F0 | F1 | F2 | F3 |    ]   size=4
//                                   ^tail
static inline void ringPushBack(const opus_int16* data) {
  opus_int16* slot = s_ringBuffer.data() + (size_t)s_ringTail * s_frameElems;
  __builtin_memcpy(slot, data, s_frameElems * sizeof(opus_int16));
  s_ringTail = (s_ringTail + 1) % s_ringCap;
  s_ringSize++;
}

// Discard the oldest frame by advancing head. The slot is not zeroed;
// it will be overwritten by the next ringPushBack that wraps around to it.
//
//   Before: [ F0 | F1 | F2 | F3 |    ]   size=4
//             ^head
//   After:  [    | F1 | F2 | F3 |    ]   size=3
//                  ^head
static inline void ringPopFront() {
  s_ringHead = (s_ringHead + 1) % s_ringCap;
  s_ringSize--;
}

// Discard the newest frame by retreating tail one slot.
// Used when AL has no free buffers and we must drop: preserves the oldest
// frames queued at head (which are next for AL) and drops the one just arrived.
//
//   Before: [ F0 | F1 | F2 | F3 |    ]   size=4
//                                   ^tail
//   After:  [ F0 | F1 | F2 |    |    ]   size=3
//                              ^tail
static inline void ringPopBack() {
  s_ringTail = (s_ringTail - 1 + s_ringCap) % s_ringCap;
  s_ringSize--;
}

// Return a pointer to the oldest frame's samples (at s_ringHead) without
// removing it. The caller passes this directly to alBufferData.
//
//   [ F0 | F1 | F2 |    |    ]
//     ^head → returns pointer here
static inline const opus_int16* ringFront() {
  return s_ringBuffer.data() + (size_t)s_ringHead * s_frameElems;
}

static bool     s_jitterReady = false;
static uint64_t s_dropCount   = 0;

int MoonlightInstance::AudDecInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  s_channelCount    = (size_t)opusConfig->channelCount;
  s_samplesPerFrame = (size_t)opusConfig->samplesPerFrame;
  s_sampleRate      = (ALsizei)opusConfig->sampleRate;

  // Compute frame count needed to cover targetJitterMs (ceiling division)
  int targetJitterMs = (g_AudioJitterMsOverride != 0) ? g_AudioJitterMsOverride : 100;
  s_jitterFrames = (targetJitterMs * (int)s_sampleRate + (int)s_samplesPerFrame * 1000 - 1)
                   / ((int)s_samplesPerFrame * 1000);
  s_numBuffers = s_jitterFrames;

  int frameDurationMs = (int)s_samplesPerFrame * 1000 / (int)s_sampleRate;
  ClLogMessage("AudDecInit: ch=%d samplesPerFrame=%d sampleRate=%d jitterFrames=%d jitterMs=%d (target=%dms)\n",
    opusConfig->channelCount, opusConfig->samplesPerFrame, opusConfig->sampleRate,
    s_jitterFrames, s_jitterFrames * frameDurationMs, targetJitterMs);

  s_jitterReady = false;
  s_dropCount   = 0;

  // Open AL device and context
  s_AlDevice = alcOpenDevice(NULL);
  if (!s_AlDevice) {
    ClLogMessage("AudDecInit: alcOpenDevice failed\n");
    return -1;
  }
  ALCint attrs[] = { ALC_FREQUENCY, (ALCint)s_sampleRate, 0 };
  s_AlContext = alcCreateContext(s_AlDevice, attrs);
  if (!s_AlContext) {
    ClLogMessage("AudDecInit: alcCreateContext failed\n");
    alcCloseDevice(s_AlDevice);
    s_AlDevice = nullptr;
    return -1;
  }
  alcMakeContextCurrent(s_AlContext);

  ALCint actualFreq = 0;
  alcGetIntegerv(s_AlDevice, ALC_FREQUENCY, 1, &actualFreq);
  ClLogMessage("AudDecInit: AL context frequency = %d Hz (Opus sampleRate = %d Hz)\n",
    (int)actualFreq, (int)s_sampleRate);

  // Select AL format (requires active context for alGetEnumValue)
  switch (s_channelCount) {
    case 2:
      s_alFormat = AL_FORMAT_STEREO16;
      break;
    case 6: {
      ALenum fmt = alGetEnumValue("AL_FORMAT_51CHN16");
      s_alFormat = (fmt != (ALenum)-1 && fmt != 0) ? fmt : AL_FORMAT_STEREO16;
      if (s_alFormat == AL_FORMAT_STEREO16) {
        ClLogMessage("AudDecInit: 5.1 AL format unavailable, downmixing to stereo\n");
        s_channelCount = 2;
      }
      break;
    }
    case 8: {
      ALenum fmt = alGetEnumValue("AL_FORMAT_71CHN16");
      s_alFormat = (fmt != (ALenum)-1 && fmt != 0) ? fmt : AL_FORMAT_STEREO16;
      if (s_alFormat == AL_FORMAT_STEREO16) {
        ClLogMessage("AudDecInit: 7.1 AL format unavailable, downmixing to stereo\n");
        s_channelCount = 2;
      }
      break;
    }
    default:
      ClLogMessage("AudDecInit: unsupported channel count %zu, using stereo\n", s_channelCount);
      s_alFormat = AL_FORMAT_STEREO16;
      s_channelCount = 2;
      break;
  }

  // Allocate decode buffer using the Opus channel count, not the (possibly downmixed) s_channelCount,
  // so opus_multistream_decode never writes past the end of the buffer
  s_DecodeBuffer.resize(s_samplesPerFrame * (size_t)opusConfig->channelCount);

  // Allocate ring buffer: capacity is s_jitterFrames + 1 to accommodate the transient
  // state where a new frame is pushed before the oldest is popped in the same call
  s_frameElems = s_samplesPerFrame * s_channelCount;
  s_ringCap    = s_jitterFrames + 1;
  s_ringBuffer.resize((size_t)s_ringCap * s_frameElems);
  s_ringHead = 0;
  s_ringTail = 0;
  s_ringSize = 0;

  // Create AL buffers and source
  s_AlBuffers.resize(s_numBuffers);
  alGenBuffers(s_numBuffers, s_AlBuffers.data());
  alGenSources(1, &s_AlSource);
  alSourcei(s_AlSource, AL_LOOPING, AL_FALSE);

  // Pre-queue silence so playback starts immediately and covers network startup latency
  {
    size_t frameBytes = s_samplesPerFrame * s_channelCount * sizeof(opus_int16);
    std::vector<opus_int16> silence(s_samplesPerFrame * s_channelCount, 0);
    for (ALuint buf : s_AlBuffers) {
      alBufferData(buf, s_alFormat, silence.data(), (ALsizei)frameBytes, s_sampleRate);
      alSourceQueueBuffers(s_AlSource, 1, &buf);
    }
  }
  alSourcePlay(s_AlSource);
  ClLogMessage("AudDecInit: AL source playing with %d silence buffers\n", s_numBuffers);

  // Create Opus decoder
  int rc;
  g_Instance->m_OpusDecoder = opus_multistream_decoder_create(
    opusConfig->sampleRate, opusConfig->channelCount,
    opusConfig->streams, opusConfig->coupledStreams,
    opusConfig->mapping, &rc
  );
  ClLogMessage("AudDecInit: opus_multistream_decoder_create rc=%d\n", rc);
  if (!g_Instance->m_OpusDecoder) {
    ClLogMessage("AudDecInit: opus decoder creation failed, tearing down AL\n");
    AudDecCleanup();
    return -1;
  }
  return 0;
}

void MoonlightInstance::AudDecCleanup(void) {
  ClLogMessage("AudDecCleanup\n");

  s_ringBuffer.clear();
  s_ringBuffer.shrink_to_fit();
  s_ringHead = 0;
  s_ringTail = 0;
  s_ringSize = 0;
  s_ringCap  = 0;

  if (s_AlSource) {
    alSourceStop(s_AlSource);
    ALint queued = 0;
    alGetSourcei(s_AlSource, AL_BUFFERS_QUEUED, &queued);
    if (queued > 0) {
      std::vector<ALuint> tmp((size_t)queued);
      alSourceUnqueueBuffers(s_AlSource, queued, tmp.data());
    }
    alDeleteSources(1, &s_AlSource);
    s_AlSource = 0;
  }
  if (!s_AlBuffers.empty()) {
    alDeleteBuffers((ALsizei)s_AlBuffers.size(), s_AlBuffers.data());
    s_AlBuffers.clear();
  }
  if (s_AlContext) {
    alcMakeContextCurrent(NULL);
    alcDestroyContext(s_AlContext);
    s_AlContext = nullptr;
  }
  if (s_AlDevice) {
    alcCloseDevice(s_AlDevice);
    s_AlDevice = nullptr;
  }
  if (g_Instance->m_OpusDecoder) {
    opus_multistream_decoder_destroy(g_Instance->m_OpusDecoder);
    g_Instance->m_OpusDecoder = nullptr;
  }
  s_DecodeBuffer.clear();
  s_DecodeBuffer.shrink_to_fit();
}

void MoonlightInstance::AudDecDecodeAndPlaySample(char* sampleData, int sampleLength) {
  if (!g_Instance->m_OpusDecoder || !s_AlSource) {
    return;
  }

  int decodeLen = opus_multistream_decode(
    g_Instance->m_OpusDecoder,
    reinterpret_cast<unsigned char*>(sampleData), sampleLength,
    s_DecodeBuffer.data(), (int)s_samplesPerFrame, 0
  );
  if (decodeLen <= 0) {
    ClLogMessage("AudDec: decode failed %d sampleLength=%d\n", decodeLen, sampleLength);
    return;
  }

  // Push decoded frame into ring buffer
  ringPushBack(s_DecodeBuffer.data());

  // Accumulate until the jitter buffer is full before feeding AL
  if (s_ringSize < s_jitterFrames) {
    return;
  }

  // Log once when the jitter buffer first reaches its target depth
  if (!s_jitterReady) {
    int fdms = (int)s_samplesPerFrame * 1000 / (int)s_sampleRate;
    ClLogMessage("AudDec: jitter buffer ready (%d frames / %dms), starting AL submission\n",
      s_jitterFrames, s_jitterFrames * fdms);
    s_jitterReady = true;
  }

  // Check how many AL buffers the source has finished playing
  ALint processed = 0;
  alGetSourcei(s_AlSource, AL_BUFFERS_PROCESSED, &processed);
  if (processed == 0) {
    // AL still busy with all queued buffers — discard the newest jitter frame (back)
    // to keep the queue at its target depth while preserving the oldest frames that
    // are next in line for AL submission, avoiding a discontinuity at the playback edge
    if (++s_dropCount <= 3 || s_dropCount % 100 == 0) {
      ClLogMessage("AudDec: no processed buffer, dropping jitter frame #%llu\n",
        (unsigned long long)s_dropCount);
    }
    ringPopBack();
    return;
  }

  // Recycle all processed AL buffers with the oldest jitter frames so AL stays
  // as full as possible — this closes any gap that opened between calls in one shot.
  // Unqueue and re-queue are batched into single calls to minimize WASM→JS crossings:
  // 3N calls (unqueue×N + bufferData×N + queue×N) become N+2 (1 + bufferData×N + 1).
  ALint count = (processed < (ALint)s_ringSize) ? processed : (ALint)s_ringSize;
  ALuint bufs[128];  // processed <= s_numBuffers <= s_jitterFrames, always fits
  alSourceUnqueueBuffers(s_AlSource, count, bufs);
  for (ALint i = 0; i < count; i++) {
    alBufferData(bufs[i], s_alFormat, ringFront(),
      (ALsizei)(s_frameElems * sizeof(opus_int16)), s_sampleRate);
    ringPopFront();
  }
  alSourceQueueBuffers(s_AlSource, count, bufs);

  // Restart if source stopped due to buffer underrun (silence pool exhausted)
  ALint state;
  alGetSourcei(s_AlSource, AL_SOURCE_STATE, &state);
  if (state != AL_PLAYING) {
    ClLogMessage("AudDec: source not playing (state=%d), restarting\n", state);
    alSourcePlay(s_AlSource);
  }
}

AUDIO_RENDERER_CALLBACKS MoonlightInstance::s_ArCallbacks = {
  .init = MoonlightInstance::AudDecInit,
  .cleanup = MoonlightInstance::AudDecCleanup,
  .decodeAndPlaySample = MoonlightInstance::AudDecDecodeAndPlaySample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
