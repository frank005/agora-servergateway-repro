#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <dlfcn.h>

#include "c/api2/agora_service.h"
#include "c/api2/agora_rtc_conn.h"
#include "c/api2/agora_local_user.h"

static const char* getenv_or(const char* name, const char* def) {
  const char* v = getenv(name);
  return (v && v[0]) ? v : def;
}

static bool getenv_bool(const char* name) {
  const char* v = getenv(name);
  if (!v || !v[0]) return false;
  return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

static int getenv_int(const char* name, int def) {
  const char* v = getenv(name);
  if (!v || !v[0]) return def;
  char* end = nullptr;
  long n = strtol(v, &end, 10);
  if (end == v) return def;
  if (n < 0) n = 0;
  if (n > 86400) n = 86400;
  return (int)n;
}

// Base64 decode; writes at most maxOut bytes, returns number decoded.
static int base64_decode(const char* in, size_t inLen, unsigned char* out, int maxOut) {
  static const unsigned char T[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,62,255,255,255,63,52,53,54,55,56,57,58,59,60,61,255,255,255,255,255,255,
    255,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,255,255,255,255,255,
    255,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,255,255,255,255,255,
  };
  int n = 0;
  unsigned val = 0;
  int bits = 0;
  for (size_t i = 0; i < inLen && n < maxOut; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c >= sizeof(T) || T[c] == 255) continue;
    val = (val << 6) | T[c];
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[n++] = (unsigned char)(val >> bits);
    }
  }
  return n;
}

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
  while (len > 0 && (m[len - 1] == ' ' || m[len - 1] == '\t')) --len;
  size_t hash = m.find('#');
  if (hash != std::string::npos) {
    if (hash < len) len = hash;
    while (len > 0 && (m[len - 1] == ' ' || m[len - 1] == '\t')) --len;
  }
  m.resize(len);
  if (m.empty()) return -1;
  for (auto& ch : m) { if (ch == '-') ch = '_'; else if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); }
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

int main() {
  const char* appId = getenv_or("AGORA_APP_ID", "dummy_app_id_for_repro");
  const char* channelId = getenv_or("AGORA_CHANNEL_ID", "");
  const char* token = getenv_or("AGORA_TOKEN", "");
  const char* uid = getenv_or("AGORA_UID", "0");
  int joinSeconds = getenv_int("AGORA_JOIN_DURATION_SEC", 60);
  bool encryptionEnable = getenv_bool("AGORA_ENCRYPTION_ENABLE");
  const char* encModeStr = getenv_or("AGORA_ENCRYPTION_MODE", "");
  const char* encSecret = getenv_or("AGORA_ENCRYPTION_SECRET", "");
  const char* encSaltStr = getenv_or("AGORA_ENCRYPTION_SALT", "");

  fprintf(stderr, "Join duration: %d s (AGORA_JOIN_DURATION_SEC; 0=until Ctrl+C).\n", joinSeconds);

  fprintf(stderr, "0. Loading Agora SDK via dlopen...\n");
  void* handle = dlopen("libagora_rtc_sdk.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }

  auto p_agora_service_create = (AGORA_HANDLE (*)())dlsym(handle, "agora_service_create");
  auto p_agora_service_initialize = (int (*)(AGORA_HANDLE, const agora_service_config*))dlsym(handle, "agora_service_initialize");
  auto p_agora_service_set_log_file = (int (*)(AGORA_HANDLE, const char*, unsigned int))dlsym(handle, "agora_service_set_log_file");
  auto p_agora_service_set_log_filter = (int (*)(AGORA_HANDLE, unsigned int))dlsym(handle, "agora_service_set_log_filter");
  auto p_agora_service_create_local_audio_track = (AGORA_HANDLE (*)(AGORA_HANDLE))dlsym(handle, "agora_service_create_local_audio_track");
  auto p_agora_service_release = (int (*)(AGORA_HANDLE))dlsym(handle, "agora_service_release");

  auto p_agora_rtc_conn_create = (AGORA_HANDLE (*)(AGORA_HANDLE, const rtc_conn_config*))dlsym(handle, "agora_rtc_conn_create");
  auto p_agora_rtc_conn_connect = (int (*)(AGORA_HANDLE, const char*, const char*, const char*))dlsym(handle, "agora_rtc_conn_connect");
  auto p_agora_rtc_conn_disconnect = (int (*)(AGORA_HANDLE))dlsym(handle, "agora_rtc_conn_disconnect");
  auto p_agora_rtc_conn_destroy = (int (*)(AGORA_HANDLE))dlsym(handle, "agora_rtc_conn_destroy");
  auto p_agora_rtc_conn_get_local_user = (AGORA_HANDLE (*)(AGORA_HANDLE))dlsym(handle, "agora_rtc_conn_get_local_user");
  auto p_agora_rtc_conn_enable_encryption = (int (*)(AGORA_HANDLE, int, const encryption_config*))dlsym(handle, "agora_rtc_conn_enable_encryption");

  auto p_agora_local_user_publish_audio = (int (*)(AGORA_HANDLE, AGORA_HANDLE))dlsym(handle, "agora_local_user_publish_audio");

  if (!p_agora_service_create || !p_agora_service_initialize || !p_agora_rtc_conn_create ||
      !p_agora_rtc_conn_connect || !p_agora_rtc_conn_destroy) {
    fprintf(stderr, "dlsym failed for one or more core functions.\n");
    return 1;
  }

  fprintf(stderr, "1. Creating Agora service...\n");
  AGORA_HANDLE service = p_agora_service_create();
  if (!service) {
    fprintf(stderr, "agora_service_create failed\n");
    return 1;
  }

  agora_service_config svc{};
  svc.enable_audio_processor = 1;
  svc.enable_audio_device = 0;
  svc.enable_video = 0;
  svc.context = nullptr;
  svc.app_id = appId;
  svc.area_code = 0;
  svc.channel_profile = 0; // communication
  svc.audio_scenario = 3;  // chatroom
  svc.use_string_uid = 1;
  svc.domain_limit = 0;

  fprintf(stderr, "2. Initializing service...\n");
  int ret = p_agora_service_initialize(service, &svc);
  fprintf(stderr, "agora_service_initialize returned %d\n", ret);
  if (ret != 0) {
    p_agora_service_release(service);
    return 1;
  }

  // Log file
  const char* logPath = getenv_or("AGORA_LOG_FILE", "/app/agora_sdk.log");
  if (p_agora_service_set_log_file) {
    if (p_agora_service_set_log_file(service, logPath, 2 * 1024 * 1024) == 0) {
      fprintf(stderr, "Agora SDK logs written to %s\n", logPath);
    }
  }
  if (p_agora_service_set_log_filter) {
    p_agora_service_set_log_filter(service, 0x080f); // LOG_FILTER_DEBUG
  }

  // Create connection
  fprintf(stderr, "3. Creating connection...\n");
  rtc_conn_config ccfg{};
  ccfg.auto_subscribe_audio = 1;
  ccfg.auto_subscribe_video = 0;
  ccfg.enable_audio_recording_or_playout = 0;
  ccfg.client_role_type = 1; // broadcaster
  ccfg.channel_profile = 0;  // communication

  AGORA_HANDLE conn = p_agora_rtc_conn_create(service, &ccfg);
  if (!conn) {
    fprintf(stderr, "agora_rtc_conn_create failed\n");
    p_agora_service_release(service);
    return 1;
  }

  // Encryption
  if (encryptionEnable) {
    if (!encSecret || !encSecret[0]) {
      fprintf(stderr, "AGORA_ENCRYPTION_ENABLE=1 requires AGORA_ENCRYPTION_SECRET\n");
      p_agora_rtc_conn_destroy(conn);
      p_agora_service_release(service);
      return 1;
    }
    int modeVal = parse_encryption_mode(encModeStr);
    if (modeVal < 0) {
      fprintf(stderr, "Invalid AGORA_ENCRYPTION_MODE '%s'\n", encModeStr);
      p_agora_rtc_conn_destroy(conn);
      p_agora_service_release(service);
      return 1;
    }
    encryption_config enc{};
    enc.encryption_mode = modeVal;
    enc.encryption_key = encSecret;
    memset(enc.encryption_kdf_salt, 0, sizeof(enc.encryption_kdf_salt));
    if (modeVal == 7 || modeVal == 8) {
      if (!encSaltStr || !encSaltStr[0]) {
        fprintf(stderr, "Mode %d requires AGORA_ENCRYPTION_SALT (Base64 32-byte)\n", modeVal);
        p_agora_rtc_conn_destroy(conn);
        p_agora_service_release(service);
        return 1;
      }
      int saltLen = base64_decode(encSaltStr, strlen(encSaltStr), enc.encryption_kdf_salt, 32);
      if (saltLen < 32) {
        fprintf(stderr, "AGORA_ENCRYPTION_SALT must decode to at least 32 bytes (got %d)\n", saltLen);
        p_agora_rtc_conn_destroy(conn);
        p_agora_service_release(service);
        return 1;
      }
    }
    if (!p_agora_rtc_conn_enable_encryption) {
      fprintf(stderr, "agora_rtc_conn_enable_encryption not found in SDK\n");
      p_agora_rtc_conn_destroy(conn);
      p_agora_service_release(service);
      return 1;
    }
    ret = p_agora_rtc_conn_enable_encryption(conn, 1, &enc);
    fprintf(stderr, "agora_rtc_conn_enable_encryption returned %d\n", ret);
    if (ret != 0) {
      p_agora_rtc_conn_destroy(conn);
      p_agora_service_release(service);
      return 1;
    }
  }

  // Local audio track publish
  fprintf(stderr, "4. Getting local user and publishing audio track...\n");
  if (p_agora_rtc_conn_get_local_user && p_agora_service_create_local_audio_track && p_agora_local_user_publish_audio) {
    AGORA_HANDLE local_user = p_agora_rtc_conn_get_local_user(conn);
    AGORA_HANDLE audio_track = p_agora_service_create_local_audio_track(service);
    if (local_user && audio_track) {
      ret = p_agora_local_user_publish_audio(local_user, audio_track);
      fprintf(stderr, "agora_local_user_publish_audio returned %d\n", ret);
    } else {
      fprintf(stderr, "local_user or audio_track is null, skipping publish\n");
    }
  } else {
    fprintf(stderr, "Local user or audio track APIs not found, skipping publish\n");
  }

  // Connect
  if (!channelId[0]) {
    fprintf(stderr, "No AGORA_CHANNEL_ID set; skipping connect.\n");
  } else {
    fprintf(stderr, "5. Connecting to channel '%s' uid=%s...\n", channelId, uid);
    const char* effToken = token && token[0] ? token : appId;
    ret = p_agora_rtc_conn_connect(conn, effToken, channelId, uid);
    fprintf(stderr, "agora_rtc_conn_connect returned %d\n", ret);
    if (ret == 0 && joinSeconds > 0) {
      for (int i = 0; i < joinSeconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        fprintf(stderr, "[channel] %d s remaining\n", joinSeconds - i);
      }
    } else if (ret == 0 && joinSeconds == 0) {
      fprintf(stderr, "Joined channel; waiting until container exit.\n");
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }

  fprintf(stderr, "6. Cleaning up...\n");
  if (p_agora_rtc_conn_disconnect) {
    p_agora_rtc_conn_disconnect(conn);
  }
  p_agora_rtc_conn_destroy(conn);
  p_agora_service_release(service);
  return 0;
}

