/**
 * Agora Server Gateway repro: join channel, optional receive/send audio and video.
 *
 * Env vars:
 *   AGORA_APP_ID, AGORA_CHANNEL_ID, AGORA_TOKEN (optional), AGORA_UID (optional)
 *   AGORA_USE_STRING_UID=1  - use string user account for join; set AGORA_UID to your account string (not only digits)
 *   AGORA_REGISTER_AUDIO_OBSERVER=0|1 - register playback audio frame observer (default 1)
 *   AGORA_ENABLE_AUDIO_VOLUME_INDICATION=0|1 - enable SDK audio volume indication callback (default 1)
 *   AGORA_RECEIVE_VIDEO=1  - subscribe to and process remote video
 *   AGORA_SEND_AUDIO=1    - publish local audio (generated PCM, e.g. 440 Hz tone)
 *   AGORA_SEND_VIDEO=1    - publish local video (generated image, 720p)
 *   AGORA_JOIN_DURATION_SEC=N - stay in channel N seconds; 0 = until Ctrl+C (default 60)
 *   AGORA_REPRO_STOP_AFTER=init | create_local_audio_track | connect | publish  - stop after that step (for bisect; connect/publish require AGORA_CHANNEL_ID)
 *   AGORA_THREAD_PRIORITY=0|1|2|3|4|5  - set deprecated threadPriority (0=LOWEST 2=NORMAL 5=CRITICAL); if SDK honors it, may avoid RT
 *   AGORA_ENCRYPTION_ENABLE=0|1  - enable built-in media encryption (0=off, 1=on)
 *   AGORA_ENCRYPTION_MODE  - number 1-8 or name: AES-128-XTS(1), AES-256-XTS(3), AES-128-ECB(2), SM4-128-ECB(4), AES-128-GCM(5), AES-256-GCM(6), AES-128-GCM2(7), AES-256-GCM2(8)
 *   AGORA_ENCRYPTION_SECRET  - encryption key (required when encryption enabled)
 *   AGORA_ENCRYPTION_SALT  - Base64-encoded 32-byte salt (required for AES-*-GCM2 modes)
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
#include <cmath>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dlfcn.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "IAgoraService.h"
#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraRtcConnection.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraVideoTrack.h"
#include "AgoraMediaBase.h"
#include "AgoraBase.h"

static volatile bool g_exit = false;
static void sig_handler(int sig) {
  const char* name = (sig == SIGABRT) ? "SIGABRT" : (sig == SIGSEGV) ? "SIGSEGV" :
                     (sig == SIGINT) ? "SIGINT" : nullptr;
  if (name)
    fprintf(stderr, "Signal %d (%s) caught.\n", sig, name);
  else
    fprintf(stderr, "Signal %d caught.\n", sig);
  if (sig == SIGABRT || sig == SIGSEGV)
    _exit(128 + sig);
  g_exit = true;
}

static const char* getenv_or(const char* name, const char* def) {
  const char* v = getenv(name);
  return (v && v[0]) ? v : def;
}

/* Return env value with trailing whitespace and inline # comment stripped (for .env lines like KEY=val  # comment). */
static std::string getenv_trimmed_or(const char* name, const char* def) {
  const char* v = getenv(name);
  if (!v || !v[0]) return def ? std::string(def) : std::string();
  std::string s(v);
  size_t len = s.size();
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) --len;
  size_t hash = s.find('#');
  if (hash != std::string::npos) {
    if (hash < len) len = hash;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) --len;
  }
  s.resize(len);
  return s;
}

static bool getenv_bool(const char* name) {
  const char* v = getenv(name);
  if (!v || !v[0]) return false;
  return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

/* Base64 decode; writes at most maxOut bytes to out, returns number decoded. */
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

/* Parse AGORA_ENCRYPTION_MODE to agora::rtc::ENCRYPTION_MODE. Accepts numeric 1-8 or name (e.g. AES-128-GCM2). Returns -1 if unknown. */
static int parse_encryption_mode(const char* mode) {
  if (!mode || !mode[0]) return -1;
  char* end = nullptr;
  long n = strtol(mode, &end, 10);
  /* Accept number with optional trailing whitespace/comment (e.g. "7    # comment" from .env) */
  if (end != mode && n >= 1 && n <= 8) {
    while (*end == ' ' || *end == '\t') ++end;
    if (*end == '\0' || *end == '#') return (int)n;
  }
  std::string m(mode);
  /* Strip trailing space and inline # comment for name comparison */
  size_t len = m.size();
  while (len > 0 && (m[len - 1] == ' ' || m[len - 1] == '\t')) --len;
  size_t hash = m.find('#');
  if (hash != std::string::npos) { if (hash < len) len = hash; while (len > 0 && (m[len - 1] == ' ' || m[len - 1] == '\t')) --len; }
  m.resize(len);
  if (m.empty()) return -1;
  for (auto& ch : m) { if (ch == '-') ch = '_'; else if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); }
  if (m == "AES_128_XTS") return (int)agora::rtc::AES_128_XTS;
  if (m == "AES_256_XTS") return (int)agora::rtc::AES_256_XTS;
  if (m == "AES_128_ECB") return (int)agora::rtc::AES_128_ECB;
  if (m == "SM4_128_ECB") return (int)agora::rtc::SM4_128_ECB;
  if (m == "AES_128_GCM") return (int)agora::rtc::AES_128_GCM;
  if (m == "AES_256_GCM") return (int)agora::rtc::AES_256_GCM;
  if (m == "AES_128_GCM2") return (int)agora::rtc::AES_128_GCM2;
  if (m == "AES_256_GCM2") return (int)agora::rtc::AES_256_GCM2;
  return -1;
}

static bool encryption_mode_needs_salt(int mode) {
  return mode == (int)agora::rtc::AES_128_GCM2 || mode == (int)agora::rtc::AES_256_GCM2;
}


/* Plain-text Agora SDK log writer so logs are readable (setLogFile writes binary). */
struct FileLogWriter : public agora::commons::ILogWriter {
  FILE* fp_ = nullptr;
  explicit FileLogWriter(FILE* fp) : fp_(fp) {}
  int32_t writeLog(agora::commons::LOG_LEVEL level, const char* message, uint16_t length) override {
    if (!fp_ || !message) return -1;
    const char* tag = "?";
    switch (level) {
      case agora::commons::LOG_LEVEL::LOG_LEVEL_DEBUG:    tag = "D"; break;
      case agora::commons::LOG_LEVEL::LOG_LEVEL_INFO:    tag = "I"; break;
      case agora::commons::LOG_LEVEL::LOG_LEVEL_WARN:    tag = "W"; break;
      case agora::commons::LOG_LEVEL::LOG_LEVEL_ERROR:   tag = "E"; break;
      case agora::commons::LOG_LEVEL::LOG_LEVEL_FATAL:   tag = "F"; break;
      case agora::commons::LOG_LEVEL::LOG_LEVEL_API_CALL: tag = "A"; break;
      default: break;
    }
    if (length > 0) {
      size_t n = (size_t)length;
      if (n > 4096) n = 4096;
      fprintf(fp_, "[%s] ", tag);
      fwrite(message, 1, n, fp_);
      if (message[n - 1] != '\n') fputc('\n', fp_);
      fflush(fp_);
    }
    return 0;
  }
};

/** Parse env as integer; defaultVal if unset/invalid. 0 = run until Ctrl+C. Max 86400. */
static int getenv_join_duration_sec(int defaultVal) {
  const char* v = getenv("AGORA_JOIN_DURATION_SEC");
  if (!v || !v[0]) return defaultVal;
  char* end = nullptr;
  long n = strtol(v, &end, 10);
  if (end == v || n < 0) return defaultVal;
  if (n > 86400) n = 86400;
  return (int)n;
}

/* ---- Audio: receive playback (mixed remote) ---- */
class PlaybackAudioObserver : public agora::media::IAudioFrameObserverBase {
 public:
  std::atomic<uint64_t> framesReceived_{0};
  bool onRecordAudioFrame(const char*, AudioFrame&) override { return true; }
  bool onPlaybackAudioFrame(const char*, AudioFrame& frame) override {
    uint64_t n = ++framesReceived_;
    if (frame.buffer && frame.samplesPerChannel > 0 && n % 100 == 0)
      fprintf(stderr, "[audio] received remote frame %llu (SDK callback, raw): %d samples, %d ch, %d Hz\n",
              (unsigned long long)n, frame.samplesPerChannel, frame.channels, frame.samplesPerSec);
    return true;
  }
  bool onMixedAudioFrame(const char*, AudioFrame&) override { return true; }
  bool onEarMonitoringAudioFrame(AudioFrame&) override { return true; }
  int getObservedAudioFramePosition() override {
    return (int)AUDIO_FRAME_POSITION_PLAYBACK;
  }
  AudioParams getPlaybackAudioParams() override {
    return AudioParams(16000, 1, agora::rtc::RAW_AUDIO_FRAME_OP_MODE_READ_ONLY, 160);
  }
  AudioParams getRecordAudioParams() override { return AudioParams(); }
  AudioParams getMixedAudioParams() override { return AudioParams(); }
  AudioParams getEarMonitoringAudioParams() override { return AudioParams(); }
};

/* ---- Video: receive remote video frames ---- */
class PlaybackVideoObserver : public agora::rtc::IVideoFrameObserver2 {
 public:
  std::atomic<uint64_t> framesReceived_{0};
  void onFrame(const char* channelId, agora::user_id_t remoteUid,
               const agora::media::base::VideoFrame* frame) override {
    (void)channelId;
    (void)remoteUid;
    uint64_t n = ++framesReceived_;
    if (frame && n % 30 == 0)
      fprintf(stderr, "[video] received remote frame %llu (SDK callback, raw): %dx%d\n",
              (unsigned long long)n, frame->width, frame->height);
  }
};

/* ---- Local user observer: register audio/video observers when remote tracks subscribed ---- */
class MinimalLocalUserObserver : public agora::rtc::ILocalUserObserver {
 public:
  MinimalLocalUserObserver(agora::rtc::ILocalUser* user,
                           agora::media::IAudioFrameObserverBase* audioObs,
                           agora::rtc::IVideoFrameObserver2* videoObs,
                           bool enableAudioObserver,
                           bool audioObserverAlreadyRegistered)
      : local_user_(user), audio_observer_(audioObs), video_observer_(videoObs),
        enable_audio_observer_(enableAudioObserver),
        audio_observer_registered_(audioObserverAlreadyRegistered),
        cleaned_up_(false) {
    local_user_->registerLocalUserObserver(this);
  }
  ~MinimalLocalUserObserver() override { teardown(); }

  void teardown() {
    if (cleaned_up_) return;
    cleaned_up_ = true;
    if (local_user_) {
      if (audio_observer_ && audio_observer_registered_) local_user_->unregisterAudioFrameObserver(audio_observer_);
      if (video_observer_) local_user_->unregisterVideoFrameObserver(video_observer_);
      local_user_->unregisterLocalUserObserver(this);
    }
    local_user_ = nullptr;
  }

  void onUserAudioTrackSubscribed(agora::user_id_t userId,
                                  agora::agora_refptr<agora::rtc::IRemoteAudioTrack> audioTrack) override {
    (void)userId;
    (void)audioTrack;
    if (enable_audio_observer_ && local_user_ && audio_observer_ && !audio_observer_registered_) {
      local_user_->registerAudioFrameObserver(audio_observer_);
      audio_observer_registered_ = true;
    }
    fprintf(stderr, "Subscribed to remote audio (user %s).\n", userId);
  }

  void onUserVideoTrackSubscribed(agora::user_id_t userId, const agora::rtc::VideoTrackInfo& trackInfo,
                                  agora::agora_refptr<agora::rtc::IRemoteVideoTrack> videoTrack) override {
    (void)trackInfo;
    (void)videoTrack;
    if (local_user_ && video_observer_)
      local_user_->registerVideoFrameObserver(video_observer_);
    fprintf(stderr, "Subscribed to remote video (user %s).\n", userId);
  }

  void onAudioTrackPublishStart(agora::agora_refptr<agora::rtc::ILocalAudioTrack>) override {}
  void onAudioTrackPublishSuccess(agora::agora_refptr<agora::rtc::ILocalAudioTrack>) override {}
  void onAudioTrackUnpublished(agora::agora_refptr<agora::rtc::ILocalAudioTrack>) override {}
  void onAudioTrackPublicationFailure(agora::agora_refptr<agora::rtc::ILocalAudioTrack>, agora::ERROR_CODE_TYPE) override {}
  void onLocalAudioTrackStatistics(const agora::rtc::LocalAudioStats&) override {}
  void onRemoteAudioTrackStatistics(agora::agora_refptr<agora::rtc::IRemoteAudioTrack>, const agora::rtc::RemoteAudioTrackStats&) override {}
  void onUserAudioTrackStateChanged(agora::user_id_t, agora::agora_refptr<agora::rtc::IRemoteAudioTrack>,
                                    agora::rtc::REMOTE_AUDIO_STATE, agora::rtc::REMOTE_AUDIO_STATE_REASON, int) override {}
  void onVideoTrackPublishStart(agora::agora_refptr<agora::rtc::ILocalVideoTrack>) override {}
  void onVideoTrackPublishSuccess(agora::agora_refptr<agora::rtc::ILocalVideoTrack>) override {}
  void onVideoTrackPublicationFailure(agora::agora_refptr<agora::rtc::ILocalVideoTrack>, agora::ERROR_CODE_TYPE) override {}
  void onVideoTrackUnpublished(agora::agora_refptr<agora::rtc::ILocalVideoTrack>) override {}
  void onLocalVideoTrackStateChanged(agora::agora_refptr<agora::rtc::ILocalVideoTrack>, agora::rtc::LOCAL_VIDEO_STREAM_STATE, agora::rtc::LOCAL_VIDEO_STREAM_REASON) override {}
  void onLocalVideoTrackStatistics(agora::agora_refptr<agora::rtc::ILocalVideoTrack>, const agora::rtc::LocalVideoTrackStats&) override {}
  void onUserVideoTrackStateChanged(agora::user_id_t, agora::agora_refptr<agora::rtc::IRemoteVideoTrack>,
                                    agora::rtc::REMOTE_VIDEO_STATE, agora::rtc::REMOTE_VIDEO_STATE_REASON, int) override {}
  void onFirstRemoteVideoFrameRendered(agora::user_id_t, int, int, int) override {}
  void onRemoteVideoTrackStatistics(agora::agora_refptr<agora::rtc::IRemoteVideoTrack>, const agora::rtc::RemoteVideoTrackStats&) override {}
  void onAudioVolumeIndication(const agora::rtc::AudioVolumeInformation* speakers, unsigned int speakerNumber, int totalVolume) override {
    static std::atomic<uint64_t> cbCount{0};
    uint64_t n = ++cbCount;
    if (n % 10 == 0) {
      const char* topUid = (speakerNumber > 0 && speakers && speakers[0].userId) ? speakers[0].userId : "-";
      unsigned int topVol = (speakerNumber > 0 && speakers) ? speakers[0].volume : 0;
      fprintf(stderr, "[audio-volume] callbacks=%llu speakers=%u total=%d top_uid=%s top_vol=%u\n",
              (unsigned long long)n, speakerNumber, totalVolume, topUid, topVol);
    }
  }
  void onActiveSpeaker(agora::user_id_t) override {}
  void onAudioSubscribeStateChanged(const char*, agora::user_id_t, agora::rtc::STREAM_SUBSCRIBE_STATE, agora::rtc::STREAM_SUBSCRIBE_STATE, int) override {}
  void onVideoSubscribeStateChanged(const char*, agora::user_id_t, agora::rtc::STREAM_SUBSCRIBE_STATE, agora::rtc::STREAM_SUBSCRIBE_STATE, int) override {}
  void onAudioPublishStateChanged(const char*, agora::rtc::STREAM_PUBLISH_STATE, agora::rtc::STREAM_PUBLISH_STATE, int) override {}
  void onVideoPublishStateChanged(const char*, agora::rtc::STREAM_PUBLISH_STATE, agora::rtc::STREAM_PUBLISH_STATE, int) override {}
  void onFirstRemoteAudioFrame(agora::user_id_t, int) override {}
  void onFirstRemoteAudioDecoded(agora::user_id_t, int) override {}
  void onFirstRemoteVideoFrame(agora::user_id_t, int, int, int) override {}
  void onFirstRemoteVideoDecoded(agora::user_id_t, int, int, int) override {}
  void onVideoSizeChanged(agora::user_id_t, int, int, int) override {}

 private:
  agora::rtc::ILocalUser* local_user_;
  agora::media::IAudioFrameObserverBase* audio_observer_;
  agora::rtc::IVideoFrameObserver2* video_observer_;
  bool enable_audio_observer_;
  bool audio_observer_registered_;
  bool cleaned_up_;
};

/* ---- Send audio thread: push 10 ms PCM (440 Hz tone) at 16 kHz mono ---- */
static void send_audio_loop(agora::agora_refptr<agora::rtc::IAudioPcmDataSender> sender) {
  const uint32_t sampleRate = 16000;
  const size_t samplesPer10ms = sampleRate / 100;
  const size_t numCh = 1;
  const double freqHz = 440.0;
  const double amplitude = 0.3;
  int16_t buf[160];
  uint32_t ts = 0;
  uint64_t sent = 0;
  while (!g_exit) {
    for (size_t i = 0; i < samplesPer10ms; ++i) {
      double t = (ts * 0.001) + (double)i / sampleRate;
      double s = amplitude * std::sin(2.0 * M_PI * freqHz * t);
      buf[i] = (int16_t)(s * 32767);
    }
    if (sender->sendAudioPcmData(buf, ts, 0, samplesPer10ms,
                                 agora::rtc::TWO_BYTES_PER_SAMPLE, numCh, sampleRate) < 0)
      break;
    ++sent;
    if (sent % 100 == 0)
      fprintf(stderr, "[audio] sent chunk #%llu ts=%u (160 samples 16kHz mono)\n",
              (unsigned long long)sent, ts);
    ts += 10;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/* ---- Send video thread: push I420 frame with drawn pattern (720p) at 15 fps ---- */
static void send_video_loop(agora::agora_refptr<agora::rtc::IVideoFrameSender> sender,
                            int width, int height, int fps) {
  const int ySize = width * height;
  const int uvSize = (width / 2) * (height / 2);
  std::vector<uint8_t> buffer(ySize + uvSize * 2);
  uint8_t* y = buffer.data();
  uint8_t* u = y + ySize;
  uint8_t* v = u + uvSize;

  /* Background: dark grey */
  memset(y, 90, (size_t)ySize);
  memset(u, 128, (size_t)uvSize);
  memset(v, 128, (size_t)uvSize);

  /* Center "badge" rectangle: cyan/teal (Y bright, U and V for cyan) */
  const int bw = width * 3 / 4, bh = height * 2 / 5;
  const int bx = (width - bw) / 2, by = (height - bh) / 2;
  for (int py = by; py < by + bh && py < height; ++py) {
    for (int px = bx; px < bx + bw && px < width; ++px) {
      y[py * width + px] = 200;
      int uidx = (py / 2) * (width / 2) + (px / 2);
      u[uidx] = 80;
      v[uidx] = 180;
    }
  }

  /* White border around badge */
  const int border = 4;
  for (int b = 0; b < border; ++b) {
    int left = bx - b, right = bx + bw + b, top = by - b, bottom = by + bh + b;
    if (left < 0) left = 0; if (right > width) right = width;
    if (top < 0) top = 0; if (bottom > height) bottom = height;
    for (int px = left; px < right; ++px) {
      if (top >= 0) y[top * width + px] = 255;
      if (bottom < height) y[(bottom - 1) * width + px] = 255;
    }
    for (int py = top; py < bottom; ++py) {
      if (left >= 0) y[py * width + left] = 255;
      if (right < width) y[py * width + (right - 1)] = 255;
    }
  }

  /* Simple "A" in the center (outline + crossbar) */
  const int ax = width / 2 - 20, ay = height / 2 - 16;
  for (int dy = 0; dy < 32; ++dy)
    for (int dx = 0; dx < 40; ++dx) {
      int gx = ax + dx, gy = ay + dy;
      if (gx < 0 || gx >= width || gy < 0 || gy >= height) continue;
      bool on = (dx >= 4 && dx < 36 && dy >= 0 && dy < 32) &&
                (dy < 4 || dy >= 28 || (dx >= 14 && dx < 26) || dx == 4 || dx == 35);
      if (on) y[gy * width + gx] = 255;
    }

  agora::media::base::ExternalVideoFrame frame;
  frame.type = agora::media::base::ExternalVideoFrame::VIDEO_BUFFER_RAW_DATA;
  frame.format = agora::media::base::VIDEO_PIXEL_I420;
  frame.buffer = buffer.data();
  frame.stride = width;
  frame.height = height;
  frame.cropLeft = frame.cropTop = frame.cropRight = frame.cropBottom = 0;
  frame.rotation = 0;
  int intervalMs = 1000 / (fps > 0 ? fps : 15);
  uint64_t sent = 0;

  while (!g_exit) {
    frame.timestamp = (int64_t)std::chrono::steady_clock::now().time_since_epoch().count() / 1000;
    sender->sendVideoFrame(frame);
    ++sent;
    if (sent % 30 == 0)
      fprintf(stderr, "[video] sent frame #%llu %dx%d ts=%lld\n",
              (unsigned long long)sent, width, height, (long long)frame.timestamp);
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  signal(SIGABRT, sig_handler);
  signal(SIGSEGV, sig_handler);
  signal(SIGINT, sig_handler);

  std::string appId(getenv_or("AGORA_APP_ID", "dummy_app_id_for_repro"));
  std::string channelId(getenv_or("AGORA_CHANNEL_ID", ""));
  std::string token(getenv_or("AGORA_TOKEN", ""));
  std::string uid(getenv_or("AGORA_UID", "0"));
  bool useStringUid = getenv_bool("AGORA_USE_STRING_UID");
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
  bool receiveVideo = getenv_bool("AGORA_RECEIVE_VIDEO");
  bool sendAudio = getenv_bool("AGORA_SEND_AUDIO");
  bool sendVideo = getenv_bool("AGORA_SEND_VIDEO");
  int joinDurationSec = getenv_join_duration_sec(60);
  std::string stopAfter(getenv_or("AGORA_REPRO_STOP_AFTER", ""));
  bool encryptionEnable = getenv_bool("AGORA_ENCRYPTION_ENABLE");
  std::string encryptionModeStr(getenv_trimmed_or("AGORA_ENCRYPTION_MODE", ""));
  std::string encryptionSecret(getenv_trimmed_or("AGORA_ENCRYPTION_SECRET", ""));
  std::string encryptionSalt(getenv_trimmed_or("AGORA_ENCRYPTION_SALT", ""));
  fprintf(stderr, "Join duration: %d s (AGORA_JOIN_DURATION_SEC; 0=until Ctrl+C).\n", joinDurationSec);
  if (!stopAfter.empty())
    fprintf(stderr, "Bisect mode: will stop after '%s' (AGORA_REPRO_STOP_AFTER).\n", stopAfter.c_str());

  if (token.empty()) token = appId;

  // Ensure libagora_rtc_sdk is loaded via dlopen before using C++ APIs.
  fprintf(stderr, "0. Loading Agora SDK via dlopen...\n");
  {
    void* h = dlopen("libagora_rtc_sdk.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      fprintf(stderr, "dlopen libagora_rtc_sdk.so failed: %s\n", dlerror());
      return 1;
    }
  }

  /* Log scheduler limits and current thread priority (context for glibc tpp.c assertion) */
  {
    int fifo_min = sched_get_priority_min(SCHED_FIFO);
    int fifo_max = sched_get_priority_max(SCHED_FIFO);
    int policy = 0;
    struct sched_param param = {};
    (void)pthread_getschedparam(pthread_self(), &policy, &param);
    const char* pol_name = (policy == SCHED_OTHER) ? "SCHED_OTHER" :
                          (policy == SCHED_FIFO) ? "SCHED_FIFO" :
                          (policy == SCHED_RR) ? "SCHED_RR" : "?";
    fprintf(stderr, "[scheduler] fifo_min_prio=%d fifo_max_prio=%d (assert allows new_prio==-1 or [%d..%d]); current thread policy=%d (%s) priority=%d\n",
            fifo_min, fifo_max, fifo_min, fifo_max, policy, pol_name, param.sched_priority);
    struct rlimit rtp = {};
    if (getrlimit(RLIMIT_RTPRIO, &rtp) == 0)
      fprintf(stderr, "[scheduler] RLIMIT_RTPRIO cur=%lld max=%lld (0 => no RT priority allowed)\n",
              (long long)rtp.rlim_cur, (long long)rtp.rlim_max);
  }

  fprintf(stderr, "Creating Agora service...\n");
  agora::base::IAgoraService* service = createAgoraService();
  if (!service) {
    fprintf(stderr, "createAgoraService() failed\n");
    return 1;
  }

  agora::base::AgoraServiceConfiguration config;
  config.appId = appId.c_str();
  config.enableAudioProcessor = 1;
  config.enableAudioDevice = 0;
  config.enableVideo = (receiveVideo || sendVideo) ? 1 : 0;
  config.useStringUid = useStringUid ? 1 : 0;
  if (useStringUid)
    fprintf(stderr, "AGORA_USE_STRING_UID=1: string user account mode; AGORA_UID=\"%s\"\n", uid.c_str());
  else
    fprintf(stderr, "Numeric UID mode (default); AGORA_UID=\"%s\" (set AGORA_USE_STRING_UID=1 for string account)\n", uid.c_str());
  /* Optional: set deprecated threadPriority (0=LOWEST..5=CRITICAL). If SDK honors it, LOWEST/NORMAL might avoid RT. */
  const char* tp = getenv("AGORA_THREAD_PRIORITY");
  if (tp && tp[0]) {
    int v = 2;
    if (sscanf(tp, "%d", &v) == 1 && v >= 0 && v <= 5) {
      config.threadPriority = (agora::rtc::THREAD_PRIORITY_TYPE)v;
      fprintf(stderr, "AGORA_THREAD_PRIORITY=%d (0=LOWEST 2=NORMAL 5=CRITICAL)\n", v);
    }
  }
  if (!config.threadPriority.has_value())
    fprintf(stderr, "threadPriority: unset (SDK default; set AGORA_THREAD_PRIORITY=0|1|2|3|4|5 to override)\n");

  fprintf(stderr, "Calling service->initialize()...\n");
  int ret = service->initialize(config);
  if (ret != 0) {
    const char* err = (ret == -1) ? "ERR_FAILED" : (ret == -2) ? "ERR_INVALID_ARGUMENT" :
                      (ret == -3) ? "ERR_NOT_READY" : (ret == -7) ? "ERR_NOT_INITIALIZED" :
                      (ret == -21) ? "ERR_INIT_NET_ENGINE" : "?";
    fprintf(stderr, "initialize() returned %d (%s)\n", ret, err);
    service->release();
    return 1;
  }
  fprintf(stderr, "initialize() returned 0 OK.\n");

  /* Agora SDK plain-text log file (custom writer so logs are readable; setLogFile writes binary). */
  {
    std::string logPath(getenv_trimmed_or("AGORA_LOG_FILE", "/app/agora_sdk.log"));
    FILE* logFp = fopen(logPath.c_str(), "a");
    if (logFp) {
      static FileLogWriter* s_logWriter = nullptr;
      s_logWriter = new FileLogWriter(logFp);
      int32_t lr = service->setLogWriter(s_logWriter);
      if (lr == 0) {
        fprintf(stderr, "Agora SDK logs (plain text) written to %s. Set AGORA_LOG_FILE to override.\n", logPath.c_str());
        (void)service->setLogFilter((unsigned int)agora::commons::LOG_FILTER_DEBUG);
      } else {
        fprintf(stderr, "setLogWriter failed %d\n", (int)lr);
        delete s_logWriter;
        s_logWriter = nullptr;
        fclose(logFp);
      }
    } else {
      fprintf(stderr, "Could not open log file %s (errno %d). SDK logs may only go to stderr.\n", logPath.c_str(), errno);
    }
  }

  if (stopAfter == "init") {
    fprintf(stderr, "Stopping after init (AGORA_REPRO_STOP_AFTER=init). Trigger was initialize().\n");
    service->release();
    return 0;
  }

  fprintf(stderr, "Creating local audio track...\n");
  agora::agora_refptr<agora::rtc::ILocalAudioTrack> defaultAudioTrack = service->createLocalAudioTrack();
  if (!defaultAudioTrack) fprintf(stderr, "createLocalAudioTrack() returned null\n");
  else fprintf(stderr, "createLocalAudioTrack() OK\n");
  defaultAudioTrack = nullptr;
  if (stopAfter == "create_local_audio_track") {
    fprintf(stderr, "Stopping after create_local_audio_track (AGORA_REPRO_STOP_AFTER=create_local_audio_track). Trigger was createLocalAudioTrack().\n");
    service->release();
    return 0;
  }

  agora::agora_refptr<agora::rtc::IRtcConnection> connection;
  if (channelId.empty() && (stopAfter == "connect" || stopAfter == "publish")) {
    fprintf(stderr, "AGORA_REPRO_STOP_AFTER=%s requires AGORA_CHANNEL_ID (and AGORA_APP_ID) to be set.\n", stopAfter.c_str());
    service->release();
    return 1;
  }
  if (!channelId.empty()) {
    fprintf(stderr, "Connecting to channel '%s' (uid=%s)... receive_video=%d send_audio=%d send_video=%d\n",
            channelId.c_str(), uid.c_str(), receiveVideo ? 1 : 0, sendAudio ? 1 : 0, sendVideo ? 1 : 0);

    agora::rtc::RtcConnectionConfiguration ccfg;
    /* Do not force clientRoleType; keep SDK default behavior. */
    ccfg.autoSubscribeAudio = true;
    ccfg.autoSubscribeVideo = receiveVideo;
    ccfg.enableAudioRecordingOrPlayout = false;

    connection = service->createRtcConnection(ccfg);
    if (!connection) {
      fprintf(stderr, "createRtcConnection() failed\n");
      service->release();
      return 1;
    }

    agora::rtc::ILocalUser* localUser = connection->getLocalUser();
    localUser->subscribeAllAudio();
    if (receiveVideo) {
      agora::rtc::VideoSubscriptionOptions vopt;
      localUser->subscribeAllVideo(vopt);
    }

    ret = localUser->setPlaybackAudioFrameParameters(1, 16000,
                                                    agora::rtc::RAW_AUDIO_FRAME_OP_MODE_READ_ONLY, 160);
    if (ret != 0) {
      fprintf(stderr, "setPlaybackAudioFrameParameters() failed %d\n", ret);
      connection = nullptr;
      service->release();
      return 1;
    }
    if (enableAudioVolumeIndication) {
      int vir = localUser->setAudioVolumeIndicationParameters(1000, 3, false);
      if (vir == 0) fprintf(stderr, "Audio volume indication enabled (interval=1000ms smooth=3 vad=0).\n");
      else fprintf(stderr, "setAudioVolumeIndicationParameters() failed %d\n", vir);
    } else {
      fprintf(stderr, "AGORA_ENABLE_AUDIO_VOLUME_INDICATION=0: audio volume indication disabled.\n");
    }

    /* Enable built-in encryption before connect if requested. Must match other clients in channel (same mode, key, salt). */
    if (encryptionEnable) {
      if (encryptionSecret.empty()) {
        fprintf(stderr, "AGORA_ENCRYPTION_ENABLE=1 requires AGORA_ENCRYPTION_SECRET to be set.\n");
        connection = nullptr;
        service->release();
        return 1;
      }
      int modeVal = parse_encryption_mode(encryptionModeStr.c_str());
      if (modeVal < 0) {
        fprintf(stderr, "Invalid or missing AGORA_ENCRYPTION_MODE '%s'. Use number 1-8 or name (e.g. 7 or AES-128-GCM2).\n", encryptionModeStr.c_str());
        connection = nullptr;
        service->release();
        return 1;
      }
      /* Use AES_128_GCM2 (7) explicitly when mode is 7; GCM2 requires key + 32-byte salt for encode/decode to match. */
      agora::rtc::EncryptionConfig encConfig;
      encConfig.encryptionMode = (agora::rtc::ENCRYPTION_MODE)modeVal;
      encConfig.encryptionKey = encryptionSecret.c_str();
      encConfig.datastreamEncryptionEnabled = false;
      memset(encConfig.encryptionKdfSalt, 0, sizeof(encConfig.encryptionKdfSalt));
      if (encryption_mode_needs_salt(modeVal)) {
        if (encryptionSalt.empty()) {
          fprintf(stderr, "AGORA_ENCRYPTION_MODE %s (AES-*-GCM2) requires AGORA_ENCRYPTION_SALT (Base64 32-byte).\n", encryptionModeStr.c_str());
          connection = nullptr;
          service->release();
          return 1;
        }
        /* Decode Base64 salt; need exactly 32 bytes for GCM2. Accept >= 32 and use first 32 (some Base64 padding can yield 33). */
        int saltLen = base64_decode(encryptionSalt.c_str(), encryptionSalt.size(), encConfig.encryptionKdfSalt, 32);
        if (saltLen < 32) {
          fprintf(stderr, "AGORA_ENCRYPTION_SALT must decode to at least 32 bytes (got %d). Use: openssl rand -base64 32\n", saltLen);
          connection = nullptr;
          service->release();
          return 1;
        }
        /* Salt must not be all zeros per Agora doc */
        bool saltNonZero = false;
        for (int i = 0; i < 32; i++) { if (encConfig.encryptionKdfSalt[i] != 0) { saltNonZero = true; break; } }
        if (!saltNonZero) {
          fprintf(stderr, "AGORA_ENCRYPTION_SALT decoded to 32 zero bytes; use a non-zero salt for GCM2.\n");
          connection = nullptr;
          service->release();
          return 1;
        }
      }
      ret = connection->enableEncryption(true, encConfig);
      if (ret != 0) {
        fprintf(stderr, "enableEncryption() failed %d (check mode/key/salt; all clients in channel must use same config).\n", ret);
        connection = nullptr;
        service->release();
        return 1;
      }
      static const char* const kModeNames[] = {
        "?", "AES_128_XTS", "AES_128_ECB", "AES_256_XTS", "SM4_128_ECB",
        "AES_128_GCM", "AES_256_GCM", "AES_128_GCM2", "AES_256_GCM2"
      };
      const char* modeName = (modeVal >= 1 && modeVal <= 8) ? kModeNames[modeVal] : "?";
      fprintf(stderr, "Encryption enabled: mode=%s (%d), key_len=%zu, salt=%s (all clients in channel must use same mode/key/salt).\n",
              modeName, modeVal, encryptionSecret.size(), encryption_mode_needs_salt(modeVal) ? "32 bytes" : "n/a");
    }

    PlaybackAudioObserver playbackAudioObs;
    PlaybackVideoObserver playbackVideoObs;
    bool audioObserverPreRegistered = false;
    if (registerAudioObserver) {
      int aor = localUser->registerAudioFrameObserver(&playbackAudioObs);
      if (aor == 0) {
        audioObserverPreRegistered = true;
        fprintf(stderr, "Audio observer registered.\n");
      } else {
        fprintf(stderr, "registerAudioFrameObserver() failed %d; will retry after remote audio subscribe.\n", aor);
      }
    } else {
      fprintf(stderr, "AGORA_REGISTER_AUDIO_OBSERVER=0: audio observer registration disabled.\n");
    }
    MinimalLocalUserObserver userObserver(localUser, &playbackAudioObs, receiveVideo ? &playbackVideoObs : nullptr,
                                          registerAudioObserver, audioObserverPreRegistered);

    /* Optional: custom send audio track */
    agora::agora_refptr<agora::rtc::ILocalAudioTrack> sendAudioTrack;
    agora::agora_refptr<agora::rtc::IAudioPcmDataSender> pcmSender;
    std::thread sendAudioThread;
    if (sendAudio) {
      agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory = service->createMediaNodeFactory();
      if (factory) {
        pcmSender = factory->createAudioPcmDataSender();
        if (pcmSender) {
          sendAudioTrack = service->createCustomAudioTrack(pcmSender);
          if (sendAudioTrack) {
            sendAudioTrack->setEnabled(true);
            fprintf(stderr, "Send audio track created (440 Hz tone).\n");
          }
        }
      }
    }

    /* Optional: custom send video track (720p) */
    agora::agora_refptr<agora::rtc::ILocalVideoTrack> sendVideoTrack;
    agora::agora_refptr<agora::rtc::IVideoFrameSender> videoFrameSender;
    std::thread sendVideoThread;
    const int sendVideoWidth = 1280, sendVideoHeight = 720, sendVideoFps = 15;
    if (sendVideo) {
      agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory = service->createMediaNodeFactory();
      if (factory) {
        videoFrameSender = factory->createVideoFrameSender();
        if (videoFrameSender) {
          sendVideoTrack = service->createCustomVideoTrack(videoFrameSender);
          if (sendVideoTrack) {
            agora::rtc::VideoEncoderConfiguration enc;
            enc.codecType = agora::rtc::VIDEO_CODEC_H264;
            enc.dimensions.width = sendVideoWidth;
            enc.dimensions.height = sendVideoHeight;
            enc.frameRate = sendVideoFps;
            enc.bitrate = 2500 * 1000;  /* 2.5 Mbps for 720p */
            sendVideoTrack->setVideoEncoderConfiguration(enc);
            sendVideoTrack->setEnabled(true);
            fprintf(stderr, "Send video track created: 720p %dx%d @ %d fps (badge + 'A' image).\n",
                    sendVideoWidth, sendVideoHeight, sendVideoFps);
          }
        }
      }
    }

    ret = connection->connect(token.c_str(), channelId.c_str(), uid.c_str());
    if (ret != 0) {
      fprintf(stderr, "connect() failed with %d\n", ret);
      connection = nullptr;
      service->release();
      return 1;
    }
    fprintf(stderr, "Connected to channel '%s' uid=%s\n", channelId.c_str(), uid.c_str());
    if (stopAfter == "connect") {
      fprintf(stderr, "Stopping after connect (AGORA_REPRO_STOP_AFTER=connect). Trigger was connect().\n");
      userObserver.teardown();
      connection->disconnect();
      connection = nullptr;
      service->release();
      return 0;
    }

    if (sendAudio && sendAudioTrack) {
      localUser->publishAudio(sendAudioTrack);
      fprintf(stderr, "Published audio track.\n");
      sendAudioThread = std::thread(send_audio_loop, pcmSender);
    }
    if (sendVideo && sendVideoTrack) {
      localUser->publishVideo(sendVideoTrack);
      fprintf(stderr, "Published video track.\n");
      sendVideoThread = std::thread(send_video_loop, videoFrameSender, sendVideoWidth, sendVideoHeight, sendVideoFps);
    }

    if (stopAfter == "publish") {
      fprintf(stderr, "Stopping after publish (AGORA_REPRO_STOP_AFTER=publish). Letting send threads run 2s then exit.\n");
      std::this_thread::sleep_for(std::chrono::seconds(2));
      g_exit = true;
      if (sendAudioThread.joinable()) sendAudioThread.join();
      if (sendVideoThread.joinable()) sendVideoThread.join();
      if (sendAudio && sendAudioTrack) localUser->unpublishAudio(sendAudioTrack);
      if (sendVideo && sendVideoTrack) localUser->unpublishVideo(sendVideoTrack);
      userObserver.teardown();
      connection->disconnect();
      connection = nullptr;
      service->release();
      return 0;
    }

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
      if (!g_exit)
        fprintf(stderr, "Duration reached, leaving channel.\n");
    } else {
      while (!g_exit)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    g_exit = true;   /* signal send threads to stop so join() returns */
    if (sendAudioThread.joinable()) sendAudioThread.join();
    if (sendVideoThread.joinable()) sendVideoThread.join();

    if (sendAudio && sendAudioTrack) localUser->unpublishAudio(sendAudioTrack);
    if (sendVideo && sendVideoTrack) localUser->unpublishVideo(sendVideoTrack);

    fprintf(stderr, "Disconnecting. Audio frames received: %llu  Video frames received: %llu\n",
            (unsigned long long)playbackAudioObs.framesReceived_.load(),
            (unsigned long long)playbackVideoObs.framesReceived_.load());
    userObserver.teardown();
    connection->disconnect();
    connection = nullptr;
  }

  fprintf(stderr, "Releasing service...\n");
  service->release();
  fprintf(stderr, "Done.\n");
  return 0;
}
