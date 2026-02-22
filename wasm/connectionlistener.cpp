#include "moonlight_wasm.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdarg>
#include <cstring>
#include <string>

#include <emscripten.h>
#include <emscripten/threading.h>

void MoonlightInstance::ClStageStarting(int stage) {
  PostToJs(std::string("ProgressMsg: Starting ") + std::string(LiGetStageName(stage)) + std::string("..."));
}

void MoonlightInstance::ClStageFailed(int stage, int errorCode) {
  PostToJs(std::string("DialogMsg: ") + std::string(LiGetStageName(stage)) + std::string(" failed (error ") + std::to_string(errorCode) + std::string(")"));
}

void MoonlightInstance::ClConnectionStarted(void) {
  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, onConnectionStarted);
}

void MoonlightInstance::ClConnectionTerminated(int errorCode) {
  // Teardown the connection
  LiStopConnection();

  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, onConnectionStopped, errorCode);
}

void MoonlightInstance::ClDisplayMessage(const char* message) {
  PostToJs(std::string("DialogMsg: ") + std::string(message));
}

void MoonlightInstance::ClDisplayTransientMessage(const char* message) {
  PostToJs(std::string("TransientMsg: ") + std::string(message));
}

void onConnectionStarted() {
  g_Instance->OnConnectionStarted(0);
}

void onConnectionStopped(int errorCode) {
  g_Instance->OnConnectionStopped(errorCode);
}

void MoonlightInstance::ClLogMessage(const char* format, ...) {
  va_list va;
  char message[1024];

  va_start(va, format);
  vsnprintf(message, sizeof(message), format, va);
  va_end(va);

  // fprintf(stderr, ...) processes message in parts, so logs from different
  // threads may interleave. Send whole message at once to minimize this.
  emscripten_log(EM_LOG_CONSOLE, "%s", message);

  // Static mutex guards against concurrent writes from audio/video threads.
  static std::mutex s_logMutex;
  static long long s_logStartMs = 0;
  static int s_logUdpSock = -1;
  static struct sockaddr_in s_logUdpAddr = {};

  std::lock_guard<std::mutex> lk(s_logMutex);

  // Compute monotonic timestamp.
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long long nowMs = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000LL;
  if (s_logStartMs == 0) s_logStartMs = nowMs;
  long long relMs = nowMs - s_logStartMs;

  // UDP sink: open socket lazily once the host IP is known, then stream every
  // log line to port 9999 on the Sunshine PC.
  // On the PC: nc -u -l -p 9999 > moonlight.log
  if (s_logUdpSock < 0 && g_Instance != nullptr && !g_Instance->m_Host.empty()) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
      struct sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(9999);
      if (inet_pton(AF_INET, g_Instance->m_Host.c_str(), &addr.sin_addr) == 1) {
        s_logUdpAddr = addr;
        s_logUdpSock = sock;
        const char* banner = "=== moonlight-tizen log stream started ===\n";
        sendto(sock, banner, strlen(banner), 0,
               (struct sockaddr*)&s_logUdpAddr, sizeof(s_logUdpAddr));
      } else {
        close(sock);
      }
    }
  }
  if (s_logUdpSock >= 0) {
    char udpMsg[1060];
    int n = snprintf(udpMsg, sizeof(udpMsg), "[%lld.%03lld] %s",
                     relMs / 1000, relMs % 1000, message);
    sendto(s_logUdpSock, udpMsg, n > 0 ? (size_t)n : 0, 0,
           (struct sockaddr*)&s_logUdpAddr, sizeof(s_logUdpAddr));
  }
}

void MoonlightInstance::ClConnectionStatusUpdate(int connectionStatus) {
  if (g_Instance->m_DisableWarningsEnabled == false) {
    switch (connectionStatus) {
      case CONN_STATUS_OKAY:
        PostToJs(std::string("NoWarningMsg: ") + std::string("Connection to PC has been improved."));
        break;
      case CONN_STATUS_POOR:
        PostToJs(std::string("WarningMsg: ") + std::string("Slow connection to PC.\nReduce your bitrate!"));
        break;
      default:
        break;
    }
  }
}

CONNECTION_LISTENER_CALLBACKS MoonlightInstance::s_ClCallbacks = {
  .stageStarting = MoonlightInstance::ClStageStarting,
  .stageFailed = MoonlightInstance::ClStageFailed,
  .connectionStarted = MoonlightInstance::ClConnectionStarted,
  .connectionTerminated = MoonlightInstance::ClConnectionTerminated,
  .logMessage = MoonlightInstance::ClLogMessage,
  .rumble = MoonlightInstance::ClControllerRumble,
  .connectionStatusUpdate = MoonlightInstance::ClConnectionStatusUpdate,
};
