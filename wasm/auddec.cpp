#include "moonlight_wasm.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>

// ─── AL handles (owned exclusively by feeder thread after AudDecInit) ─────────
static ALCdevice*          s_AlDevice  = nullptr;
static ALCcontext*         s_AlContext = nullptr;
static ALuint              s_AlSource  = 0;
static std::vector<ALuint> s_AlBuffers;

static size_t  s_samplesPerFrame = 0;
static size_t  s_channelCount    = 0;
static ALenum  s_alFormat        = 0;
static ALsizei s_sampleRate      = 0;

// ─── Jitter / buffer sizing ───────────────────────────────────────────────────
// Target jitter protection in wall-clock ms.  Both the jitter queue depth and
// the AL buffer pool size are computed from this so they scale correctly
// regardless of the negotiated AudioPacketDuration (5 / 10 / 20 ms frames).
// g_AudioJitterMsOverride == 0 means "use default of 100 ms".
extern int g_AudioJitterMsOverride;
static int s_jitterFrames = 0;  // = ceil(targetJitterMs / frameDurationMs)
static int s_numBuffers   = 0;  // = s_jitterFrames  (AL pool depth)

static OpusMSDecoder* s_OpusDecoder = nullptr;  // copy of s_OpusDecoder

// ─── Encoded-packet queue  (network thread → feeder thread) ──────────────────
// Pre-allocated fixed-size slots avoid per-packet heap allocation.
// 4 KiB far exceeds the largest legal Opus packet (≤ 1275 B per RFC 6716).
static constexpr int kMaxPacketBytes = 4096;

struct PacketSlot {
  uint8_t data[kMaxPacketBytes];
  int     length;
};

static std::vector<PacketSlot> s_pktQueue;  // circular, capacity = s_pktCap
static int  s_pktHead  = 0;
static int  s_pktTail  = 0;
static int  s_pktCount = 0;
static int  s_pktCap   = 0;
static std::mutex              s_pktMutex;
static std::condition_variable s_pktCv;

// ─── Feeder thread ────────────────────────────────────────────────────────────
static std::thread       s_feederThread;
static std::atomic<bool> s_feederRunning{false};

// ─── PCM ring buffer  (feeder thread only — no locking required) ──────────────
// Flat allocation of (s_jitterFrames + 1) decoded frames.
// Head = oldest frame ready for AL; tail = next empty write slot.
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
static std::vector<opus_int16> s_ringBuffer;
static size_t s_frameElems = 0;
static int    s_ringHead   = 0;
static int    s_ringTail   = 0;
static int    s_ringSize   = 0;
static int    s_ringCap    = 0;

static inline void ringPushBack(const opus_int16* src) {
  opus_int16* dst = s_ringBuffer.data() + (size_t)s_ringTail * s_frameElems;
  __builtin_memcpy(dst, src, s_frameElems * sizeof(opus_int16));
  s_ringTail = (s_ringTail + 1) % s_ringCap;
  ++s_ringSize;
}

static inline void ringPopFront() {
  s_ringHead = (s_ringHead + 1) % s_ringCap;
  --s_ringSize;
}

static inline void ringPopBack() {
  s_ringTail = (s_ringTail + s_ringCap - 1) % s_ringCap;
  --s_ringSize;
}

static inline const opus_int16* ringFront() {
  return s_ringBuffer.data() + (size_t)s_ringHead * s_frameElems;
}

// ─── Feeder thread body ────────────────────────────────────────────────────────
//
// Owns the AL context and the Opus decoder.  The network callback (producer)
// only pushes encoded packets to s_pktQueue; all decoding, PLC synthesis, and
// AL buffer management happen here.
//
// Main loop:
//   1. Drain the packet queue — decode each encoded packet into the PCM ring.
//   2. During startup, wait until the ring reaches s_jitterFrames depth.
//   3. Poll AL for processed (free) buffer slots.
//      - If none are free, wait up to 1 ms then re-check.
//      - If slots are free, pull real frames from the ring (or PLC if ring is
//        empty) and re-queue them to AL.  Restart the source if it stalled.
//
static void feederLoop() {
  // Take ownership of the AL context on this thread.
  alcMakeContextCurrent(s_AlContext);

  // Per-thread decode scratch buffer.
  std::vector<opus_int16> decodeBuf(s_samplesPerFrame * s_channelCount);

  bool     jitterReady  = false;
  uint64_t overflowCount = 0;
  uint64_t plcTotal      = 0;

  while (s_feederRunning.load(std::memory_order_relaxed)) {

    // ── Step 1: decode all queued encoded packets into the PCM ring ───────────
    while (true) {
      uint8_t pktData[kMaxPacketBytes];
      int     pktLen = 0;
      {
        std::unique_lock<std::mutex> lk(s_pktMutex);
        if (s_pktCount == 0) break;
        const PacketSlot& slot = s_pktQueue[s_pktHead];
        pktLen = slot.length;
        __builtin_memcpy(pktData, slot.data, (size_t)pktLen);
        s_pktHead = (s_pktHead + 1) % s_pktCap;
        --s_pktCount;
      }  // release lock before decode — Opus is CPU-intensive

      // Drop newest decoded frame if the ring is full (late-arrival burst).
      // Dropping newest preserves the oldest frames that are next in line for AL,
      // matching the original behaviour of ringPopBack() on processed==0.
      if (s_ringSize >= s_ringCap) {
        ringPopBack();
        if (++overflowCount <= 3 || overflowCount % 100 == 0)
          MoonlightInstance::ClLogMessage("AudDec: PCM ring overflow, dropping oldest frame #%llu\n",
            (unsigned long long)overflowCount);
      }

      int n = opus_multistream_decode(
        s_OpusDecoder,
        pktData, pktLen,
        decodeBuf.data(), (int)s_samplesPerFrame, 0);
      if (n > 0)
        ringPushBack(decodeBuf.data());
      else
        MoonlightInstance::ClLogMessage("AudDec: Opus decode failed rc=%d\n", n);
    }

    // ── Step 2: wait for initial jitter buffer to fill ────────────────────────
    if (!jitterReady) {
      if (s_ringSize < s_jitterFrames) {
        std::unique_lock<std::mutex> lk(s_pktMutex);
        s_pktCv.wait_for(lk, std::chrono::milliseconds(1), [] {
          return s_pktCount > 0 || !s_feederRunning.load(std::memory_order_relaxed);
        });
        continue;
      }
      int fdms = (int)s_samplesPerFrame * 1000 / (int)s_sampleRate;
      MoonlightInstance::ClLogMessage("AudDec: jitter buffer ready (%d frames / %d ms), starting AL submission\n",
        s_jitterFrames, s_jitterFrames * fdms);
      jitterReady = true;
    }

    // ── Step 3: feed processed AL buffer slots ────────────────────────────────
    ALint processed = 0;
    alGetSourcei(s_AlSource, AL_BUFFERS_PROCESSED, &processed);

    if (processed == 0) {
      // AL is still busy — sleep up to 1 ms or wake early on a new packet.
      std::unique_lock<std::mutex> lk(s_pktMutex);
      s_pktCv.wait_for(lk, std::chrono::milliseconds(1), [] {
        return s_pktCount > 0 || !s_feederRunning.load(std::memory_order_relaxed);
      });
      continue;
    }

    // Recycle all processed AL buffers.  Real frames are drawn from the PCM ring;
    // any remaining freed slots are filled with Opus PLC (genuine packet loss).
    // Unqueue + re-queue are batched to minimise WASM→JS crossings.
    ALint count    = (processed < (ALint)s_ringSize) ? processed : (ALint)s_ringSize;
    ALint plcCount = processed - count;
    ALuint bufs[128];  // processed <= s_numBuffers, always fits
    alSourceUnqueueBuffers(s_AlSource, processed, bufs);

    for (ALint i = 0; i < count; i++) {
      alBufferData(bufs[i], s_alFormat, ringFront(),
        (ALsizei)(s_frameElems * sizeof(opus_int16)), s_sampleRate);
      ringPopFront();
    }
    if (plcCount > 0) {
      plcTotal += (uint64_t)plcCount;
      MoonlightInstance::ClLogMessage("AudDec: %d lost packet(s), filling with PLC (total=%llu)\n",
        (int)plcCount, (unsigned long long)plcTotal);
      for (ALint i = count; i < processed; i++) {
        opus_multistream_decode(s_OpusDecoder,
          nullptr, 0, decodeBuf.data(), (int)s_samplesPerFrame, 0);
        alBufferData(bufs[i], s_alFormat, decodeBuf.data(),
          (ALsizei)(s_frameElems * sizeof(opus_int16)), s_sampleRate);
      }
    }
    alSourceQueueBuffers(s_AlSource, processed, bufs);

    if (s_ringSize == 0)
      MoonlightInstance::ClLogMessage("AudDec: ring drained (submitted %d real + %d PLC)\n",
        (int)count, (int)plcCount);
    if (processed >= (ALint)s_numBuffers)
      MoonlightInstance::ClLogMessage("AudDec: AL pool fully consumed (%d/%d slots), underrun risk\n",
        (int)processed, s_numBuffers);

    // Restart source if it stopped (underrun or Web Audio interruption).
    ALint state;
    alGetSourcei(s_AlSource, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) {
      MoonlightInstance::ClLogMessage("AudDec: source not playing (state=%d), restarting\n", state);
      alSourcePlay(s_AlSource);
    }
  }

  alcMakeContextCurrent(nullptr);
  MoonlightInstance::ClLogMessage("AudDec: feeder thread exiting\n");
}

// ─── AudDecInit ───────────────────────────────────────────────────────────────

int MoonlightInstance::AudDecInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  s_channelCount    = (size_t)opusConfig->channelCount;
  s_samplesPerFrame = (size_t)opusConfig->samplesPerFrame;
  s_sampleRate      = (ALsizei)opusConfig->sampleRate;

  // Compute frame count needed to cover targetJitterMs (ceiling division).
  int targetJitterMs = (g_AudioJitterMsOverride != 0) ? g_AudioJitterMsOverride : 100;
  s_jitterFrames = (targetJitterMs * (int)s_sampleRate + (int)s_samplesPerFrame * 1000 - 1)
                   / ((int)s_samplesPerFrame * 1000);
  s_numBuffers = std::max(10, s_jitterFrames);
  int burstSlack = s_jitterFrames*2;
  s_ringCap    = s_jitterFrames + burstSlack;

  int frameDurationMs = (int)s_samplesPerFrame * 1000 / (int)s_sampleRate;
  MoonlightInstance::ClLogMessage("AudDecInit: ch=%d samplesPerFrame=%d sampleRate=%d jitterFrames=%d jitterMs=%d numBuffers=%d ringCap=%d (target=%dms)\n",
    opusConfig->channelCount, opusConfig->samplesPerFrame, opusConfig->sampleRate,
    s_jitterFrames, s_jitterFrames * frameDurationMs, s_numBuffers, s_ringCap, targetJitterMs);

  // ── Open AL device and context ────────────────────────────────────────────
  s_AlDevice = alcOpenDevice(NULL);
  if (!s_AlDevice) {
    MoonlightInstance::ClLogMessage("AudDecInit: alcOpenDevice failed\n");
    return -1;
  }
  ALCint attrs[] = { ALC_FREQUENCY, (ALCint)s_sampleRate, 0 };
  s_AlContext = alcCreateContext(s_AlDevice, attrs);
  if (!s_AlContext) {
    MoonlightInstance::ClLogMessage("AudDecInit: alcCreateContext failed\n");
    alcCloseDevice(s_AlDevice);
    s_AlDevice = nullptr;
    return -1;
  }
  alcMakeContextCurrent(s_AlContext);

  ALCint actualFreq = 0;
  alcGetIntegerv(s_AlDevice, ALC_FREQUENCY, 1, &actualFreq);
  MoonlightInstance::ClLogMessage("AudDecInit: AL context frequency = %d Hz (Opus sampleRate = %d Hz)\n",
    (int)actualFreq, (int)s_sampleRate);

  // ── Select AL format ──────────────────────────────────────────────────────
  switch (s_channelCount) {
    case 2:
      s_alFormat = AL_FORMAT_STEREO16;
      break;
    case 6: {
      ALenum fmt = alGetEnumValue("AL_FORMAT_51CHN16");
      s_alFormat = (fmt != (ALenum)-1 && fmt != 0) ? fmt : AL_FORMAT_STEREO16;
      if (s_alFormat == AL_FORMAT_STEREO16) {
        MoonlightInstance::ClLogMessage("AudDecInit: 5.1 AL format unavailable, downmixing to stereo\n");
        s_channelCount = 2;
      }
      break;
    }
    case 8: {
      ALenum fmt = alGetEnumValue("AL_FORMAT_71CHN16");
      s_alFormat = (fmt != (ALenum)-1 && fmt != 0) ? fmt : AL_FORMAT_STEREO16;
      if (s_alFormat == AL_FORMAT_STEREO16) {
        MoonlightInstance::ClLogMessage("AudDecInit: 7.1 AL format unavailable, downmixing to stereo\n");
        s_channelCount = 2;
      }
      break;
    }
    default:
      MoonlightInstance::ClLogMessage("AudDecInit: unsupported channel count %zu, using stereo\n", s_channelCount);
      s_alFormat = AL_FORMAT_STEREO16;
      s_channelCount = 2;
      break;
  }

  // ── Allocate AL buffers and source ────────────────────────────────────────
  s_AlBuffers.resize(s_numBuffers);
  alGenBuffers(s_numBuffers, s_AlBuffers.data());
  alGenSources(1, &s_AlSource);
  alSourcei(s_AlSource, AL_LOOPING, AL_FALSE);

  // Pre-queue silence so playback starts immediately and covers the initial
  // jitter accumulation window while the feeder fills the PCM ring.
  {
    size_t frameBytes = s_samplesPerFrame * s_channelCount * sizeof(opus_int16);
    std::vector<opus_int16> silence(s_samplesPerFrame * s_channelCount, 0);
    for (ALuint buf : s_AlBuffers) {
      alBufferData(buf, s_alFormat, silence.data(), (ALsizei)frameBytes, s_sampleRate);
      alSourceQueueBuffers(s_AlSource, 1, &buf);
    }
  }
  alSourcePlay(s_AlSource);
  MoonlightInstance::ClLogMessage("AudDecInit: AL source playing with %d silence buffers\n", s_numBuffers);

  // ── Allocate PCM ring (feeder-private, no locking needed) ────────────────
  s_frameElems = s_samplesPerFrame * s_channelCount;
  s_ringBuffer.resize((size_t)s_ringCap * s_frameElems);
  s_ringHead = s_ringTail = s_ringSize = 0;

  // ── Allocate encoded-packet queue ────────────────────────────────────────
  s_pktCap = s_jitterFrames * 4;
  if (s_pktCap < 64) s_pktCap = 64;
  s_pktQueue.resize(s_pktCap);
  s_pktHead = s_pktTail = s_pktCount = 0;

  // ── Create Opus decoder ───────────────────────────────────────────────────
  int rc;
  s_OpusDecoder = opus_multistream_decoder_create(
    opusConfig->sampleRate, opusConfig->channelCount,
    opusConfig->streams, opusConfig->coupledStreams,
    opusConfig->mapping, &rc);
  MoonlightInstance::ClLogMessage("AudDecInit: opus_multistream_decoder_create rc=%d\n", rc);
  g_Instance->m_OpusDecoder = s_OpusDecoder;  // keep instance in sync
  if (!s_OpusDecoder) {
    MoonlightInstance::ClLogMessage("AudDecInit: opus decoder creation failed, tearing down AL\n");
    AudDecCleanup();
    return -1;
  }

  // ── Release AL context from init thread and hand it to the feeder ─────────
  alcMakeContextCurrent(nullptr);
  s_feederRunning.store(true, std::memory_order_release);
  s_feederThread = std::thread(feederLoop);
  MoonlightInstance::ClLogMessage("AudDecInit: feeder thread started\n");
  return 0;
}

// ─── AudDecCleanup ────────────────────────────────────────────────────────────

void MoonlightInstance::AudDecCleanup(void) {
  MoonlightInstance::ClLogMessage("AudDecCleanup\n");

  // Signal feeder to stop and wait for it to exit.
  if (s_feederThread.joinable()) {
    s_feederRunning.store(false, std::memory_order_release);
    s_pktCv.notify_all();
    s_feederThread.join();
  }

  // Free packet queue.
  s_pktQueue.clear();
  s_pktQueue.shrink_to_fit();
  s_pktHead = s_pktTail = s_pktCount = s_pktCap = 0;

  // Free PCM ring.
  s_ringBuffer.clear();
  s_ringBuffer.shrink_to_fit();
  s_ringHead = s_ringTail = s_ringSize = s_ringCap = 0;

  // Reclaim AL context on the cleanup thread, then tear down AL.
  if (s_AlContext) alcMakeContextCurrent(s_AlContext);

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

  if (s_OpusDecoder) {
    opus_multistream_decoder_destroy(s_OpusDecoder);
    s_OpusDecoder = nullptr;
  }
}

// ─── AudDecDecodeAndPlaySample ────────────────────────────────────────────────
//
// Called by the moonlight-common network thread on every received audio packet.
// Pushes the raw encoded packet into the lock-protected queue; the feeder thread
// decodes and submits it to AL independently of packet arrival timing.

void MoonlightInstance::AudDecDecodeAndPlaySample(char* sampleData, int sampleLength) {
  if (!s_feederRunning.load(std::memory_order_relaxed)) return;

  if (sampleLength <= 0 || sampleLength > kMaxPacketBytes) {
    MoonlightInstance::ClLogMessage("AudDec: packet length %d out of range, dropping\n", sampleLength);
    return;
  }

  {
    std::unique_lock<std::mutex> lk(s_pktMutex);
    if (s_pktCount >= s_pktCap) {
      // Queue full: drop the oldest packet to make room for the new one.
      s_pktHead = (s_pktHead + 1) % s_pktCap;
      --s_pktCount;
      MoonlightInstance::ClLogMessage("AudDec: packet queue overflow, dropping oldest\n");
    }
    PacketSlot& slot = s_pktQueue[s_pktTail];
    __builtin_memcpy(slot.data, sampleData, (size_t)sampleLength);
    slot.length = sampleLength;
    s_pktTail = (s_pktTail + 1) % s_pktCap;
    ++s_pktCount;
  }
  s_pktCv.notify_one();
}

AUDIO_RENDERER_CALLBACKS MoonlightInstance::s_ArCallbacks = {
  .init = MoonlightInstance::AudDecInit,
  .cleanup = MoonlightInstance::AudDecCleanup,
  .decodeAndPlaySample = MoonlightInstance::AudDecDecodeAndPlaySample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
