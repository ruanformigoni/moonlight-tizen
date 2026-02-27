#include "moonlight_wasm.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <emscripten.h>

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

// ─── Decoded-frame slot pool ──────────────────────────────────────────────────
// After decoding each Opus packet the feeder writes PCM into slot[s_slotIdx %
// kNumSlots] then calls MAIN_THREAD_ASYNC_EM_ASM to push the pointer to the
// JS audio scheduler (_audReceiveFrame).  The pool must be large enough that
// the main thread processes each slot before the feeder cycles back around;
// kNumSlots * frameDurationMs (32 * 10 ms = 320 ms) is the protection window.
static constexpr int kNumSlots      = 32;
static constexpr int kMaxFrameElems = 4096;  // 480 * 8 ch = 3840, rounded up
static opus_int16    s_frameSlots[kNumSlots][kMaxFrameElems];
static int           s_slotIdx = 0;

// ─── Feeder thread ────────────────────────────────────────────────────────────
static std::thread       s_feederThread;
static std::atomic<bool> s_feederRunning{false};

static void feederLoop() {
  auto lastDiag = std::chrono::steady_clock::now();

  while (s_feederRunning.load(std::memory_order_relaxed)) {

    // ── Periodic diagnostic ───────────────────────────────────────────────────
    {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDiag).count() >= 5) {
        MoonlightInstance::ClLogMessage("AudDec: feeder alive, pktCount=%d\n", s_pktCount);
        lastDiag = now;
      }
    }

    // ── Drain encoded-packet queue → decode → push to JS ─────────────────────
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

      opus_int16* dst = s_frameSlots[s_slotIdx % kNumSlots];
      int n = opus_multistream_decode(
        s_OpusDecoder, pktData, pktLen,
        dst, (int)s_samplesPerFrame, 0);
      if (n > 0) {
        // Pass slot pointer + audio params to main-thread JS scheduler.
        // _audReceiveFrame reads HEAP16 at this address and schedules an
        // AudioBufferSourceNode before the feeder cycles back to this slot
        // (kNumSlots frames = 320 ms protection at 10 ms/frame).
        int slotPtr = (int)(size_t)dst;
        int spf     = (int)s_samplesPerFrame;
        int ch      = (int)s_channelCount;
        int rate    = s_sampleRate;
        MAIN_THREAD_ASYNC_EM_ASM({
          if (typeof _audReceiveFrame === 'function') _audReceiveFrame($0, $1, $2, $3);
        }, slotPtr, spf, ch, rate);
        s_slotIdx++;
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

  int targetJitterMs = (g_AudioJitterMsOverride != 0) ? g_AudioJitterMsOverride : 100;
  s_frameDurationMs  = (double)s_samplesPerFrame * 1000.0 / s_sampleRate;
  s_jitterFrames     = (int)std::ceil((double)targetJitterMs / s_frameDurationMs);

  MoonlightInstance::ClLogMessage(
    "AudDecInit: ch=%d spf=%d rate=%d jitterFrames=%d target=%dms\n",
    opusConfig->channelCount, opusConfig->samplesPerFrame, opusConfig->sampleRate,
    s_jitterFrames, targetJitterMs);

  // ── Allocate encoded-packet queue ────────────────────────────────────────
  s_pktCap = s_jitterFrames * 4;
  if (s_pktCap < 64) s_pktCap = 64;
  s_pktQueue.resize(s_pktCap);
  s_pktHead = s_pktTail = s_pktCount = 0;

  s_slotIdx = 0;

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

  // ── Publish targetMs to JS scheduler ─────────────────────────────────────
  MAIN_THREAD_ASYNC_EM_ASM({
    window._mlAudioTargetMs = $0;
  }, targetJitterMs);

  // ── Start feeder thread ───────────────────────────────────────────────────
  s_feederRunning.store(true, std::memory_order_release);
  s_feederThread = std::thread(feederLoop);
  MoonlightInstance::ClLogMessage("AudDecInit: feeder thread started\n");
  return 0;
}

// ─── AudDecCleanup ────────────────────────────────────────────────────────────

void MoonlightInstance::AudDecCleanup(void) {
  MoonlightInstance::ClLogMessage("AudDecCleanup\n");

  if (s_feederThread.joinable()) {
    s_feederRunning.store(false, std::memory_order_release);
    s_pktCv.notify_all();
    s_feederThread.join();
  }

  MAIN_THREAD_ASYNC_EM_ASM({
    if (typeof stopAudioScheduler === 'function') stopAudioScheduler();
  });

  s_pktQueue.clear();
  s_pktQueue.shrink_to_fit();
  s_pktHead = s_pktTail = s_pktCount = s_pktCap = 0;

  if (s_OpusDecoder) {
    opus_multistream_decoder_destroy(s_OpusDecoder);
    s_OpusDecoder = nullptr;
  }
}

// ─── AudDecDecodeAndPlaySample ────────────────────────────────────────────────
//
// Called by the moonlight-common network thread on every received audio packet.
// Pushes the raw encoded packet into the lock-protected queue; the feeder thread
// decodes and dispatches to the JS scheduler independently.

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
