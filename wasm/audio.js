// wasm/audio.js — placeholder.
//
// The Web Audio scheduler now lives entirely in wasm/platform/index.js and is
// driven by the setInterval started in startGame().  C++ notifies JS via
// MAIN_THREAD_ASYNC_EM_ASM (same mechanism as PostToJsAsync) rather than
// through the --js-library proxy system.
//
// This file is kept so the --js-library CMake flag remains valid.

mergeInto(LibraryManager.library, {});
