// platform/audio.js — event-driven Web Audio scheduler for Moonlight WASM.
//
// C++ (auddec.cpp) decodes each Opus frame in the feeder thread, then calls
// MAIN_THREAD_ASYNC_EM_ASM to invoke _audReceiveFrame() on the main thread.
// No setInterval polling — audio is scheduled on demand as frames arrive,
// so Tizen timer throttling during TV UI overlays cannot interrupt playback.
//
// startAudioScheduler() / stopAudioScheduler() are called from index.js /
// messages.js; they reset scheduling state around the stream lifetime.

var _audNextTime = 0.0;  // next AudioBufferSourceNode start time (Web Audio clock)

// Called by C++ feeder thread via MAIN_THREAD_ASYNC_EM_ASM for each decoded frame.
//   ptr      — WASM heap byte offset of interleaved int16 PCM
//   spf      — samples per frame (per channel)
//   channels — channel count
//   sampleRate — Hz
function _audReceiveFrame(ptr, spf, channels, sampleRate) {
  var ctx = window._mlAudioCtx;
  if (!ctx) return;

  if (ctx.state === 'suspended') {
    try { ctx.resume(); } catch(e) {}
    // Drop frame; _audNextTime snap happens on the next frame after resume.
    return;
  }

  var now      = ctx.currentTime;
  var targetS  = (window._mlAudioTargetMs || 100) / 1000.0;

  // Snap if behind (initial start or gap after suspension).
  if (_audNextTime < now) _audNextTime = now;

  // Discard if already targetMs of audio is queued — handles stale bursts
  // that accumulate in the MAIN_THREAD_ASYNC_EM_ASM task queue while the
  // TV UI is open and the main thread is throttled.
  if (_audNextTime > now + targetS) return;

  // Copy PCM from WASM heap into an AudioBuffer.
  var abuf = ctx.createBuffer(channels, spf, sampleRate);
  for (var c = 0; c < channels; c++) {
    var cd   = abuf.getChannelData(c);
    var base = ptr >> 1;  // byte offset → int16 index
    for (var i = 0; i < spf; i++)
      cd[i] = Module.HEAP16[base + i * channels + c] * (1.0 / 32768.0);
  }

  var src = ctx.createBufferSource();
  src.buffer = abuf;
  src.connect(ctx.destination);
  src.start(_audNextTime);
  _audNextTime += abuf.duration;
}

function startAudioScheduler() {
  _audNextTime = 0.0;
}

function stopAudioScheduler() {
  _audNextTime = 0.0;
}
