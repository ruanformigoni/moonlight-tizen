// platform/audio.js — event-driven Web Audio scheduler for Moonlight WASM.
//
// C++ (auddec.cpp) decodes each Opus frame in the feeder thread, then calls
// MAIN_THREAD_ASYNC_EM_ASM to invoke _audReceiveFrame() on the main thread.
// No setInterval polling — audio is scheduled on demand as frames arrive,
// so Tizen timer throttling during TV UI overlays cannot interrupt playback.
//
// startAudioScheduler() / stopAudioScheduler() are called from index.js /
// messages.js; they reset scheduling state around the stream lifetime.

// Web Audio clock time (seconds) at which the next frame should start playing.
// Advances by one frame duration after each scheduled frame.
var _audNextTime = 0.0;

// Called by C++ feeder thread via MAIN_THREAD_ASYNC_EM_ASM for each decoded frame.
//   ptr        — WASM heap byte offset of interleaved int16 PCM samples
//   spf        — samples per frame (per channel), e.g. 240 (5ms), 480 (10ms), 960 (20ms)
//   channels   — number of audio channels (2 = stereo, 6 = 5.1, 8 = 7.1)
//   sampleRate — audio sample rate in Hz (48000)
function _audReceiveFrame(ptr, spf, channels, sampleRate) {
  var ctx = window._mlAudioCtx;  // AudioContext created in startGame() user-gesture handler
  if (!ctx) return;

  // AudioContext starts suspended until a user gesture; resume and wait for next frame.
  if (ctx.state === 'suspended') {
    try { ctx.resume(); } catch(e) {}
    return;
  }

  // ctx.currentTime: hardware audio clock position in seconds (read-only, always advancing).
  var now = ctx.currentTime;

  // Maximum lookahead in seconds — how far ahead of the hardware clock we allow scheduling.
  // _mlAudioTargetMs comes from the UI jitter buffer setting (published by C++ AudDecInit).
  // Fallback 100ms if C++ hasn't published yet. Divided by 1000 to convert ms → seconds.
  var targetS = (window._mlAudioTargetMs || 100) / 1000.0;

  // If _audNextTime fell behind the hardware clock (first frame, or gap after suspension),
  // snap it forward to now so we don't schedule audio in the past.
  if (_audNextTime < now) _audNextTime = now;

  // If we've already scheduled targetS worth of audio ahead of the hardware clock,
  // discard this frame. This prevents stale bursts from piling up when
  // MAIN_THREAD_ASYNC_EM_ASM tasks queue during TV UI throttling.
  if (_audNextTime > now + targetS) return;

  // Copy interleaved int16 PCM from the WASM heap into a Web Audio AudioBuffer.
  // createBuffer(channels, length, sampleRate) allocates a float32 buffer per channel.
  var abuf = ctx.createBuffer(channels, spf, sampleRate);
  for (var c = 0; c < channels; c++) {
    var cd   = abuf.getChannelData(c);       // Float32Array for this channel
    var base = ptr >> 1;                      // byte offset → int16 index (divide by 2)
    for (var i = 0; i < spf; i++)
      // De-interleave: HEAP16 layout is [L,R,L,R,...], stride = channels.
      // Normalize int16 (-32768..32767) to float (-1.0..1.0) for Web Audio.
      cd[i] = Module.HEAP16[base + i * channels + c] * (1.0 / 32768.0);
  }

  // Schedule this frame for playback at _audNextTime on the hardware clock.
  var src = ctx.createBufferSource();
  src.buffer = abuf;
  src.connect(ctx.destination);  // Route to default audio output
  src.start(_audNextTime);       // Play at the scheduled time
  _audNextTime += abuf.duration; // Advance schedule by frame duration (spf / sampleRate)
}

function startAudioScheduler() {
  _audNextTime = 0.0;
}

function stopAudioScheduler() {
  _audNextTime = 0.0;
}
