// platform/audio.js — Web Audio scheduler for Moonlight WASM.
//
// C++ (auddec.cpp) publishes the WASM address of an AudioInitConfig struct to
// window._mlAudioConfigPtr via MAIN_THREAD_ASYNC_EM_ASM once AudDecInit has
// run.  The scheduler reads the PCM ring via Module.HEAP32 / Module.HEAP16.
//
// Call startAudioScheduler() from inside a user-gesture handler (startGame)
// so the AudioContext and setInterval are created on the main thread with
// autoplay permission.  Call stopAudioScheduler() when the stream ends.

var _audNextTime     = 0.0;
var _audRingHead     = 0;
var _audCfg          = null;  // ring params cached once per AudDecInit
var _audJitReady     = false;
var _audLastWallMs   = 0;     // Date.now() at last scheduler tick (not during suspension)
var _audPendingNodes = [];    // {node, endTime} for every scheduled node still in the graph

// Cancel and remove all pre-scheduled nodes that haven't played yet.
function _audCancelPending() {
  for (var i = 0; i < _audPendingNodes.length; i++) {
    try { _audPendingNodes[i].node.stop(0); } catch(e) {}
  }
  _audPendingNodes = [];
  _audNextTime = 0.0;  // will be snapped to ctx.currentTime before next schedule
}

function startAudioScheduler() {
  // Stop any previous scheduler first.
  stopAudioScheduler();
  window._mlAudioConfigPtr = 0;

  window._mlAudioScheduler = setInterval(function() {
    try {
      var ctx = window._mlAudioCtx;
      if (!ctx) return;

      // During suspension _audLastWallMs is not updated (we return early here),
      // so wallGapMs will accumulate the full suspension duration automatically.
      if (ctx.state === 'suspended') {
        try { ctx.resume(); } catch(e) {}
        return;
      }

      // ── Wall-clock gap detection ──────────────────────────────────────────
      // ctx.currentTime freezes during AudioContext suspension, so it cannot
      // measure the gap.  Date.now() always advances.
      // During suspension _audLastWallMs is not updated (we returned early above),
      // so the first running tick after resume measures the full suspension duration.
      var wallNow   = Date.now();
      var wallGapMs = (_audLastWallMs > 0) ? (wallNow - _audLastWallMs) : 0;
      _audLastWallMs = wallNow;

      // Discover ring config the first time C++ publishes the config pointer.
      var configPtr = window._mlAudioConfigPtr;
      if (!_audCfg) {
        if (!configPtr || !Module || !Module.HEAP32) return;
        var idx = configPtr >> 2;
        if (!Module.HEAP32[idx + 8]) return;  // jsInitDone == 0
        _audCfg = {
          sampleRate:      Module.HEAP32[idx + 0],
          channels:        Module.HEAP32[idx + 1],
          ringDataPtr:     Module.HEAP32[idx + 2],
          ringSizeIdx:     Module.HEAP32[idx + 3] >> 2,  // int32 index into HEAP32
          ringCap:         Module.HEAP32[idx + 4],
          frameElems:      Module.HEAP32[idx + 5],
          samplesPerFrame: (Module.HEAP32[idx + 5] / Module.HEAP32[idx + 1]) | 0,
          jitterFrames:    Module.HEAP32[idx + 6],
          targetMs:        Module.HEAP32[idx + 7],
          initIdx:         idx   // kept to check jsInitDone on each tick
        };
        _audNextTime   = 0.0;
        _audRingHead   = 0;
        _audJitReady   = false;
        _audLastWallMs = 0;
        console.log('[audio] scheduler config acquired: ch=' + _audCfg.channels +
          ' spf=' + _audCfg.samplesPerFrame + ' rate=' + _audCfg.sampleRate +
          ' jitter=' + _audCfg.jitterFrames + 'f targetMs=' + _audCfg.targetMs);
      }

      var cfg = _audCfg;

      // If C++ cleared the config (AudDecCleanup set jsInitDone=0), reset.
      if (!Module.HEAP32[cfg.initIdx + 8]) {
        _audCancelPending();
        _audCfg = null;
        return;
      }

      // ── Trim nodes that have already played ───────────────────────────────
      var now = ctx.currentTime;
      while (_audPendingNodes.length > 0 && _audPendingNodes[0].endTime <= now) {
        _audPendingNodes.shift();
      }

      // ── Gap recovery ──────────────────────────────────────────────────────
      // Threshold is cfg.targetMs: if the wall-clock gap exceeds the entire
      // pre-scheduled buffer depth, the scheduler fell too far behind to recover
      // gracefully.  Cancel pre-scheduled nodes so stale audio doesn't replay,
      // discard all PCM that accumulated in the ring, and rebuild the jitter
      // buffer from scratch.  Any gap shorter than targetMs was covered by the
      // pre-scheduled lookahead — no resync needed.
      if (wallGapMs > cfg.targetMs) {
        _audCancelPending();
        var stale = Module.HEAP32[cfg.ringSizeIdx];
        if (stale > 0) {
          _audRingHead = (_audRingHead + stale) % cfg.ringCap;
          Module.HEAP32[cfg.ringSizeIdx] = 0;
        }
        // Tell the C++ feeder to discard its encoded-packet queue.  Without
        // this the feeder immediately re-fills the ring with stale Opus packets
        // that accumulated during the interruption, causing the perceived lag
        // to equal the interruption duration rather than just the jitter rebuild.
        Module.HEAP32[cfg.initIdx + 9] = 1;  // flushRequest
        _audJitReady = false;
      }

      // Wait until the jitter buffer is full before starting playback.
      if (!_audJitReady) {
        if (Module.HEAP32[cfg.ringSizeIdx] < cfg.jitterFrames) return;
        _audJitReady = true;
      }

      if (_audNextTime < now) _audNextTime = now;

      // ── Schedule one batched AudioBufferSourceNode per tick ───────────────
      // The old per-frame loop created one AudioBufferSourceNode per 10 ms
      // Opus frame.  On Tizen each node-creation API call (createBuffer,
      // createBufferSource, connect, start) is expensive enough that scheduling
      // a full 100 ms batch (10 nodes) could take > 100 ms of CPU, causing the
      // next setInterval tick to fire late, triggering a false gap recovery, and
      // putting the scheduler in an infinite flush/rebuild loop (seen as 4+
      // minutes of silence in the diagnostic log).
      //
      // Fix: compute how many frames are needed to reach targetMs of lookahead,
      // fill them all into a single AudioBuffer, and create exactly one
      // AudioBufferSourceNode per tick.  PCM fill work is identical; node-
      // creation overhead drops from N calls to 1.
      var lookaheadMs = (_audNextTime - ctx.currentTime) * 1000.0;
      if (lookaheadMs < cfg.targetMs) {
        var frameDurMs = cfg.samplesPerFrame * 1000.0 / cfg.sampleRate;
        var frameCount = Math.ceil((cfg.targetMs - lookaheadMs) / frameDurMs);
        var avail      = Module.HEAP32[cfg.ringSizeIdx];
        frameCount     = Math.min(frameCount, avail);

        if (frameCount <= 0) {
          // Ring drained — reset anchor so refilled frames play without drift.
          if (_audPendingNodes.length === 0) _audNextTime = 0.0;
        } else {
          var abuf = ctx.createBuffer(cfg.channels, frameCount * cfg.samplesPerFrame,
                                      cfg.sampleRate);
          for (var c = 0; c < cfg.channels; c++) {
            var cd = abuf.getChannelData(c);
            for (var f = 0; f < frameCount; f++) {
              var head   = (_audRingHead + f) % cfg.ringCap;
              var base   = (cfg.ringDataPtr >> 1) + head * cfg.frameElems;
              var dstOff = f * cfg.samplesPerFrame;
              for (var i = 0; i < cfg.samplesPerFrame; i++)
                cd[dstOff + i] = Module.HEAP16[base + i * cfg.channels + c] * (1.0 / 32768.0);
            }
          }
          var src = ctx.createBufferSource();
          src.buffer = abuf;
          src.connect(ctx.destination);
          src.start(_audNextTime);
          _audNextTime += abuf.duration;
          _audPendingNodes.push({node: src, endTime: _audNextTime});

          _audRingHead = (_audRingHead + frameCount) % cfg.ringCap;
          // JS is single-threaded; C++ only increments s_ringSize.
          // Worst-case race: stale increment seen — one extra drop, not a crash.
          Module.HEAP32[cfg.ringSizeIdx] -= frameCount;
        }
      }
    } catch(e) {}
  }, 5);
}

function stopAudioScheduler() {
  if (window._mlAudioScheduler) {
    clearInterval(window._mlAudioScheduler);
    window._mlAudioScheduler = null;
  }
  _audCancelPending();
  window._mlAudioConfigPtr = 0;
  _audCfg                  = null;
  _audLastWallMs           = 0;
}
