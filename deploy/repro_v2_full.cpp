/**
 * repro_v2_full.cpp — Agora Server Gateway repro: v2 C API + dlopen/dlsym only.
 *
 * Parallel implementation of repro_pthread_init.cpp using the v2 C API surface
 * (agora_service_*, agora_rtc_conn_*, agora_local_user_*, etc.).
 * No direct link dependency on libagora_rtc_sdk — all symbols loaded via dlsym at runtime.
 *
 * All env vars, logs, and behavior identical to repro_pthread_init.cpp:
 *   AGORA_APP_ID, AGORA_CHANNEL_ID, AGORA_TOKEN (optional), AGORA_UID (optional)
 *   AGORA_USE_STRING_UID=1  - string user account mode; AGORA_UID is the account string (enable in Agora Console if required)
 *   AGORA_REGISTER_AUDIO_OBSERVER=0|1  - register playback audio frame observer (default 1)
 *   AGORA_ENABLE_AUDIO_VOLUME_INDICATION=0|1  - enable SDK audio volume indication callback (default 1)
 *   AGORA_SET_CHANNEL_PROFILE=0|1  - whether to set channel_profile on connection config (default 0)
 *   AGORA_CHANNEL_PROFILE=COMMUNICATION|LIVE_BROADCASTING (or 0|1)
 *   AGORA_SET_CLIENT_ROLE_TYPE=0|1 - whether to set client_role_type on connection config (default 1)
 *   AGORA_CLIENT_ROLE_TYPE=AUDIENCE|BROADCASTER (or 2|1)
 *   AGORA_REGISTER_CONN_OBSERVER=0|1 - register rtc_conn observer callbacks (default 0 for stability)
 *   AGORA_REGISTER_LOCAL_USER_OBSERVER=0|1 - register local_user observer callbacks (default 1; match C++ repro)
 *   AGORA_LU_CB_AUDIO_SUB=0|1 - local_user_observer.on_user_audio_track_subscribed (default 1)
 *   AGORA_LU_CB_VIDEO_SUB=0|1 - local_user_observer.on_user_video_track_subscribed (default 1)
 *   AGORA_LU_CB_VOLUME_IND=0|1 - on_audio_volume_indication (default follows AGORA_ENABLE_AUDIO_VOLUME_INDICATION; set 0 to disable callback only)
 *   AGORA_RECEIVE_VIDEO=1  - subscribe to and process remote video
 *   AGORA_SEND_AUDIO=1    - publish local audio (440 Hz PCM tone, 16 kHz mono)
 *   AGORA_SEND_VIDEO=1    - publish local video (720p I420 badge pattern)
 *   AGORA_JOIN_DURATION_SEC=N - stay N seconds; 0 = until Ctrl+C (default 60)
 *   AGORA_REPRO_STOP_AFTER=init|create_local_audio_track|connect|publish
 *   AGORA_THREAD_PRIORITY=0-5 - logged only; v2 agora_service_config has no threadPriority field
 *   AGORA_ENCRYPTION_ENABLE=0|1
 *   AGORA_ENCRYPTION_MODE  - 1-8 or name: AES-128-XTS(1), AES-128-ECB(2), AES-256-XTS(3),
 *                            SM4-128-ECB(4), AES-128-GCM(5), AES-256-GCM(6),
 *                            AES-128-GCM2(7), AES-256-GCM2(8)
 *   AGORA_ENCRYPTION_SECRET  - encryption key
 *   AGORA_ENCRYPTION_SALT    - Base64-encoded 32-byte salt (required for GCM2 modes)
 *   AGORA_LOG_FILE           - SDK log file path (default /app/agora_sdk.log)
 *   AGORA_AREA_CODE=GLOB|OVS|0xFFFFFFFF — service area (default GLOB)
 *   AGORA_SET_SERVICE_CHANNEL_PROFILE=0|1 — set channel_profile on agora_service_config (default 0)
 *   AGORA_SERVICE_CHANNEL_PROFILE — COMMUNICATION|LIVE_BROADCASTING (or 0|1)
 *   AGORA_SET_SERVICE_AUDIO_SCENARIO=0|1 — set audio_scenario on agora_service_config (default 0)
 *   AGORA_SERVICE_AUDIO_SCENARIO — DEFAULT|GAME_STREAMING|CHATROOM|… or numeric 0–10
 *   AGORA_VOLUME_INDICATION_INTERVAL_MS, AGORA_VOLUME_INDICATION_SMOOTH, AGORA_VOLUME_INDICATION_VAD (0|1)
 *   AGORA_SET_LOCAL_USER_AUDIO_SCENARIO=0|1 — call agora_local_user_set_audio_scenario after get_local_user (default 0)
 *   AGORA_LOCAL_USER_AUDIO_SCENARIO — same names/numbers as service audio scenario
 *   AGORA_LU_CB_USER_INFO_UPDATED=0|1 — wire on_user_info_updated (default 0)
 *   AGORA_DUMP_BEFORE_MIXING_PCM=0|1 — per-remote-uid raw PCM from before-mixing callback (default 0)
 *   AGORA_DUMP_PLAYBACK_PCM=0|1 — mixed playback PCM to playback_mixed.pcm (default 0; easier sanity check than per-uid)
 *   AGORA_DUMP_PCM_DIR — output directory for *.pcm (default /tmp/agora_pcm_dump)
 */

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <map>
#include <mutex>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dlfcn.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* v2 C API headers — included for struct/typedef definitions only.
   All functions are loaded at runtime via dlsym; none are called directly. */
#include "c/api2/agora_service.h"
#include "c/api2/agora_rtc_conn.h"
#include "c/api2/agora_local_user.h"
#include "c/api2/agora_audio_track.h"
#include "c/api2/agora_video_track.h"
#include "c/api2/agora_media_node_factory.h"
#include "c/base/agora_media_base.h"

/* ============================================================
 * Signal handling
 * ============================================================ */

static volatile bool g_exit = false;

static void sig_handler(int sig) {
  const char* name = (sig == SIGABRT) ? "SIGABRT" : (sig == SIGSEGV) ? "SIGSEGV" :
                     (sig == SIGINT)  ? "SIGINT"  : nullptr;
  if (name) fprintf(stderr, "Signal %d (%s) caught.\n", sig, name);
  else      fprintf(stderr, "Signal %d caught.\n", sig);
  if (sig == SIGABRT || sig == SIGSEGV) _exit(128 + sig);
  g_exit = true;
}

/* ============================================================
 * Environment helpers (identical to repro_pthread_init.cpp)
 * ============================================================ */

static const char* getenv_or(const char* name, const char* def) {
  const char* v = getenv(name);
  return (v && v[0]) ? v : def;
}

static std::string getenv_trimmed_or(const char* name, const char* def) {
  const char* v = getenv(name);
  if (!v || !v[0]) return def ? std::string(def) : std::string();
  std::string s(v);
  size_t len = s.size();
  while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) --len;
  size_t hash = s.find('#');
  if (hash != std::string::npos) {
    if (hash < len) len = hash;
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) --len;
  }
  s.resize(len);
  return s;
}

static bool getenv_bool(const char* name) {
  const char* v = getenv(name);
  if (!v || !v[0]) return false;
  return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

static int getenv_join_duration_sec(int defaultVal) {
  const char* v = getenv("AGORA_JOIN_DURATION_SEC");
  if (!v || !v[0]) return defaultVal;
  char* end = nullptr;
  long n = strtol(v, &end, 10);
  if (end == v || n < 0) return defaultVal;
  if (n > 86400) n = 86400;
  return (int)n;
}

/* CHANNEL_PROFILE_TYPE commonly: COMMUNICATION=0, LIVE_BROADCASTING=1 */
static int parse_channel_profile(const char* profile) {
  if (!profile || !profile[0]) return 0;
  char* end = nullptr;
  long n = strtol(profile, &end, 10);
  if (end != profile && (n == 0 || n == 1)) return (int)n;
  std::string p(profile);
  for (auto& c : p) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (p == "LIVE_BROADCASTING" || p == "LIVE") return 1;
  return 0; /* COMMUNICATION */
}

/* CLIENT_ROLE_TYPE in C API: BROADCASTER=1, AUDIENCE=2 */
static int parse_client_role_type(const char* role) {
  if (!role || !role[0]) return 2;
  char* end = nullptr;
  long n = strtol(role, &end, 10);
  if (end != role && (n == 1 || n == 2)) return (int)n;
  std::string r(role);
  for (auto& c : r) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (r == "BROADCASTER") return 1;
  return 2; /* AUDIENCE */
}

/* AUDIO_SCENARIO_TYPE: DEFAULT=0, GAME_STREAMING=3, CHATROOM=5, CHORUS=7, MEETING=8, … */
static int parse_audio_scenario(const char* s) {
  if (!s || !s[0]) return 0;
  char* end = nullptr;
  long n = strtol(s, &end, 10);
  if (end != s && n >= 0 && n <= 10) {
    while (*end == ' ' || *end == '\t') ++end;
    if (*end == '\0' || *end == '#') return (int)n;
  }
  std::string m(s);
  size_t len = m.size();
  while (len > 0 && (m[len-1] == ' ' || m[len-1] == '\t')) --len;
  size_t hash = m.find('#');
  if (hash != std::string::npos) { if (hash < len) len = hash; while (len > 0 && (m[len-1] == ' ' || m[len-1] == '\t')) --len; }
  m.resize(len);
  for (auto& c : m) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (m == "DEFAULT" || m == "0") return 0;
  if (m == "GAME_STREAMING" || m == "GAME") return 3;
  if (m == "CHATROOM" || m == "CHAT") return 5;
  if (m == "CHORUS") return 7;
  if (m == "MEETING") return 8;
  if (m == "AI_SERVER") return 9;
  if (m == "AI_CLIENT") return 10;
  return 0;
}

static unsigned int parse_area_code(const char* s) {
  if (!s || !s[0]) return 0xFFFFFFFFu;
  std::string m(s);
  size_t len = m.size();
  while (len > 0 && (m[len-1] == ' ' || m[len-1] == '\t')) --len;
  size_t hash = m.find('#');
  if (hash != std::string::npos) { if (hash < len) len = hash; while (len > 0 && (m[len-1] == ' ' || m[len-1] == '\t')) --len; }
  m.resize(len);
  for (auto& c : m) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (m == "GLOB" || m == "GLOBAL" || m == "AREA_CODE_GLOB") return 0xFFFFFFFFu;
  if (m == "OVS" || m == "AREA_CODE_OVS") return 0xFFFFFFFEu;
  char* end = nullptr;
  unsigned long v = strtoul(m.c_str(), &end, 0);
  if (end != m.c_str() && v <= 0xFFFFFFFFu) return (unsigned int)v;
  return 0xFFFFFFFFu;
}

static int getenv_int_or(const char* name, int def) {
  const char* v = getenv(name);
  if (!v || !v[0]) return def;
  char* end = nullptr;
  long n = strtol(v, &end, 10);
  if (end == v) return def;
  return (int)n;
}
static bool getenv_bool_default(const char* name, bool def) {
  const char* v = getenv(name);
  if (!v || !v[0]) return def;
  return getenv_bool(name);
}

/* ============================================================
 * Base64 decode (identical to repro_pthread_init.cpp)
 * ============================================================ */

static int base64_decode(const char* in, size_t inLen, unsigned char* out, int maxOut) {
  static const unsigned char T[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,255,255,255,
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
  };
  int n = 0;
  unsigned val = 0;
  int bits = 0;
  for (size_t i = 0; i < inLen && n < maxOut; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c >= sizeof(T) || T[c] == 255) continue;
    val = (val << 6) | T[c];
    bits += 6;
    if (bits >= 8) { bits -= 8; out[n++] = (unsigned char)(val >> bits); }
  }
  return n;
}

/* ============================================================
 * Encryption mode parsing (numeric values, no C++ enum dep)
 * Modes: AES_128_XTS=1, AES_128_ECB=2, AES_256_XTS=3, SM4_128_ECB=4,
 *        AES_128_GCM=5, AES_256_GCM=6, AES_128_GCM2=7, AES_256_GCM2=8
 * ============================================================ */

static int parse_encryption_mode(const char* mode) {
  if (!mode || !mode[0]) return -1;
  char* end = nullptr;
  long n = strtol(mode, &end, 10);
  if (end != mode && n >= 1 && n <= 8) {
    while (*end == ' ' || *end == '\t') ++end;
    if (*end == '\0' || *end == '#') return (int)n;
  }
  std::string m(mode);
  size_t len = m.size();
  while (len > 0 && (m[len-1] == ' ' || m[len-1] == '\t')) --len;
  size_t hash = m.find('#');
  if (hash != std::string::npos) {
    if (hash < len) len = hash;
    while (len > 0 && (m[len-1] == ' ' || m[len-1] == '\t')) --len;
  }
  m.resize(len);
  if (m.empty()) return -1;
  for (auto& ch : m) { if (ch == '-') ch = '_'; else if (ch >= 'a' && ch <= 'z') ch -= 32; }
  if (m == "AES_128_XTS") return 1;
  if (m == "AES_128_ECB") return 2;
  if (m == "AES_256_XTS") return 3;
  if (m == "SM4_128_ECB") return 4;
  if (m == "AES_128_GCM") return 5;
  if (m == "AES_256_GCM") return 6;
  if (m == "AES_128_GCM2") return 7;
  if (m == "AES_256_GCM2") return 8;
  return -1;
}

static bool encryption_mode_needs_salt(int mode) {
  return mode == 7 || mode == 8;  /* AES_128_GCM2 or AES_256_GCM2 */
}

/* ============================================================
 * SDK function pointer types and globals (all loaded via dlsym)
 * ============================================================ */

typedef void* (*pfn_agora_service_create)();
typedef int   (*pfn_agora_service_initialize)(void*, const agora_service_config*);
typedef void  (*pfn_agora_service_at_exit)(void*);
typedef int   (*pfn_agora_service_release)(void*);
typedef int   (*pfn_agora_service_set_log_file)(void*, const char*, unsigned int);
typedef int   (*pfn_agora_service_set_log_filter)(void*, unsigned int);
typedef void* (*pfn_agora_service_create_local_audio_track)(void*);
typedef void* (*pfn_agora_service_create_custom_audio_track_pcm)(void*, void*);
typedef void* (*pfn_agora_service_create_custom_video_track_frame)(void*, void*);
typedef void* (*pfn_agora_service_create_media_node_factory)(void*);
typedef void  (*pfn_agora_media_node_factory_destroy)(void*);
typedef void* (*pfn_agora_media_node_factory_create_audio_pcm_data_sender)(void*);
typedef void  (*pfn_agora_audio_pcm_data_sender_destroy)(void*);
typedef void* (*pfn_agora_media_node_factory_create_video_frame_sender)(void*);
typedef void  (*pfn_agora_video_frame_sender_destroy)(void*);
typedef void  (*pfn_agora_local_audio_track_destroy)(void*);
typedef void  (*pfn_agora_local_audio_track_set_enabled)(void*, int);
typedef void  (*pfn_agora_local_video_track_destroy)(void*);
typedef void  (*pfn_agora_local_video_track_set_enabled)(void*, int);
typedef int   (*pfn_agora_local_video_track_set_video_encoder_config)(void*, const video_encoder_config*);
typedef void* (*pfn_agora_rtc_conn_create)(void*, const rtc_conn_config*);
typedef void  (*pfn_agora_rtc_conn_destroy)(void*);
typedef int   (*pfn_agora_rtc_conn_connect)(void*, const char*, const char*, const char*);
typedef int   (*pfn_agora_rtc_conn_disconnect)(void*);
typedef int   (*pfn_agora_rtc_conn_enable_encryption)(void*, int, const encryption_config*);
typedef int   (*pfn_agora_rtc_conn_register_observer)(void*, rtc_conn_observer*);
typedef int   (*pfn_agora_rtc_conn_unregister_observer)(void*);
typedef void* (*pfn_agora_rtc_conn_get_local_user)(void*);
typedef int   (*pfn_agora_local_user_subscribe_all_audio)(void*);
typedef int   (*pfn_agora_local_user_subscribe_all_video)(void*, const video_subscription_options*);
typedef int   (*pfn_agora_local_user_set_playback_audio_frame_parameters)(void*, unsigned int, unsigned int, int, int);
typedef int   (*pfn_agora_local_user_set_playback_audio_frame_before_mixing_parameters)(void*, uint32_t, uint32_t);
typedef int   (*pfn_agora_local_user_set_audio_scenario)(void*, int);
typedef int   (*pfn_agora_local_user_set_audio_volume_indication_parameters)(void*, int, int, bool);
typedef int   (*pfn_agora_local_user_register_audio_frame_observer)(void*, audio_frame_observer*);
typedef int   (*pfn_agora_local_user_unregister_audio_frame_observer)(void*);
typedef int   (*pfn_agora_local_user_register_observer)(void*, local_user_observer*);
typedef int   (*pfn_agora_local_user_unregister_observer)(void*);
typedef int   (*pfn_agora_local_user_register_video_frame_observer)(void*, void*);
typedef int   (*pfn_agora_local_user_unregister_video_frame_observer)(void*, void*);
typedef int   (*pfn_agora_local_user_publish_audio)(void*, void*);
typedef int   (*pfn_agora_local_user_unpublish_audio)(void*, void*);
typedef int   (*pfn_agora_local_user_publish_video)(void*, void*);
typedef int   (*pfn_agora_local_user_unpublish_video)(void*, void*);
typedef void* (*pfn_agora_video_frame_observer2_create)(video_frame_observer2*);
typedef void  (*pfn_agora_video_frame_observer2_destroy)(void*);
typedef int   (*pfn_agora_audio_pcm_data_sender_send)(void*, const void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef int   (*pfn_agora_video_frame_sender_send)(void*, const external_video_frame*);

static pfn_agora_service_create                                 g_svc_create = nullptr;
static pfn_agora_service_initialize                             g_svc_init = nullptr;
static pfn_agora_service_at_exit                                g_svc_at_exit = nullptr;  /* optional */
static pfn_agora_service_release                                g_svc_release = nullptr;
static pfn_agora_service_set_log_file                           g_svc_set_log_file = nullptr;
static pfn_agora_service_set_log_filter                         g_svc_set_log_filter = nullptr;
static pfn_agora_service_create_local_audio_track               g_svc_create_local_audio_track = nullptr;
static pfn_agora_service_create_custom_audio_track_pcm          g_svc_create_custom_audio_pcm = nullptr;
static pfn_agora_service_create_custom_video_track_frame        g_svc_create_custom_video_frame = nullptr;
static pfn_agora_service_create_media_node_factory              g_svc_create_factory = nullptr;
static pfn_agora_media_node_factory_destroy                     g_factory_destroy = nullptr;
static pfn_agora_media_node_factory_create_audio_pcm_data_sender g_factory_create_pcm_sender = nullptr;
static pfn_agora_audio_pcm_data_sender_destroy                  g_pcm_sender_destroy = nullptr;
static pfn_agora_media_node_factory_create_video_frame_sender   g_factory_create_video_sender = nullptr;
static pfn_agora_video_frame_sender_destroy                     g_video_sender_destroy = nullptr;
static pfn_agora_local_audio_track_destroy                      g_audio_track_destroy = nullptr;
static pfn_agora_local_audio_track_set_enabled                  g_audio_track_set_enabled = nullptr;
static pfn_agora_local_video_track_destroy                      g_video_track_destroy = nullptr;
static pfn_agora_local_video_track_set_enabled                  g_video_track_set_enabled = nullptr;
static pfn_agora_local_video_track_set_video_encoder_config     g_video_track_set_enc_cfg = nullptr;
static pfn_agora_rtc_conn_create                                g_conn_create = nullptr;
static pfn_agora_rtc_conn_destroy                               g_conn_destroy = nullptr;
static pfn_agora_rtc_conn_connect                               g_conn_connect = nullptr;
static pfn_agora_rtc_conn_disconnect                            g_conn_disconnect = nullptr;
static pfn_agora_rtc_conn_enable_encryption                     g_conn_enable_encryption = nullptr;
static pfn_agora_rtc_conn_register_observer                     g_conn_register_obs = nullptr;
static pfn_agora_rtc_conn_unregister_observer                   g_conn_unregister_obs = nullptr;
static pfn_agora_rtc_conn_get_local_user                        g_conn_get_local_user = nullptr;
static pfn_agora_local_user_subscribe_all_audio                 g_luser_sub_all_audio = nullptr;
static pfn_agora_local_user_subscribe_all_video                 g_luser_sub_all_video = nullptr;
static pfn_agora_local_user_set_playback_audio_frame_parameters g_luser_set_playback_params = nullptr;
static pfn_agora_local_user_set_playback_audio_frame_before_mixing_parameters g_luser_set_before_mixing_params = nullptr;
static pfn_agora_local_user_set_audio_scenario                  g_luser_set_audio_scenario = nullptr;
static pfn_agora_local_user_set_audio_volume_indication_parameters g_luser_set_volume_indication = nullptr;
static pfn_agora_local_user_register_audio_frame_observer       g_luser_reg_audio_obs = nullptr;
static pfn_agora_local_user_unregister_audio_frame_observer     g_luser_unreg_audio_obs = nullptr;
static pfn_agora_local_user_register_observer                   g_luser_reg_obs = nullptr;
static pfn_agora_local_user_unregister_observer                 g_luser_unreg_obs = nullptr;
static pfn_agora_local_user_register_video_frame_observer       g_luser_reg_video_obs = nullptr;
static pfn_agora_local_user_unregister_video_frame_observer     g_luser_unreg_video_obs = nullptr;
static pfn_agora_local_user_publish_audio                       g_luser_publish_audio = nullptr;
static pfn_agora_local_user_unpublish_audio                     g_luser_unpublish_audio = nullptr;
static pfn_agora_local_user_publish_video                       g_luser_publish_video = nullptr;
static pfn_agora_local_user_unpublish_video                     g_luser_unpublish_video = nullptr;
static pfn_agora_video_frame_observer2_create                   g_vobs2_create = nullptr;
static pfn_agora_video_frame_observer2_destroy                  g_vobs2_destroy = nullptr;
static pfn_agora_audio_pcm_data_sender_send                     g_pcm_send = nullptr;
static pfn_agora_video_frame_sender_send                        g_video_send = nullptr;

#define LOAD_SYM(lib, sym, type, var) \
  do { \
    (var) = (type)dlsym((lib), (sym)); \
    if (!(var)) { \
      fprintf(stderr, "[v2] dlsym(%s) failed: %s\n", (sym), dlerror()); \
      return 1; \
    } \
  } while (0)


static int load_symbols(void* lib) {
  LOAD_SYM(lib, "agora_service_create",             pfn_agora_service_create,             g_svc_create);
  LOAD_SYM(lib, "agora_service_initialize",         pfn_agora_service_initialize,         g_svc_init);
  /* agora_service_at_exit is optional — not present in all SDK builds */
  g_svc_at_exit = (pfn_agora_service_at_exit)dlsym(lib, "agora_service_at_exit");
  LOAD_SYM(lib, "agora_service_release",            pfn_agora_service_release,            g_svc_release);
  LOAD_SYM(lib, "agora_service_set_log_file",       pfn_agora_service_set_log_file,       g_svc_set_log_file);
  LOAD_SYM(lib, "agora_service_set_log_filter",     pfn_agora_service_set_log_filter,     g_svc_set_log_filter);
  LOAD_SYM(lib, "agora_service_create_local_audio_track",          pfn_agora_service_create_local_audio_track,      g_svc_create_local_audio_track);
  LOAD_SYM(lib, "agora_service_create_custom_audio_track_pcm",     pfn_agora_service_create_custom_audio_track_pcm, g_svc_create_custom_audio_pcm);
  LOAD_SYM(lib, "agora_service_create_custom_video_track_frame",   pfn_agora_service_create_custom_video_track_frame, g_svc_create_custom_video_frame);
  LOAD_SYM(lib, "agora_service_create_media_node_factory",         pfn_agora_service_create_media_node_factory,     g_svc_create_factory);
  LOAD_SYM(lib, "agora_media_node_factory_destroy",                pfn_agora_media_node_factory_destroy,            g_factory_destroy);
  LOAD_SYM(lib, "agora_media_node_factory_create_audio_pcm_data_sender", pfn_agora_media_node_factory_create_audio_pcm_data_sender, g_factory_create_pcm_sender);
  LOAD_SYM(lib, "agora_audio_pcm_data_sender_destroy",             pfn_agora_audio_pcm_data_sender_destroy,         g_pcm_sender_destroy);
  LOAD_SYM(lib, "agora_media_node_factory_create_video_frame_sender", pfn_agora_media_node_factory_create_video_frame_sender, g_factory_create_video_sender);
  LOAD_SYM(lib, "agora_video_frame_sender_destroy",                pfn_agora_video_frame_sender_destroy,            g_video_sender_destroy);
  LOAD_SYM(lib, "agora_local_audio_track_destroy",                 pfn_agora_local_audio_track_destroy,             g_audio_track_destroy);
  LOAD_SYM(lib, "agora_local_audio_track_set_enabled",             pfn_agora_local_audio_track_set_enabled,         g_audio_track_set_enabled);
  LOAD_SYM(lib, "agora_local_video_track_destroy",                 pfn_agora_local_video_track_destroy,             g_video_track_destroy);
  LOAD_SYM(lib, "agora_local_video_track_set_enabled",             pfn_agora_local_video_track_set_enabled,         g_video_track_set_enabled);
  LOAD_SYM(lib, "agora_local_video_track_set_video_encoder_config", pfn_agora_local_video_track_set_video_encoder_config, g_video_track_set_enc_cfg);
  LOAD_SYM(lib, "agora_rtc_conn_create",            pfn_agora_rtc_conn_create,            g_conn_create);
  LOAD_SYM(lib, "agora_rtc_conn_destroy",           pfn_agora_rtc_conn_destroy,           g_conn_destroy);
  LOAD_SYM(lib, "agora_rtc_conn_connect",           pfn_agora_rtc_conn_connect,           g_conn_connect);
  LOAD_SYM(lib, "agora_rtc_conn_disconnect",        pfn_agora_rtc_conn_disconnect,        g_conn_disconnect);
  LOAD_SYM(lib, "agora_rtc_conn_enable_encryption", pfn_agora_rtc_conn_enable_encryption, g_conn_enable_encryption);
  LOAD_SYM(lib, "agora_rtc_conn_register_observer", pfn_agora_rtc_conn_register_observer, g_conn_register_obs);
  LOAD_SYM(lib, "agora_rtc_conn_unregister_observer", pfn_agora_rtc_conn_unregister_observer, g_conn_unregister_obs);
  LOAD_SYM(lib, "agora_rtc_conn_get_local_user",    pfn_agora_rtc_conn_get_local_user,    g_conn_get_local_user);
  LOAD_SYM(lib, "agora_local_user_subscribe_all_audio",            pfn_agora_local_user_subscribe_all_audio,            g_luser_sub_all_audio);
  LOAD_SYM(lib, "agora_local_user_subscribe_all_video",            pfn_agora_local_user_subscribe_all_video,            g_luser_sub_all_video);
  LOAD_SYM(lib, "agora_local_user_set_playback_audio_frame_parameters", pfn_agora_local_user_set_playback_audio_frame_parameters, g_luser_set_playback_params);
  LOAD_SYM(lib, "agora_local_user_set_playback_audio_frame_before_mixing_parameters",
           pfn_agora_local_user_set_playback_audio_frame_before_mixing_parameters, g_luser_set_before_mixing_params);
  LOAD_SYM(lib, "agora_local_user_set_audio_scenario", pfn_agora_local_user_set_audio_scenario, g_luser_set_audio_scenario);
  LOAD_SYM(lib, "agora_local_user_set_audio_volume_indication_parameters", pfn_agora_local_user_set_audio_volume_indication_parameters, g_luser_set_volume_indication);
  LOAD_SYM(lib, "agora_local_user_register_audio_frame_observer",  pfn_agora_local_user_register_audio_frame_observer,  g_luser_reg_audio_obs);
  LOAD_SYM(lib, "agora_local_user_unregister_audio_frame_observer", pfn_agora_local_user_unregister_audio_frame_observer, g_luser_unreg_audio_obs);
  LOAD_SYM(lib, "agora_local_user_register_observer",              pfn_agora_local_user_register_observer,              g_luser_reg_obs);
  LOAD_SYM(lib, "agora_local_user_unregister_observer",            pfn_agora_local_user_unregister_observer,            g_luser_unreg_obs);
  LOAD_SYM(lib, "agora_local_user_register_video_frame_observer",  pfn_agora_local_user_register_video_frame_observer,  g_luser_reg_video_obs);
  LOAD_SYM(lib, "agora_local_user_unregister_video_frame_observer", pfn_agora_local_user_unregister_video_frame_observer, g_luser_unreg_video_obs);
  LOAD_SYM(lib, "agora_local_user_publish_audio",   pfn_agora_local_user_publish_audio,   g_luser_publish_audio);
  LOAD_SYM(lib, "agora_local_user_unpublish_audio", pfn_agora_local_user_unpublish_audio, g_luser_unpublish_audio);
  LOAD_SYM(lib, "agora_local_user_publish_video",   pfn_agora_local_user_publish_video,   g_luser_publish_video);
  LOAD_SYM(lib, "agora_local_user_unpublish_video", pfn_agora_local_user_unpublish_video, g_luser_unpublish_video);
  LOAD_SYM(lib, "agora_video_frame_observer2_create",  pfn_agora_video_frame_observer2_create,  g_vobs2_create);
  LOAD_SYM(lib, "agora_video_frame_observer2_destroy", pfn_agora_video_frame_observer2_destroy, g_vobs2_destroy);
  LOAD_SYM(lib, "agora_audio_pcm_data_sender_send", pfn_agora_audio_pcm_data_sender_send, g_pcm_send);
  LOAD_SYM(lib, "agora_video_frame_sender_send",    pfn_agora_video_frame_sender_send,    g_video_send);
  return 0;
}

/* ============================================================
 * Global observer state (C callbacks access via globals)
 * ============================================================ */

static std::atomic<uint64_t> g_audio_frames_received{0};
static std::atomic<uint64_t> g_video_frames_received{0};
static std::atomic<uint64_t> g_audio_volume_cb_count{0};

static bool              g_enable_audio_observer = true;
static bool              g_conn_obs_registered = false;
static bool              g_luser_obs_registered = false;
static bool              g_lu_cb_audio_sub = true;
static bool              g_lu_cb_video_sub = true;
static bool              g_lu_cb_volume_ind = false;
static bool              g_lu_cb_user_info = false;
static audio_frame_observer  g_audio_obs = {};
static video_frame_observer2 g_vobs2_impl = {};
static void*             g_vobs2_handle = nullptr;
static bool              g_audio_obs_registered = false;
static bool              g_video_obs_registered = false;

/* Optional: raw PCM dumps */
static bool g_dump_before_mixing_pcm = false;
static bool g_dump_playback_pcm = false;
static std::string g_dump_pcm_dir;
static std::mutex g_dump_pcm_mutex;
static std::map<std::string, FILE*> g_dump_pcm_files;
static FILE* g_dump_playback_fp = nullptr;

static std::string sanitize_uid_for_filename(const char* uid) {
  std::string s(uid && uid[0] ? uid : "unknown");
  for (auto& c : s) {
    if (std::isalnum((unsigned char)c)) continue;
    if (c == '-' || c == '.' || c == '@' || c == '_') continue;
    c = '_';
  }
  if (s.empty()) s = "unknown";
  return s;
}

static void close_all_pcm_dump_files() {
  std::lock_guard<std::mutex> lock(g_dump_pcm_mutex);
  for (auto& kv : g_dump_pcm_files) {
    if (kv.second) {
      fclose(kv.second);
      kv.second = nullptr;
    }
  }
  g_dump_pcm_files.clear();
  if (g_dump_playback_fp) {
    fclose(g_dump_playback_fp);
    g_dump_playback_fp = nullptr;
  }
}

/* ============================================================
 * Audio frame observer callbacks
 * ============================================================ */

static int cb_on_playback_audio_frame(void* local_user, const char* channel_id, const audio_frame* frame) {
  (void)local_user; (void)channel_id;
  uint64_t n = ++g_audio_frames_received;
  if (g_dump_playback_pcm && frame && frame->buffer && frame->samples_per_channel > 0) {
    int bps = frame->bytes_per_sample > 0 ? frame->bytes_per_sample : 2;
    int ch = frame->channels > 0 ? frame->channels : 1;
    size_t nbytes = (size_t)frame->samples_per_channel * (size_t)ch * (size_t)bps;
    if (nbytes > 0) {
      std::lock_guard<std::mutex> lock(g_dump_pcm_mutex);
      if (!g_dump_playback_fp) {
        std::string path = g_dump_pcm_dir + "/playback_mixed.pcm";
        g_dump_playback_fp = fopen(path.c_str(), "ab");
        if (g_dump_playback_fp)
          fprintf(stderr, "[pcm-dump] writing mixed playback PCM -> %s (sr=%d ch=%d bps=%zd)\n",
                  path.c_str(), frame->samples_per_sec, ch, (size_t)bps);
        else
          fprintf(stderr, "[pcm-dump] fopen(%s) failed errno=%d\n", path.c_str(), errno);
      }
      if (g_dump_playback_fp)
        fwrite(frame->buffer, 1, nbytes, g_dump_playback_fp);
    }
  }
  if (frame && frame->buffer && frame->samples_per_channel > 0 && n % 100 == 0)
    fprintf(stderr, "[audio] received remote frame %llu (SDK callback, raw): %d samples, %d ch, %d Hz\n",
            (unsigned long long)n, frame->samples_per_channel, frame->channels, frame->samples_per_sec);
  return 1;
}
static int cb_on_record_audio_frame(void*, const char*, const audio_frame*)        { return 1; }
static int cb_on_mixed_audio_frame(void*, const char*, const audio_frame*)         { return 1; }
static int cb_on_ear_monitoring_audio_frame(void*, const audio_frame*)             { return 1; }
static int cb_on_playback_audio_frame_before_mixing(void* /*local_user*/, const char* /*channel_id*/,
                                                    user_id_t uid, const audio_frame* frame) {
  if (!g_dump_before_mixing_pcm || !frame || !frame->buffer || frame->samples_per_channel <= 0)
    return 1;
  int bps = frame->bytes_per_sample > 0 ? frame->bytes_per_sample : 2;
  int ch = frame->channels > 0 ? frame->channels : 1;
  size_t nbytes = (size_t)frame->samples_per_channel * (size_t)ch * (size_t)bps;
  if (nbytes == 0) return 1;

  const char* uid_key = (uid && uid[0]) ? uid : "unknown";
  std::string key(uid_key);

  std::lock_guard<std::mutex> lock(g_dump_pcm_mutex);
  FILE* fp = nullptr;
  auto it = g_dump_pcm_files.find(key);
  if (it == g_dump_pcm_files.end()) {
    std::string path = g_dump_pcm_dir + "/before_mixing_" + sanitize_uid_for_filename(uid_key) + ".pcm";
    fp = fopen(path.c_str(), "ab");
    if (!fp) {
      fprintf(stderr, "[pcm-dump] fopen(%s) failed errno=%d\n", path.c_str(), errno);
      return 1;
    }
    g_dump_pcm_files[key] = fp;
    fprintf(stderr, "[pcm-dump] writing before-mixing raw PCM -> %s (sr=%d ch=%d bps=%d)\n",
            path.c_str(), frame->samples_per_sec, ch, bps);
  } else {
    fp = it->second;
  }
  fwrite(frame->buffer, 1, nbytes, fp);
  fflush(fp);
  return 1;
}

static int cb_on_get_audio_frame_position(void*) {
  int pos = (int)AUDIO_FRAME_POSITION_PLAYBACK;
  if (g_dump_before_mixing_pcm)
    pos |= (int)AUDIO_FRAME_POSITION_BEFORE_MIXING;
  return pos;
}
static audio_params cb_on_get_playback_audio_frame_param(void*) {
  audio_params p = {}; p.sample_rate = 16000; p.channels = 1;
  p.mode = RAW_AUDIO_FRAME_OP_MODE_READ_ONLY; p.samples_per_call = 160;
  return p;
}
static audio_params cb_on_get_record_audio_frame_param(void*)         { audio_params p = {}; return p; }
static audio_params cb_on_get_mixed_audio_frame_param(void*)          { audio_params p = {}; return p; }
static audio_params cb_on_get_ear_monitoring_audio_frame_param(void*) { audio_params p = {}; return p; }

/* ============================================================
 * Video frame observer callback
 * ============================================================ */

static void cb_on_video_frame(void* obs_handle, const char* channel_id, const char* remote_uid, const video_frame* frame) {
  (void)obs_handle; (void)channel_id; (void)remote_uid;
  uint64_t n = ++g_video_frames_received;
  if (frame && n % 30 == 0)
    fprintf(stderr, "[video] received remote frame %llu (SDK callback, raw): %dx%d\n",
            (unsigned long long)n, frame->width, frame->height);
}

/* ============================================================
 * Local user observer callbacks
 * ============================================================ */

static local_user_observer g_luser_obs = {};

static void cb_on_user_audio_track_subscribed(void* local_user, const char* user_id, void* remote_audio_track) {
  (void)local_user;
  (void)user_id;
  (void)remote_audio_track;
  fprintf(stderr, "Subscribed to remote audio track.\n");
}

static void cb_on_user_video_track_subscribed(void* local_user, const char* user_id,
                                               const video_track_info* info, void* remote_video_track) {
  (void)local_user;
  (void)user_id;
  (void)info; (void)remote_video_track;
  fprintf(stderr, "Subscribed to remote video track.\n");
}

static void cb_on_audio_volume_indication(void* local_user, const audio_volume_info* speakers,
                                          unsigned int speaker_number, int total_volume) {
  (void)local_user;
  uint64_t n = ++g_audio_volume_cb_count;
  /* Log every 5th callback; always print all speaker slots the SDK returned (see speakers=N). */
  if (n % 5 != 0) return;
  fprintf(stderr, "[audio-volume] callbacks=%llu speakers=%u total=%d",
          (unsigned long long)n, speaker_number, total_volume);
  if (speaker_number > 1)
    fprintf(stderr, " (multi-speaker)");
  if (!speakers || speaker_number == 0) {
    fprintf(stderr, "\n");
    return;
  }
  fprintf(stderr, " |");
  for (unsigned int i = 0; i < speaker_number; ++i) {
    const char* uid = speakers[i].user_id ? speakers[i].user_id : "?";
    fprintf(stderr, " [%u] user_id=%s vol=%u", i, uid, speakers[i].volume);
    char* end = nullptr;
    unsigned long un = strtoul(uid, &end, 10);
    if (uid[0] && end != uid && *end == '\0')
      fprintf(stderr, " uid_int=%lu", un);
    else
      fprintf(stderr, " uid_int=n/a");
  }
  fprintf(stderr, "\n");
}

static std::atomic<uint64_t> g_user_info_cb_count{0};
static void cb_on_user_info_updated(void* local_user, user_id_t user_id, int msg, int val) {
  (void)local_user;
  uint64_t n = ++g_user_info_cb_count;
  if (n % 20 == 0) {
    const char* u = user_id ? user_id : "-";
    char* end = nullptr;
    unsigned long un = strtoul(u, &end, 10);
    if (end != u && *end == '\0')
      fprintf(stderr, "[user-info] callbacks=%llu user_id=%s uid_int=%lu msg=%d val=%d\n",
              (unsigned long long)n, u, un, msg, val);
    else
      fprintf(stderr, "[user-info] callbacks=%llu user_id=%s uid_int=n/a msg=%d val=%d\n",
              (unsigned long long)n, u, msg, val);
  }
}

/* ============================================================
 * Connection observer callbacks
 * ============================================================ */

static rtc_conn_observer g_conn_obs = {};

static void log_remote_user_id_uid_int(FILE* out, const char* prefix, const char* user_id) {
  const char* u = (user_id && user_id[0]) ? user_id : "?";
  char* end = nullptr;
  unsigned long n = strtoul(u, &end, 10);
  if (end != u && *end == '\0')
    fprintf(out, "%s user_id=%s uid_int=%lu\n", prefix, u, n);
  else
    fprintf(out, "%s user_id=%s uid_int=n/a (string account)\n", prefix, u);
}

static void cb_on_connected(void* conn, const rtc_conn_info* info, int reason) {
  (void)conn;
  const char* ch = (info && info->channel_id) ? info->channel_id : "?";
  const char* lid = (info && info->local_user_id) ? info->local_user_id : "?";
  unsigned int iuid = info ? info->internal_uid : 0;
  fprintf(stderr, "[conn] Connected: channel='%s' local_user_id='%s' internal_uid=%u reason=%d\n",
          ch, lid, iuid, reason);
}
static void cb_on_disconnected(void* conn, const rtc_conn_info* info, int reason) {
  (void)conn;
  if (info) {
    const char* ch = info->channel_id ? info->channel_id : "?";
    const char* lid = info->local_user_id ? info->local_user_id : "?";
    const unsigned int iuid = info->internal_uid;
    fprintf(stderr, "[conn] Disconnected: channel='%s' local_user_id='%s' internal_uid=%u reason=%d\n",
            ch, lid, iuid, reason);
  } else {
    fprintf(stderr, "[conn] Disconnected (reason=%d).\n", reason);
  }
}
static void cb_on_connecting(void* conn, const rtc_conn_info* info, int reason)    { (void)conn; (void)info; (void)reason; }
static void cb_on_reconnecting(void* conn, const rtc_conn_info* info, int reason)  { (void)conn; (void)info; fprintf(stderr, "[conn] Reconnecting (reason=%d).\n", reason); }
static void cb_on_reconnected(void* conn, const rtc_conn_info* info, int reason)   { (void)conn; (void)info; fprintf(stderr, "[conn] Reconnected (reason=%d).\n", reason); }
static void cb_on_conn_lost(void* conn, const rtc_conn_info* info)                 { (void)conn; (void)info; fprintf(stderr, "[conn] Connection lost.\n"); }
static void cb_on_conn_error(void* conn, int error, const char* msg)               { (void)conn; fprintf(stderr, "[conn] Error %d: %s\n", error, msg ? msg : ""); }
static void cb_on_conn_warning(void* conn, int warning, const char* msg)           { (void)conn; fprintf(stderr, "[conn] Warning %d: %s\n", warning, msg ? msg : ""); }
static void cb_on_user_joined(void* conn, const char* user_id) {
  (void)conn;
  log_remote_user_id_uid_int(stderr, "[conn] Remote user joined:", user_id);
}
static void cb_on_user_left(void* conn, const char* user_id, int reason) {
  (void)conn;
  const char* u = user_id ? user_id : "?";
  char* end = nullptr;
  unsigned long n = strtoul(u, &end, 10);
  if (end != u && *end == '\0')
    fprintf(stderr, "[conn] Remote user left: user_id=%s uid_int=%lu reason=%d\n", u, n, reason);
  else
    fprintf(stderr, "[conn] Remote user left: user_id=%s uid_int=n/a account/string reason=%d\n", u, reason);
}
static void cb_on_token_expire(void* conn, const char* token)                      { (void)conn; (void)token; fprintf(stderr, "[conn] Token privilege will expire.\n"); }

/* ============================================================
 * Send audio thread: 440 Hz PCM tone, 16 kHz mono, 10 ms chunks
 * (identical behavior to repro_pthread_init.cpp)
 * ============================================================ */

struct AudioSendArgs { void* sender; };

static void send_audio_loop(AudioSendArgs a) {
  const uint32_t sampleRate   = 16000;
  const size_t   samplesPerChunk = sampleRate / 100;  /* 160 samples = 10 ms */
  const size_t   numCh        = 1;
  const double   freqHz       = 440.0;
  const double   amplitude    = 0.3;
  int16_t buf[160];
  uint32_t ts = 0;
  uint64_t sent = 0;
  while (!g_exit) {
    for (size_t i = 0; i < samplesPerChunk; ++i) {
      double t = (ts * 0.001) + (double)i / sampleRate;
      buf[i] = (int16_t)(amplitude * std::sin(2.0 * M_PI * freqHz * t) * 32767);
    }
    int ret = g_pcm_send(a.sender, buf, ts,
                         (uint32_t)samplesPerChunk,
                         2,               /* bytes_per_sample (TWO_BYTES_PER_SAMPLE) */
                         (uint32_t)numCh,
                         sampleRate);
    if (ret < 0) break;
    ++sent;
    if (sent % 100 == 0)
      fprintf(stderr, "[audio] sent chunk #%llu ts=%u (160 samples 16kHz mono)\n",
              (unsigned long long)sent, ts);
    ts += 10;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/* ============================================================
 * Send video thread: 720p I420 badge + "A" pattern at 15 fps
 * (identical behavior to repro_pthread_init.cpp)
 * ============================================================ */

struct VideoSendArgs { void* sender; int width; int height; int fps; };

static void send_video_loop(VideoSendArgs a) {
  const int w = a.width, h = a.height;
  const int ySize  = w * h;
  const int uvSize = (w / 2) * (h / 2);
  std::vector<uint8_t> buf((size_t)(ySize + uvSize * 2));
  uint8_t* y = buf.data();
  uint8_t* u = y + ySize;
  uint8_t* v = u + uvSize;

  /* Dark grey background */
  memset(y, 90, (size_t)ySize);
  memset(u, 128, (size_t)uvSize);
  memset(v, 128, (size_t)uvSize);

  /* Cyan/teal badge rectangle */
  const int bw = w*3/4, bh = h*2/5, bx = (w-bw)/2, by = (h-bh)/2;
  for (int py = by; py < by+bh && py < h; ++py)
    for (int px = bx; px < bx+bw && px < w; ++px) {
      y[py*w + px] = 200;
      int ui = (py/2)*(w/2) + (px/2);
      u[ui] = 80; v[ui] = 180;
    }

  /* White border */
  for (int b = 0; b < 4; ++b) {
    int l = bx-b, r = bx+bw+b, t = by-b, bot = by+bh+b;
    if (l<0) l=0; if (r>w) r=w; if (t<0) t=0; if (bot>h) bot=h;
    for (int px = l; px < r; ++px) {
      if (t >= 0 && t < h)     y[t*w + px]     = 255;
      if (bot > 0 && bot <= h) y[(bot-1)*w + px] = 255;
    }
    for (int py = t; py < bot; ++py) {
      if (l >= 0 && l < w)     y[py*w + l]     = 255;
      if (r > 0 && r <= w)     y[py*w + (r-1)] = 255;
    }
  }

  /* "A" glyph in centre */
  const int ax = w/2-20, ay = h/2-16;
  for (int dy = 0; dy < 32; ++dy)
    for (int dx = 0; dx < 40; ++dx) {
      int gx = ax+dx, gy = ay+dy;
      if (gx < 0 || gx >= w || gy < 0 || gy >= h) continue;
      bool on = (dx>=4 && dx<36 && dy>=0 && dy<32) &&
                (dy<4 || dy>=28 || (dx>=14 && dx<26) || dx==4 || dx==35);
      if (on) y[gy*w + gx] = 255;
    }

  external_video_frame frame = {};
  frame.type     = 1;   /* VIDEO_BUFFER_RAW_DATA */
  frame.format   = 1;   /* VIDEO_PIXEL_I420 */
  frame.buffer   = buf.data();
  frame.stride   = w;
  frame.height   = h;
  frame.rotation = 0;

  const int intervalMs = 1000 / (a.fps > 0 ? a.fps : 15);
  uint64_t sent = 0;
  while (!g_exit) {
    frame.timestamp = (long long)std::chrono::steady_clock::now().time_since_epoch().count() / 1000;
    g_video_send(a.sender, &frame);
    ++sent;
    if (sent % 30 == 0)
      fprintf(stderr, "[video] sent frame #%llu %dx%d ts=%lld\n",
              (unsigned long long)sent, w, h, (long long)frame.timestamp);
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }
}

/* ============================================================
 * Teardown helper — cleans up connection-scoped resources
 * ============================================================ */

static void teardown_conn(void* local_user, void* conn,
                          void* send_audio_track, void* pcm_sender, void* audio_factory,
                          void* send_video_track, void* video_sender, void* video_factory) {
  if (conn) g_conn_disconnect(conn);
  if (local_user) {
    close_all_pcm_dump_files();
    if (g_luser_obs_registered) {
      g_luser_unreg_obs(local_user);
      g_luser_obs_registered = false;
    }
    if (g_audio_obs_registered) { g_luser_unreg_audio_obs(local_user); g_audio_obs_registered = false; }
    if (g_video_obs_registered) { g_luser_unreg_video_obs(local_user, g_vobs2_handle); g_video_obs_registered = false; }
  }
  if (conn && g_conn_obs_registered) {
    g_conn_unregister_obs(conn);
    g_conn_obs_registered = false;
  }
  if (g_vobs2_handle)  { g_vobs2_destroy(g_vobs2_handle); g_vobs2_handle = nullptr; }
  if (send_video_track)  g_video_track_destroy(send_video_track);
  if (video_sender)      g_video_sender_destroy(video_sender);
  if (video_factory)     g_factory_destroy(video_factory);
  if (send_audio_track)  g_audio_track_destroy(send_audio_track);
  if (pcm_sender)        g_pcm_sender_destroy(pcm_sender);
  if (audio_factory)     g_factory_destroy(audio_factory);
  if (conn) g_conn_destroy(conn);
}

/* ============================================================
 * main
 * ============================================================ */

int main(int argc, char* argv[]) {
  (void)argc; (void)argv;

  signal(SIGABRT, sig_handler);
  signal(SIGSEGV, sig_handler);
  signal(SIGINT,  sig_handler);

  /* Read all env vars */
  std::string appId(getenv_or("AGORA_APP_ID", "dummy_app_id_for_repro"));
  std::string channelId(getenv_or("AGORA_CHANNEL_ID", ""));
  std::string token(getenv_or("AGORA_TOKEN", ""));
  std::string uid(getenv_or("AGORA_UID", "0"));
  bool useStringUid   = getenv_bool("AGORA_USE_STRING_UID");
  bool registerAudioObserver = true;
  {
    const char* rao = getenv("AGORA_REGISTER_AUDIO_OBSERVER");
    if (rao && rao[0]) registerAudioObserver = getenv_bool("AGORA_REGISTER_AUDIO_OBSERVER");
  }
  bool enableAudioVolumeIndication = true;
  {
    const char* eavi = getenv("AGORA_ENABLE_AUDIO_VOLUME_INDICATION");
    if (eavi && eavi[0]) enableAudioVolumeIndication = getenv_bool("AGORA_ENABLE_AUDIO_VOLUME_INDICATION");
  }
  bool setChannelProfile = false;
  {
    const char* scp = getenv("AGORA_SET_CHANNEL_PROFILE");
    if (scp && scp[0]) setChannelProfile = getenv_bool("AGORA_SET_CHANNEL_PROFILE");
  }
  int channelProfile = parse_channel_profile(getenv_trimmed_or("AGORA_CHANNEL_PROFILE", "COMMUNICATION").c_str());
  bool setClientRoleType = true;
  {
    const char* scr = getenv("AGORA_SET_CLIENT_ROLE_TYPE");
    if (scr && scr[0]) setClientRoleType = getenv_bool("AGORA_SET_CLIENT_ROLE_TYPE");
  }
  int clientRoleType = parse_client_role_type(getenv_trimmed_or("AGORA_CLIENT_ROLE_TYPE", "AUDIENCE").c_str());
  bool registerConnObserver = false;
  {
    const char* rco = getenv("AGORA_REGISTER_CONN_OBSERVER");
    if (rco && rco[0]) registerConnObserver = getenv_bool("AGORA_REGISTER_CONN_OBSERVER");
  }
  bool registerLocalUserObserver = true;
  {
    const char* rluo = getenv("AGORA_REGISTER_LOCAL_USER_OBSERVER");
    if (rluo && rluo[0]) registerLocalUserObserver = getenv_bool("AGORA_REGISTER_LOCAL_USER_OBSERVER");
  }
  {
    const char* v = getenv("AGORA_LU_CB_AUDIO_SUB");
    if (v && v[0]) g_lu_cb_audio_sub = getenv_bool("AGORA_LU_CB_AUDIO_SUB");
  }
  {
    const char* v = getenv("AGORA_LU_CB_VIDEO_SUB");
    if (v && v[0]) g_lu_cb_video_sub = getenv_bool("AGORA_LU_CB_VIDEO_SUB");
  }
  /* Default volume callback on when volume indication is on; set AGORA_LU_CB_VOLUME_IND=0 to disable callback only. */
  g_lu_cb_volume_ind = enableAudioVolumeIndication;
  {
    const char* v = getenv("AGORA_LU_CB_VOLUME_IND");
    if (v && v[0]) g_lu_cb_volume_ind = getenv_bool("AGORA_LU_CB_VOLUME_IND");
  }
  {
    const char* v = getenv("AGORA_LU_CB_USER_INFO_UPDATED");
    if (v && v[0]) g_lu_cb_user_info = getenv_bool("AGORA_LU_CB_USER_INFO_UPDATED");
  }
  unsigned int areaCode = parse_area_code(getenv_trimmed_or("AGORA_AREA_CODE", "GLOB").c_str());
  bool setServiceChannelProfile = false;
  {
    const char* v = getenv("AGORA_SET_SERVICE_CHANNEL_PROFILE");
    if (v && v[0]) setServiceChannelProfile = getenv_bool("AGORA_SET_SERVICE_CHANNEL_PROFILE");
  }
  int serviceChannelProfile = parse_channel_profile(getenv_trimmed_or("AGORA_SERVICE_CHANNEL_PROFILE", "COMMUNICATION").c_str());
  bool setServiceAudioScenario = false;
  {
    const char* v = getenv("AGORA_SET_SERVICE_AUDIO_SCENARIO");
    if (v && v[0]) setServiceAudioScenario = getenv_bool("AGORA_SET_SERVICE_AUDIO_SCENARIO");
  }
  int serviceAudioScenario = parse_audio_scenario(getenv_trimmed_or("AGORA_SERVICE_AUDIO_SCENARIO", "DEFAULT").c_str());
  int volIndIntervalMs = getenv_int_or("AGORA_VOLUME_INDICATION_INTERVAL_MS", 1000);
  int volIndSmooth = getenv_int_or("AGORA_VOLUME_INDICATION_SMOOTH", 3);
  bool volIndVad = getenv_bool_default("AGORA_VOLUME_INDICATION_VAD", false);
  bool setLocalUserAudioScenario = false;
  {
    const char* v = getenv("AGORA_SET_LOCAL_USER_AUDIO_SCENARIO");
    if (v && v[0]) setLocalUserAudioScenario = getenv_bool("AGORA_SET_LOCAL_USER_AUDIO_SCENARIO");
  }
  int localUserAudioScenario = parse_audio_scenario(getenv_trimmed_or("AGORA_LOCAL_USER_AUDIO_SCENARIO", "DEFAULT").c_str());
  bool receiveVideo   = getenv_bool("AGORA_RECEIVE_VIDEO");
  bool sendAudio      = getenv_bool("AGORA_SEND_AUDIO");
  bool sendVideo      = getenv_bool("AGORA_SEND_VIDEO");
  int  joinDurationSec = getenv_join_duration_sec(60);
  std::string stopAfter(getenv_or("AGORA_REPRO_STOP_AFTER", ""));
  bool dumpBeforeMixingPcm = false;
  {
    const char* v = getenv("AGORA_DUMP_BEFORE_MIXING_PCM");
    if (v && v[0]) dumpBeforeMixingPcm = getenv_bool("AGORA_DUMP_BEFORE_MIXING_PCM");
  }
  bool dumpPlaybackPcm = false;
  {
    const char* v = getenv("AGORA_DUMP_PLAYBACK_PCM");
    if (v && v[0]) dumpPlaybackPcm = getenv_bool("AGORA_DUMP_PLAYBACK_PCM");
  }
  std::string dumpPcmDir = getenv_trimmed_or("AGORA_DUMP_PCM_DIR", "/tmp/agora_pcm_dump");
  if (dumpBeforeMixingPcm || dumpPlaybackPcm)
    registerAudioObserver = true;
  bool encryptionEnable = getenv_bool("AGORA_ENCRYPTION_ENABLE");
  std::string encModeStr(getenv_trimmed_or("AGORA_ENCRYPTION_MODE", ""));
  std::string encSecret(getenv_trimmed_or("AGORA_ENCRYPTION_SECRET", ""));
  std::string encSalt(getenv_trimmed_or("AGORA_ENCRYPTION_SALT", ""));

  fprintf(stderr, "[v2] repro_v2_full: v2 C API + dlopen/dlsym — parallel to repro_pthread_init.\n");
  fprintf(stderr, "Join duration: %d s (AGORA_JOIN_DURATION_SEC; 0=until Ctrl+C).\n", joinDurationSec);
  if (!stopAfter.empty())
    fprintf(stderr, "Bisect mode: will stop after '%s' (AGORA_REPRO_STOP_AFTER).\n", stopAfter.c_str());
  g_dump_before_mixing_pcm = dumpBeforeMixingPcm;
  g_dump_playback_pcm = dumpPlaybackPcm;
  g_dump_pcm_dir = dumpPcmDir;
  if (dumpBeforeMixingPcm || dumpPlaybackPcm) {
    if (mkdir(g_dump_pcm_dir.c_str(), 0755) != 0 && errno != EEXIST)
      fprintf(stderr, "AGORA_DUMP_PCM_DIR: mkdir(%s) errno=%d (continuing anyway)\n", g_dump_pcm_dir.c_str(), errno);
    if (dumpBeforeMixingPcm)
      fprintf(stderr,
              "AGORA_DUMP_BEFORE_MIXING_PCM=1: per-remote raw PCM -> %s/before_mixing_<uid>.pcm\n",
              g_dump_pcm_dir.c_str());
    if (dumpPlaybackPcm)
      fprintf(stderr,
              "AGORA_DUMP_PLAYBACK_PCM=1: mixed playback -> %s/playback_mixed.pcm\n",
              g_dump_pcm_dir.c_str());
  }
  if (enableAudioVolumeIndication) {
    if (!registerLocalUserObserver)
      fprintf(stderr,
              "Warning: AGORA_ENABLE_AUDIO_VOLUME_INDICATION=1 but AGORA_REGISTER_LOCAL_USER_OBSERVER=0 — "
              "volume callbacks will not run.\n");
    else if (!g_lu_cb_volume_ind)
      fprintf(stderr,
              "Warning: AGORA_ENABLE_AUDIO_VOLUME_INDICATION=1 but AGORA_LU_CB_VOLUME_IND=0 — "
              "on_audio_volume_indication will not fire.\n");
  }

  if (token.empty()) token = appId;
  g_enable_audio_observer = registerAudioObserver;

  /* ------ 0. dlopen + dlsym ------ */
  fprintf(stderr, "0. Loading Agora SDK via dlopen (v2 C API, dlsym only — no -lagora_rtc_sdk link)...\n");
  void* lib = dlopen("libagora_rtc_sdk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lib) { fprintf(stderr, "dlopen libagora_rtc_sdk.so failed: %s\n", dlerror()); return 1; }
  {
    typedef const char* (*pfn_get_sdk_version)(int*);
    pfn_get_sdk_version get_ver = (pfn_get_sdk_version)dlsym(lib, "getAgoraSdkVersion");
    if (get_ver) {
      int b = 0;
      const char* v = get_ver(&b);
      fprintf(stderr, "[v2] Agora SDK version: %s (build %d)\n", v ? v : "?", b);
    } else {
      fprintf(stderr, "[v2] getAgoraSdkVersion: dlsym failed (optional)\n");
    }
  }
  if (load_symbols(lib) != 0) { dlclose(lib); return 1; }
  fprintf(stderr, "dlopen + all dlsym OK.\n");

  /* ------ Scheduler / RLIMIT_RTPRIO diagnostic (same as C++ repro) ------ */
  {
    int fifo_min = sched_get_priority_min(SCHED_FIFO);
    int fifo_max = sched_get_priority_max(SCHED_FIFO);
    int policy = 0;
    struct sched_param param = {};
    (void)pthread_getschedparam(pthread_self(), &policy, &param);
    const char* pol_name = (policy == SCHED_OTHER) ? "SCHED_OTHER" :
                           (policy == SCHED_FIFO)  ? "SCHED_FIFO"  :
                           (policy == SCHED_RR)    ? "SCHED_RR"    : "?";
    fprintf(stderr,
            "[scheduler] fifo_min_prio=%d fifo_max_prio=%d (assert allows new_prio==-1 or [%d..%d]);"
            " current thread policy=%d (%s) priority=%d\n",
            fifo_min, fifo_max, fifo_min, fifo_max, policy, pol_name, param.sched_priority);
    struct rlimit rtp = {};
    if (getrlimit(RLIMIT_RTPRIO, &rtp) == 0)
      fprintf(stderr, "[scheduler] RLIMIT_RTPRIO cur=%lld max=%lld (0 => no RT priority allowed)\n",
              (long long)rtp.rlim_cur, (long long)rtp.rlim_max);
  }

  /* ------ Thread priority note (v2 C API has no threadPriority in agora_service_config) ------ */
  {
    const char* tp = getenv("AGORA_THREAD_PRIORITY");
    if (tp && tp[0]) {
      int v = 2;
      if (sscanf(tp, "%d", &v) == 1 && v >= 0 && v <= 5)
        fprintf(stderr,
                "AGORA_THREAD_PRIORITY=%d (0=LOWEST 2=NORMAL 5=CRITICAL) — "
                "noted but not configurable via agora_service_config in v2 C API.\n", v);
    } else {
      fprintf(stderr, "threadPriority: unset — v2 agora_service_config has no threadPriority field.\n");
    }
  }

  /* ------ 1. Create and initialize service ------ */
  fprintf(stderr, "Creating Agora service (v2)...\n");
  void* svc = g_svc_create();
  if (!svc) { fprintf(stderr, "agora_service_create() returned null\n"); dlclose(lib); return 1; }

  agora_service_config svc_cfg = {};
  svc_cfg.app_id                = appId.c_str();
  svc_cfg.enable_audio_processor = 1;
  svc_cfg.enable_audio_device    = 0;
  svc_cfg.enable_video           = (receiveVideo || sendVideo) ? 1 : 0;
  svc_cfg.use_string_uid         = useStringUid ? 1 : 0;
  if (useStringUid)
    fprintf(stderr, "AGORA_USE_STRING_UID=1: string user account mode; AGORA_UID=\"%s\"\n", uid.c_str());
  else
    fprintf(stderr, "Numeric UID mode (default); AGORA_UID=\"%s\" (set AGORA_USE_STRING_UID=1 for string account)\n", uid.c_str());
  svc_cfg.area_code = areaCode;
  fprintf(stderr, "AGORA_AREA_CODE -> 0x%x\n", areaCode);
  if (setServiceChannelProfile) {
    svc_cfg.channel_profile = serviceChannelProfile;
    fprintf(stderr, "Service channel_profile=%d (%s)\n", serviceChannelProfile,
            (serviceChannelProfile == 1) ? "LIVE_BROADCASTING" : "COMMUNICATION");
  } else {
    fprintf(stderr, "AGORA_SET_SERVICE_CHANNEL_PROFILE=0: not setting service channel_profile.\n");
  }
  if (setServiceAudioScenario) {
    svc_cfg.audio_scenario = serviceAudioScenario;
    fprintf(stderr, "Service audio_scenario=%d\n", serviceAudioScenario);
  } else {
    fprintf(stderr, "AGORA_SET_SERVICE_AUDIO_SCENARIO=0: not setting service audio_scenario.\n");
  }

  fprintf(stderr, "Calling agora_service_initialize()...\n");
  int ret = g_svc_init(svc, &svc_cfg);
  if (ret != 0) {
    const char* err = (ret==-1)?"ERR_FAILED":(ret==-2)?"ERR_INVALID_ARGUMENT":
                     (ret==-3)?"ERR_NOT_READY":(ret==-7)?"ERR_NOT_INITIALIZED":
                     (ret==-21)?"ERR_INIT_NET_ENGINE":"?";
    fprintf(stderr, "agora_service_initialize() returned %d (%s)\n", ret, err);
    g_svc_release(svc); dlclose(lib); return 1;
  }
  fprintf(stderr, "agora_service_initialize() returned 0 OK.\n");

  /* ------ Log file (SDK binary format; plain-text writer not available in v2 C API) ------ */
  {
    std::string logPath(getenv_trimmed_or("AGORA_LOG_FILE", "/app/agora_sdk.log"));
    int lr = g_svc_set_log_file(svc, logPath.c_str(), 10u * 1024u * 1024u /* 10 MB */);
    if (lr == 0) {
      g_svc_set_log_filter(svc, 0x080f /* LOG_FILTER_DEBUG */);
      fprintf(stderr, "Agora SDK logs -> %s (binary SDK format). Set AGORA_LOG_FILE to override.\n", logPath.c_str());
    } else {
      fprintf(stderr, "agora_service_set_log_file(%s) failed %d; SDK logs may only go to internal stderr.\n",
              logPath.c_str(), lr);
    }
  }

  if (stopAfter == "init") {
    fprintf(stderr, "Stopping after init (AGORA_REPRO_STOP_AFTER=init).\n");
    if (g_svc_at_exit) g_svc_at_exit(svc);
    g_svc_release(svc); dlclose(lib); return 0;
  }

  /* ------ 2. Default local audio track (warm-up / bisect) ------ */
  fprintf(stderr, "Creating local audio track (v2)...\n");
  void* default_audio_track = g_svc_create_local_audio_track(svc);
  if (!default_audio_track) fprintf(stderr, "agora_service_create_local_audio_track() returned null\n");
  else                       fprintf(stderr, "agora_service_create_local_audio_track() OK\n");
  if (default_audio_track) { g_audio_track_destroy(default_audio_track); default_audio_track = nullptr; }

  if (stopAfter == "create_local_audio_track") {
    fprintf(stderr, "Stopping after create_local_audio_track (AGORA_REPRO_STOP_AFTER=create_local_audio_track).\n");
    if (g_svc_at_exit) g_svc_at_exit(svc);
    g_svc_release(svc); dlclose(lib); return 0;
  }

  if (channelId.empty() && (stopAfter == "connect" || stopAfter == "publish")) {
    fprintf(stderr, "AGORA_REPRO_STOP_AFTER=%s requires AGORA_CHANNEL_ID to be set.\n", stopAfter.c_str());
    if (g_svc_at_exit) g_svc_at_exit(svc);
    g_svc_release(svc); dlclose(lib); return 1;
  }

  /* ------ 3. Connect (if channelId is set) ------ */
  if (!channelId.empty()) {
    rtc_conn_config conn_cfg = {};
    if (setClientRoleType) {
      conn_cfg.client_role_type = clientRoleType;
      fprintf(stderr, "Set client_role_type=%d (%s)\n", clientRoleType, (clientRoleType == 1) ? "BROADCASTER" : "AUDIENCE");
    } else {
      fprintf(stderr, "AGORA_SET_CLIENT_ROLE_TYPE=0: not setting client_role_type.\n");
    }
    if (setChannelProfile) {
      conn_cfg.channel_profile = channelProfile;
      fprintf(stderr, "Set channel_profile=%d (%s)\n", channelProfile, (channelProfile == 1) ? "LIVE_BROADCASTING" : "COMMUNICATION");
    } else {
      fprintf(stderr, "AGORA_SET_CHANNEL_PROFILE=0: not setting channel_profile.\n");
    }
    conn_cfg.auto_subscribe_audio         = 1;
    conn_cfg.auto_subscribe_video         = receiveVideo ? 1 : 0;
    conn_cfg.enable_audio_recording_or_playout = 0;

    fprintf(stderr, "Connecting to channel '%s' (uid=%s)... receive_video=%d send_audio=%d send_video=%d\n",
            channelId.c_str(), uid.c_str(), receiveVideo ? 1 : 0, sendAudio ? 1 : 0, sendVideo ? 1 : 0);

    void* conn = g_conn_create(svc, &conn_cfg);
    if (!conn) {
      fprintf(stderr, "agora_rtc_conn_create() failed\n");
      if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
    }

    /* Connection observer */
    memset(&g_conn_obs, 0, sizeof(g_conn_obs));
    if (registerConnObserver) {
      g_conn_obs.on_connected    = cb_on_connected;
      g_conn_obs.on_disconnected = cb_on_disconnected;
      g_conn_obs.on_connecting   = cb_on_connecting;
      g_conn_obs.on_reconnecting = cb_on_reconnecting;
      g_conn_obs.on_reconnected  = cb_on_reconnected;
      g_conn_obs.on_connection_lost = cb_on_conn_lost;
      g_conn_obs.on_error        = cb_on_conn_error;
      g_conn_obs.on_warning      = cb_on_conn_warning;
      g_conn_obs.on_user_joined  = cb_on_user_joined;
      g_conn_obs.on_user_left    = cb_on_user_left;
      g_conn_obs.on_token_privilege_will_expire = cb_on_token_expire;
      int cor = g_conn_register_obs(conn, &g_conn_obs);
      if (cor == 0) {
        g_conn_obs_registered = true;
        fprintf(stderr, "Connection observer registered.\n");
      } else {
        fprintf(stderr, "agora_rtc_conn_register_observer() failed %d\n", cor);
      }
    } else {
      fprintf(stderr, "AGORA_REGISTER_CONN_OBSERVER=0: connection observer registration disabled.\n");
    }

    void* local_user = g_conn_get_local_user(conn);
    if (!local_user) {
      fprintf(stderr, "agora_rtc_conn_get_local_user() returned null\n");
      if (g_conn_obs_registered) {
        g_conn_unregister_obs(conn);
        g_conn_obs_registered = false;
      }
      g_conn_disconnect(conn);
      g_conn_destroy(conn);
      if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
    }

    g_luser_sub_all_audio(local_user);
    if (receiveVideo) {
      video_subscription_options vopt = {}; /* type=0 (HIGH), encoded_frame_only=0 */
      g_luser_sub_all_video(local_user, &vopt);
    }

    ret = g_luser_set_playback_params(local_user, 1, 16000, (int)RAW_AUDIO_FRAME_OP_MODE_READ_ONLY, 160);
    if (ret != 0) {
      fprintf(stderr, "agora_local_user_set_playback_audio_frame_parameters() failed %d\n", ret);
      g_conn_destroy(conn);
      if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
    }
    if (g_dump_before_mixing_pcm) {
      int bmr = g_luser_set_before_mixing_params(local_user, 1u, 16000u);
      if (bmr == 0)
        fprintf(stderr, "agora_local_user_set_playback_audio_frame_before_mixing_parameters(1, 16000) OK.\n");
      else
        fprintf(stderr, "agora_local_user_set_playback_audio_frame_before_mixing_parameters() failed %d\n", bmr);
    }
    if (setLocalUserAudioScenario) {
      int asr = g_luser_set_audio_scenario(local_user, localUserAudioScenario);
      if (asr == 0) fprintf(stderr, "agora_local_user_set_audio_scenario(%d) OK.\n", localUserAudioScenario);
      else fprintf(stderr, "agora_local_user_set_audio_scenario() failed %d\n", asr);
    } else {
      fprintf(stderr, "AGORA_SET_LOCAL_USER_AUDIO_SCENARIO=0: not calling agora_local_user_set_audio_scenario.\n");
    }

    if (enableAudioVolumeIndication) {
      int vir = g_luser_set_volume_indication(local_user, volIndIntervalMs, volIndSmooth, volIndVad);
      if (vir == 0)
        fprintf(stderr, "Audio volume indication enabled (interval=%dms smooth=%d vad=%d).\n",
                volIndIntervalMs, volIndSmooth, volIndVad ? 1 : 0);
      else fprintf(stderr, "agora_local_user_set_audio_volume_indication_parameters() failed %d\n", vir);
    } else {
      fprintf(stderr, "AGORA_ENABLE_AUDIO_VOLUME_INDICATION=0: audio volume indication disabled.\n");
    }

    /* Audio frame observer */
    memset(&g_audio_obs, 0, sizeof(g_audio_obs));
    g_audio_obs.on_record_audio_frame               = cb_on_record_audio_frame;
    g_audio_obs.on_playback_audio_frame              = cb_on_playback_audio_frame;
    g_audio_obs.on_mixed_audio_frame                 = cb_on_mixed_audio_frame;
    g_audio_obs.on_ear_monitoring_audio_frame        = cb_on_ear_monitoring_audio_frame;
    g_audio_obs.on_playback_audio_frame_before_mixing = cb_on_playback_audio_frame_before_mixing;
    g_audio_obs.on_get_audio_frame_position          = cb_on_get_audio_frame_position;
    g_audio_obs.on_get_playback_audio_frame_param    = cb_on_get_playback_audio_frame_param;
    g_audio_obs.on_get_record_audio_frame_param      = cb_on_get_record_audio_frame_param;
    g_audio_obs.on_get_mixed_audio_frame_param       = cb_on_get_mixed_audio_frame_param;
    g_audio_obs.on_get_ear_monitoring_audio_frame_param = cb_on_get_ear_monitoring_audio_frame_param;
    if (g_enable_audio_observer) {
      int aor = g_luser_reg_audio_obs(local_user, &g_audio_obs);
      if (aor == 0) {
        g_audio_obs_registered = true;
        fprintf(stderr, "Audio observer registered.\n");
      } else {
        fprintf(stderr, "agora_local_user_register_audio_frame_observer() failed %d\n", aor);
      }
    } else {
      fprintf(stderr, "AGORA_REGISTER_AUDIO_OBSERVER=0: audio observer registration disabled.\n");
    }

    /* Video frame observer (create only when video receive is requested) */
    if (receiveVideo) {
      memset(&g_vobs2_impl, 0, sizeof(g_vobs2_impl));
      g_vobs2_impl.on_frame = cb_on_video_frame;
      g_vobs2_handle = g_vobs2_create(&g_vobs2_impl);
      if (g_vobs2_handle) {
        int vor = g_luser_reg_video_obs(local_user, g_vobs2_handle);
        if (vor == 0) {
          g_video_obs_registered = true;
          fprintf(stderr, "Video observer registered.\n");
        } else {
          fprintf(stderr, "agora_local_user_register_video_frame_observer() failed %d\n", vor);
        }
      } else {
        fprintf(stderr, "agora_video_frame_observer2_create() returned null\n");
      }
    }

    /* Local user observer (optional due stability concerns in some SDK/runtime combos) */
    if (registerLocalUserObserver) {
      memset(&g_luser_obs, 0, sizeof(g_luser_obs));
      if (g_lu_cb_audio_sub) g_luser_obs.on_user_audio_track_subscribed = cb_on_user_audio_track_subscribed;
      if (g_lu_cb_video_sub) g_luser_obs.on_user_video_track_subscribed = cb_on_user_video_track_subscribed;
      if (g_lu_cb_volume_ind) g_luser_obs.on_audio_volume_indication = cb_on_audio_volume_indication;
      if (g_lu_cb_user_info) g_luser_obs.on_user_info_updated = cb_on_user_info_updated;
      fprintf(stderr,
              "Local user observer callbacks: audio_sub=%d video_sub=%d volume_ind=%d user_info=%d\n",
              g_lu_cb_audio_sub ? 1 : 0, g_lu_cb_video_sub ? 1 : 0, g_lu_cb_volume_ind ? 1 : 0,
              g_lu_cb_user_info ? 1 : 0);
      int luor = g_luser_reg_obs(local_user, &g_luser_obs);
      if (luor == 0) {
        g_luser_obs_registered = true;
        fprintf(stderr, "Local user observer registered.\n");
      } else {
        fprintf(stderr, "agora_local_user_register_observer() failed %d\n", luor);
      }
    } else {
      fprintf(stderr, "AGORA_REGISTER_LOCAL_USER_OBSERVER=0: local user observer registration disabled.\n");
    }

    /* ------ Encryption ------ */
    if (encryptionEnable) {
      if (encSecret.empty()) {
        fprintf(stderr, "AGORA_ENCRYPTION_ENABLE=1 requires AGORA_ENCRYPTION_SECRET to be set.\n");
        teardown_conn(local_user, conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
      }
      int modeVal = parse_encryption_mode(encModeStr.c_str());
      if (modeVal < 0) {
        fprintf(stderr, "Invalid/missing AGORA_ENCRYPTION_MODE '%s'. Use 1-8 or name (e.g. 7 or AES-128-GCM2).\n",
                encModeStr.c_str());
        teardown_conn(local_user, conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
      }
      encryption_config enc_cfg = {};
      enc_cfg.encryption_mode = modeVal;
      enc_cfg.encryption_key  = encSecret.c_str();
      memset(enc_cfg.encryption_kdf_salt, 0, sizeof(enc_cfg.encryption_kdf_salt));
      if (encryption_mode_needs_salt(modeVal)) {
        if (encSalt.empty()) {
          fprintf(stderr, "AGORA_ENCRYPTION_MODE %s (AES-*-GCM2) requires AGORA_ENCRYPTION_SALT (Base64 32-byte).\n",
                  encModeStr.c_str());
          teardown_conn(local_user, conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
          if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
        }
        int saltLen = base64_decode(encSalt.c_str(), encSalt.size(), enc_cfg.encryption_kdf_salt, 32);
        if (saltLen < 32) {
          fprintf(stderr, "AGORA_ENCRYPTION_SALT must decode to >= 32 bytes (got %d). Use: openssl rand -base64 32\n",
                  saltLen);
          teardown_conn(local_user, conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
          if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
        }
        bool saltNonZero = false;
        for (int i = 0; i < 32; i++) if (enc_cfg.encryption_kdf_salt[i]) { saltNonZero = true; break; }
        if (!saltNonZero) {
          fprintf(stderr, "AGORA_ENCRYPTION_SALT decoded to 32 zero bytes; use a non-zero salt for GCM2.\n");
          teardown_conn(local_user, conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
          if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
        }
      }
      ret = g_conn_enable_encryption(conn, 1, &enc_cfg);
      if (ret != 0) {
        fprintf(stderr, "agora_rtc_conn_enable_encryption() failed %d (all clients must use same config).\n", ret);
        teardown_conn(local_user, conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
      }
      static const char* const kModeNames[] = {
        "?", "AES_128_XTS", "AES_128_ECB", "AES_256_XTS", "SM4_128_ECB",
        "AES_128_GCM", "AES_256_GCM", "AES_128_GCM2", "AES_256_GCM2"
      };
      const char* modeName = (modeVal >= 1 && modeVal <= 8) ? kModeNames[modeVal] : "?";
      fprintf(stderr, "Encryption enabled: mode=%s (%d), key_len=%zu, salt=%s (all clients must match).\n",
              modeName, modeVal, encSecret.size(), encryption_mode_needs_salt(modeVal) ? "32 bytes" : "n/a");
    }

    /* ------ Optional: custom PCM audio send track ------ */
    void* audio_factory    = nullptr;
    void* pcm_sender       = nullptr;
    void* send_audio_track = nullptr;
    std::thread audio_thread;

    if (sendAudio) {
      audio_factory = g_svc_create_factory(svc);
      if (audio_factory) {
        pcm_sender = g_factory_create_pcm_sender(audio_factory);
        if (pcm_sender) {
          send_audio_track = g_svc_create_custom_audio_pcm(svc, pcm_sender);
          if (send_audio_track) {
            g_audio_track_set_enabled(send_audio_track, 1);
            fprintf(stderr, "Send audio track created (440 Hz tone).\n");
          }
        }
      }
    }

    /* ------ Optional: custom I420 video send track (720p) ------ */
    void* video_factory    = nullptr;
    void* video_sender     = nullptr;
    void* send_video_track = nullptr;
    std::thread video_thread;
    const int vw = 1280, vh = 720, vfps = 15;

    if (sendVideo) {
      video_factory = g_svc_create_factory(svc);
      if (video_factory) {
        video_sender = g_factory_create_video_sender(video_factory);
        if (video_sender) {
          send_video_track = g_svc_create_custom_video_frame(svc, video_sender);
          if (send_video_track) {
            video_encoder_config enc_cfg2 = {};
            enc_cfg2.codec_type         = 2;    /* VIDEO_CODEC_H264 */
            enc_cfg2.dimensions.width   = vw;
            enc_cfg2.dimensions.height  = vh;
            enc_cfg2.frame_rate         = vfps;
            enc_cfg2.bitrate            = 2500; /* Kbps */
            g_video_track_set_enc_cfg(send_video_track, &enc_cfg2);
            g_video_track_set_enabled(send_video_track, 1);
            fprintf(stderr, "Send video track created: 720p %dx%d @ %d fps (badge + 'A' image).\n", vw, vh, vfps);
          }
        }
      }
    }

    /* ------ connect() ------ */
    ret = g_conn_connect(conn, token.c_str(), channelId.c_str(), uid.c_str());
    if (ret != 0) {
      fprintf(stderr, "agora_rtc_conn_connect() failed %d\n", ret);
      teardown_conn(local_user, conn, send_audio_track, pcm_sender, audio_factory,
                    send_video_track, video_sender, video_factory);
      if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 1;
    }
    fprintf(stderr,
            "agora_rtc_conn_connect() OK; join_user_id='%s' (see [conn] Connected for local_user_id + internal_uid).\n",
            uid.c_str());

    if (stopAfter == "connect") {
      fprintf(stderr, "Stopping after connect (AGORA_REPRO_STOP_AFTER=connect).\n");
      teardown_conn(local_user, conn, send_audio_track, pcm_sender, audio_factory,
                    send_video_track, video_sender, video_factory);
      if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 0;
    }

    /* ------ Publish tracks and launch send threads ------ */
    if (sendAudio && send_audio_track) {
      g_luser_publish_audio(local_user, send_audio_track);
      fprintf(stderr, "Published audio track.\n");
      audio_thread = std::thread(send_audio_loop, AudioSendArgs{pcm_sender});
    }
    if (sendVideo && send_video_track) {
      g_luser_publish_video(local_user, send_video_track);
      fprintf(stderr, "Published video track.\n");
      video_thread = std::thread(send_video_loop, VideoSendArgs{video_sender, vw, vh, vfps});
    }

    if (stopAfter == "publish") {
      fprintf(stderr, "Stopping after publish (AGORA_REPRO_STOP_AFTER=publish). Letting send threads run 2s.\n");
      std::this_thread::sleep_for(std::chrono::seconds(2));
      g_exit = true;
      if (audio_thread.joinable()) audio_thread.join();
      if (video_thread.joinable()) video_thread.join();
      if (sendAudio && send_audio_track) g_luser_unpublish_audio(local_user, send_audio_track);
      if (sendVideo && send_video_track) g_luser_unpublish_video(local_user, send_video_track);
      teardown_conn(local_user, conn, send_audio_track, pcm_sender, audio_factory,
                    send_video_track, video_sender, video_factory);
      if (g_svc_at_exit) g_svc_at_exit(svc); g_svc_release(svc); dlclose(lib); return 0;
    }

    /* ------ Wait in channel ------ */
    if (joinDurationSec > 0)
      fprintf(stderr, "Joined channel. Staying %d s (or Ctrl+C).\n", joinDurationSec);
    else
      fprintf(stderr, "Joined channel. Running until Ctrl+C.\n");

    if (joinDurationSec > 0) {
      for (int i = 0; i < joinDurationSec && !g_exit; ++i) {
        if (i > 0 && i % 10 == 0)
          fprintf(stderr, "[channel] %d s remaining\n", joinDurationSec - i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (!g_exit) fprintf(stderr, "Duration reached, leaving channel.\n");
    } else {
      while (!g_exit)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    /* ------ Teardown ------ */
    g_exit = true;
    if (audio_thread.joinable()) audio_thread.join();
    if (video_thread.joinable()) video_thread.join();

    if (sendAudio && send_audio_track) g_luser_unpublish_audio(local_user, send_audio_track);
    if (sendVideo && send_video_track) g_luser_unpublish_video(local_user, send_video_track);

    fprintf(stderr, "Disconnecting. Audio frames received: %llu  Video frames received: %llu\n",
            (unsigned long long)g_audio_frames_received.load(),
            (unsigned long long)g_video_frames_received.load());

    teardown_conn(local_user, conn, send_audio_track, pcm_sender, audio_factory,
                  send_video_track, video_sender, video_factory);
    fprintf(stderr, "Disconnected.\n");

  } else {
    fprintf(stderr, "No AGORA_CHANNEL_ID set — service init + audio track creation only.\n");
  }

  fprintf(stderr, "Releasing Agora service (v2).\n");
  if (g_svc_at_exit) g_svc_at_exit(svc);
  g_svc_release(svc);
  dlclose(lib);
  fprintf(stderr, "Done.\n");
  return 0;
}
