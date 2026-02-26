#include "moonlight_wasm.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <emscripten.h>

// ─── AudioInitConfig ─────────────────────────────────────────────────────────
//
// Passed by WASM heap pointer to the JS scheduler in platform/index.js.
// Set in AudDecInit (called from a moonlight-common-c pthread), published via
// MAIN_THREAD_ASYNC_EM_ASM.  s_audioInitConfig is a static global so its
// WASM address is fixed for the lifetime of the module.
//
// jsInitDone: C++ writes 1 after all fields are valid; writes 0 in cleanup.
// The JS scheduler checks this field on every tick.
struct alignas(4) AudioInitConfig {
  int sampleRate;
  int channelCount;
  int ringPtr;     // byte offset of interleaved int16 PCM ring in WASM heap
  int sizePtr;     // byte offset of std::atomic<int32_t> frame count
  int ringCap;     // ring capacity in frames
  int frameElems;  // samplesPerFrame * channelCount
  int jitterFrames;
  int targetMs;
  int jsInitDone;    // 1 = running, 0 = not initialised / cleanup in progress
  int flushRequest;  // JS sets 1 on gap recovery; feeder clears packet queue then resets to 0
};
static AudioInitConfig s_audioInitConfig;

// ─── Jitter / sizing ──────────────────────────────────────────────────────────
// g_AudioJitterMsOverride == 0  →  use default of 100 ms.
extern int g_AudioJitterMsOverride;
static int    s_jitterFrames    = 0;
static double s_frameDurationMs = 0.0;

static size_t s_samplesPerFrame = 0;
static size_t s_channelCount    = 0;
static int    s_sampleRate      = 0;

static OpusMSDecoder* s_OpusDecoder = nullptr;

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

// ─── PCM ring buffer ──────────────────────────────────────────────────────────
// Write side: C++ feeder thread  — s_ringTail, s_ringSize.fetch_add
// Read  side: JS setInterval     — _ringHead (JS-private), Module.HEAP32[sizeIdx]--
//
// s_ringSize is std::atomic<int32_t> so JS can read/write it via Module.HEAP32.
// C++ increments with memory_order_release after writing PCM so the
// data is visible to the JS reader before the size increment.
static std::vector<opus_int16>  s_ringBuffer;
static size_t                   s_frameElems = 0;
static int                      s_ringTail   = 0;
static int                      s_ringCap    = 0;
static std::atomic<int32_t>     s_ringSize{0};

// ─── Feeder thread body ────────────────────────────────────────────────────────
//
// Decodes Opus packets from the network-thread queue into the PCM ring.
// All Web Audio scheduling is handled by the JS setInterval tick; the feeder
// never calls any proxied JS function, so it cannot deadlock.
//
static void feederLoop() {
  std::vector<opus_int16> decodeBuf(s_samplesPerFrame * s_channelCount);
  uint64_t overflowCount = 0;
  auto     lastDiag      = std::chrono::steady_clock::now();

  while (s_feederRunning.load(std::memory_order_relaxed)) {

    // ── Periodic diagnostic: JS init status and ring occupancy ───────────────
    {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDiag).count() >= 5) {
        MoonlightInstance::ClLogMessage(
          "AudDec: diag jsInitDone=%d ringSize=%d ringCap=%d\n",
          s_audioInitConfig.jsInitDone,
          s_ringSize.load(std::memory_order_relaxed),
          s_ringCap);
        lastDiag = now;
      }
    }

    // ── JS gap-recovery flush request ────────────────────────────────────────
    // JS sets flushRequest=1 when it detects a wall-clock gap > targetMs.
    // Clearing the encoded-packet queue here ensures the feeder doesn't decode
    // stale Opus packets (accumulated during the interruption) into the ring
    // immediately after JS has already discarded the stale PCM frames.
    if (s_audioInitConfig.flushRequest) {
      s_audioInitConfig.flushRequest = 0;
      std::unique_lock<std::mutex> lk(s_pktMutex);
      s_pktHead = s_pktTail = s_pktCount = 0;
      MoonlightInstance::ClLogMessage("AudDec: packet queue flushed by JS gap recovery\n");
    }

    // ── Drain encoded-packet queue into PCM ring ────────────────────────────
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

      if (s_ringSize.load(std::memory_order_relaxed) >= s_ringCap) {
        if (++overflowCount <= 3 || overflowCount % 100 == 0)
          MoonlightInstance::ClLogMessage("AudDec: PCM ring overflow #%llu, dropping packet\n",
            (unsigned long long)overflowCount);
        continue;  // ring full — drop this encoded packet
      }

      int n = opus_multistream_decode(
        s_OpusDecoder, pktData, pktLen,
        decodeBuf.data(), (int)s_samplesPerFrame, 0);
      if (n > 0) {
        opus_int16* dst = s_ringBuffer.data() + (size_t)s_ringTail * s_frameElems;
        __builtin_memcpy(dst, decodeBuf.data(), s_frameElems * sizeof(opus_int16));
        s_ringTail = (s_ringTail + 1) % s_ringCap;
        // release so JS reader sees written data before size increment
        s_ringSize.fetch_add(1, std::memory_order_release);
      } else {
        MoonlightInstance::ClLogMessage("AudDec: Opus decode failed rc=%d\n", n);
      }
    }

    // ── Wait for the next encoded packet ──────────────────────────────────────
    {
      std::unique_lock<std::mutex> lk(s_pktMutex);
      s_pktCv.wait_for(lk, std::chrono::milliseconds(1), [] {
        return s_pktCount > 0 || !s_feederRunning.load(std::memory_order_relaxed);
      });
    }
  }

  MoonlightInstance::ClLogMessage("AudDec: feeder thread exiting\n");
}

// ─── AudDecInit ───────────────────────────────────────────────────────────────

int MoonlightInstance::AudDecInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  s_channelCount    = (size_t)opusConfig->channelCount;
  s_samplesPerFrame = (size_t)opusConfig->samplesPerFrame;
  s_sampleRate      = opusConfig->sampleRate;

  int targetJitterMs  = (g_AudioJitterMsOverride != 0) ? g_AudioJitterMsOverride : 100;
  s_frameDurationMs   = (double)s_samplesPerFrame * 1000.0 / s_sampleRate;
  s_jitterFrames      = (int)std::ceil((double)targetJitterMs / s_frameDurationMs);
  int ringCap         = s_jitterFrames * 4;
  if (ringCap < 32) ringCap = 32;
  s_ringCap           = ringCap;

  MoonlightInstance::ClLogMessage(
    "AudDecInit: ch=%d spf=%d rate=%d jitterFrames=%d jitterMs=%d target=%dms ringCap=%d\n",
    opusConfig->channelCount, opusConfig->samplesPerFrame, opusConfig->sampleRate,
    s_jitterFrames, (int)(s_jitterFrames * s_frameDurationMs), targetJitterMs, s_ringCap);

  // ── Allocate PCM ring ─────────────────────────────────────────────────────
  s_frameElems = s_samplesPerFrame * s_channelCount;
  s_ringBuffer.resize((size_t)s_ringCap * s_frameElems);
  s_ringTail = 0;
  s_ringSize.store(0, std::memory_order_relaxed);

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
  g_Instance->m_OpusDecoder = s_OpusDecoder;
  if (!s_OpusDecoder) {
    MoonlightInstance::ClLogMessage("AudDecInit: opus decoder creation failed\n");
    return -1;
  }

  // ── Publish config to JS scheduler via MAIN_THREAD_ASYNC_EM_ASM ──────────
  // MAIN_THREAD_ASYNC_EM_ASM runs on the main JS thread without blocking this
  // pthread — it is the same proven mechanism used by PostToJsAsync().
  // The s_audioInitConfig struct is a static global, so its WASM address is
  // stable for the entire module lifetime.
  s_audioInitConfig = { s_sampleRate, (int)s_channelCount,
    (int)(size_t)s_ringBuffer.data(), (int)(size_t)&s_ringSize,
    s_ringCap, (int)s_frameElems, s_jitterFrames, targetJitterMs,
    /*jsInitDone=*/1, /*flushRequest=*/0 };
  MoonlightInstance::ClLogMessage("AudDecInit: publishing config to JS scheduler (configPtr=%d)\n",
    (int)(size_t)&s_audioInitConfig);
  MAIN_THREAD_ASYNC_EM_ASM({
    window._mlAudioConfigPtr = $0;
  }, (int)(size_t)&s_audioInitConfig);

  // ── Start feeder thread ───────────────────────────────────────────────────
  s_feederRunning.store(true, std::memory_order_release);
  s_feederThread = std::thread(feederLoop);
  MoonlightInstance::ClLogMessage("AudDecInit: feeder thread started\n");
  return 0;
}

// ─── AudDecCleanup ────────────────────────────────────────────────────────────

void MoonlightInstance::AudDecCleanup(void) {
  MoonlightInstance::ClLogMessage("AudDecCleanup\n");

  // Signal JS scheduler to stop playing before we free the ring.
  // jsInitDone=0 is visible to JS via Module.HEAP32 immediately.
  s_audioInitConfig.jsInitDone = 0;

  if (s_feederThread.joinable()) {
    s_feederRunning.store(false, std::memory_order_release);
    s_pktCv.notify_all();
    s_feederThread.join();
  }

  // Clear the JS-side config pointer.
  MAIN_THREAD_ASYNC_EM_ASM({
    window._mlAudioConfigPtr = 0;
  });

  s_pktQueue.clear();
  s_pktQueue.shrink_to_fit();
  s_pktHead = s_pktTail = s_pktCount = s_pktCap = 0;

  s_ringBuffer.clear();
  s_ringBuffer.shrink_to_fit();
  s_ringTail = 0;
  s_ringSize.store(0, std::memory_order_relaxed);
  s_ringCap = 0;

  if (s_OpusDecoder) {
    opus_multistream_decoder_destroy(s_OpusDecoder);
    s_OpusDecoder = nullptr;
  }
}

// ─── AudDecDecodeAndPlaySample ────────────────────────────────────────────────
//
// Called by the moonlight-common network thread on every received audio packet.
// Pushes the raw encoded packet into the lock-protected queue; the feeder thread
// decodes and writes to the PCM ring independently of packet arrival timing.

void MoonlightInstance::AudDecDecodeAndPlaySample(char* sampleData, int sampleLength) {
  if (!s_feederRunning.load(std::memory_order_relaxed)) return;

  if (sampleLength <= 0 || sampleLength > kMaxPacketBytes) {
    MoonlightInstance::ClLogMessage("AudDec: packet length %d out of range, dropping\n", sampleLength);
    return;
  }

  {
    std::unique_lock<std::mutex> lk(s_pktMutex);
    if (s_pktCount >= s_pktCap) {
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
