// platform/audio.js — Web Audio scheduler for Moonlight WASM.
//
// C++ (auddec.cpp) publishes the WASM address of an AudioInitConfig struct to
// window._mlAudioConfigPtr via MAIN_THREAD_ASYNC_EM_ASM once AudDecInit has
// run.  The scheduler reads the PCM ring via Module.HEAP32 / Module.HEAP16.
//
// Call startAudioScheduler() from inside a user-gesture handler (startGame)
// so the AudioContext and setInterval are created on the main thread with
// autoplay permission.  Call stopAudioScheduler() when the stream ends.

var _audNextTime = 0.0;
var _audRingHead = 0;
var _audCfg      = null;   // ring params cached once per AudDecInit
var _audJitReady = false;

function startAudioScheduler() {
  // Stop any previous scheduler first.
  stopAudioScheduler();
  window._mlAudioConfigPtr = 0;

  window._mlAudioScheduler = setInterval(function() {
    try {
      var ctx = window._mlAudioCtx;
      if (!ctx) return;
      if (ctx.state === 'suspended') { try { ctx.resume(); } catch(e) {} return; }

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
        _audNextTime = 0.0;
        _audRingHead = 0;
        _audJitReady = false;
        console.log('[audio] scheduler config acquired: ch=' + _audCfg.channels +
          ' spf=' + _audCfg.samplesPerFrame + ' rate=' + _audCfg.sampleRate +
          ' jitter=' + _audCfg.jitterFrames + 'f targetMs=' + _audCfg.targetMs);
      }

      var cfg = _audCfg;

      // If C++ cleared the config (AudDecCleanup set jsInitDone=0), reset.
      if (!Module.HEAP32[cfg.initIdx + 8]) {
        _audCfg = null;
        return;
      }

      // Wait until the jitter buffer is full before starting playback.
      if (!_audJitReady) {
        if (Module.HEAP32[cfg.ringSizeIdx] < cfg.jitterFrames) return;
        _audJitReady = true;
      }

      var now = ctx.currentTime;
      if (_audNextTime < now) _audNextTime = now;

      while (true) {
        var lookaheadMs = (_audNextTime - ctx.currentTime) * 1000.0;
        if (lookaheadMs >= cfg.targetMs) break;
        if (Module.HEAP32[cfg.ringSizeIdx] <= 0) break;

        // Read one interleaved int16 frame from the PCM ring.
        var base = (cfg.ringDataPtr >> 1) + _audRingHead * cfg.frameElems;
        var abuf = ctx.createBuffer(cfg.channels, cfg.samplesPerFrame, cfg.sampleRate);
        for (var c = 0; c < cfg.channels; c++) {
          var cd = abuf.getChannelData(c);
          for (var i = 0; i < cfg.samplesPerFrame; i++)
            cd[i] = Module.HEAP16[base + i * cfg.channels + c] * (1.0 / 32768.0);
        }
        var src = ctx.createBufferSource();
        src.buffer = abuf;
        src.connect(ctx.destination);
        src.start(_audNextTime);
        _audNextTime += abuf.duration;

        _audRingHead = (_audRingHead + 1) % cfg.ringCap;
        // JS is single-threaded; C++ only increments s_ringSize.
        // Worst-case race: we see a stale increment — one extra drop, not a crash.
        Module.HEAP32[cfg.ringSizeIdx]--;
      }
    } catch(e) {}
  }, 5);
}

function stopAudioScheduler() {
  if (window._mlAudioScheduler) {
    clearInterval(window._mlAudioScheduler);
    window._mlAudioScheduler = null;
  }
  window._mlAudioConfigPtr = 0;
  _audCfg = null;
}
