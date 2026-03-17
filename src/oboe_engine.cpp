#include <android/log.h>
#include <mutex>
#include <memory>
#include <oboe/Oboe.h>

// We need to pull in RtProcessor from realtime_multiband_stream.cpp or similar
// For this standalone, we'll redefine the minimum needed based on the poc code.
#include <fstream>
#include <vector>

#define LOGI(...)                                                              \
  __android_log_print(ANDROID_LOG_INFO, "ClearToneEngine", __VA_ARGS__)
#define LOGE(...)                                                              \
  __android_log_print(ANDROID_LOG_ERROR, "ClearToneEngine", __VA_ARGS__)

#include <algorithm>
#include <cmath>
#include <cstdint>

static inline float clampf(float x, float lo, float hi) {
  return std::max(lo, std::min(hi, x));
}
static inline float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }
static inline float lin_to_db(float lin) {
  return 20.0f * std::log10(std::max(lin, 1e-12f));
}

// -------------------- Stable TPT State Variable Filter --------------------
struct SVF {
  float g, k, a1, a2, a3;
  float s1 = 0, s2 = 0;
  void reset() { s1 = s2 = 0; }
  void setLowpass(float fs, float fc, float Q) {
    float wd = 2.0f * 3.14159265f * fc;
    float T = 1.0f / fs;
    float wa = (2.0f / T) * std::tan(wd * T / 2.0f);
    g = wa * T / 2.0f;
    k = 1.0f / Q;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  // This SVF calculates Low, High, and Band pass simultaneously
  inline void process(float x, float& lp, float& hp, float& bp) {
    float v3 = x - s2;
    float v1 = a1 * s1 + a2 * v3;
    float v2 = s2 + a2 * s1 + a3 * v3;
    s1 = 2.0f * v1 - s1;
    s2 = 2.0f * v2 - s2;
    lp = v2;
    hp = x - k * v1 - v2;
    bp = v1;
    // Protection
    if (!std::isfinite(s1) || !std::isfinite(s2)) reset();
  }
};

struct ParallelCrossover6 {
  SVF filters[5];
  void init(float sampleRate, const float edges[5]) {
    for (int i = 0; i < 5; i++) {
        filters[i].setLowpass(sampleRate, edges[i], 0.7071f);
        filters[i].reset();
    }
  }
  void reset() {
    for (int i = 0; i < 5; i++) filters[i].reset();
  }
  inline void split(float x, float b[6]) {
    float lp[5], hp[5], bp[5];
    for (int i = 0; i < 5; i++) {
        filters[i].process(x, lp[i], hp[i], bp[i]);
    }
    // Band splitting:
    b[0] = lp[0];              // < 500
    b[1] = lp[1] - lp[0];      // 500 - 1000
    b[2] = lp[2] - lp[1];      // 1000 - 2000
    b[3] = lp[3] - lp[2];      // 2000 - 4000
    b[4] = lp[4] - lp[3];      // 4000 - 8000
    b[5] = x - lp[4];          // > 8000
  }
};

struct Compressor {
  float fs = 48000.0f;
  float thresholdDb = -25.0f;
  float ratio = 4.0f;
  float attackMs = 20.0f;
  float releaseMs = 250.0f;
  float env = 0.0f;
  float aC = 0.0f;
  float rC = 0.0f;

  void init(float sampleRate) {
    fs = sampleRate > 0 ? sampleRate : 48000.0f;
    env = 0.0f;
    updateConstants();
  }

  void updateConstants() {
    aC = std::exp(-1.0f / (fs * (attackMs * 0.001f)));
    rC = std::exp(-1.0f / (fs * (releaseMs * 0.001f)));
  }

  inline float process(float x) {
    if (std::isnan(x) || std::isinf(x)) return 0.0f;
    float ax = std::fabs(x);
    env = (ax > env) ? (aC * env + (1.0f - aC) * ax) : (rC * env + (1.0f - rC) * ax);
    float eDb = lin_to_db(env);
    float gainDb = (eDb > thresholdDb)
                       ? (thresholdDb + (eDb - thresholdDb) / ratio - eDb)
                       : 0.0f;
    return x * db_to_lin(gainDb);
  }
};

struct SoftLimiter {
  float thr = 0.95f;
  float strength = 10.0f;
  inline float process(float x) const {
    if (!std::isfinite(x)) return 0.0f;
    float ax = std::fabs(x);
    if (ax <= thr)
      return x;
    float s = (x >= 0.0f) ? 1.0f : -1.0f;
    float ex = ax - thr;
    // Limit expansion to prevent NaN in extreme cases
    return s * (thr + ex / (1.0f + strength * ex));
  }
};

class RealtimeProcessor {
public:
  ParallelCrossover6 xo;
  Compressor comp[6];
  SoftLimiter lim;
  float makeupLin[6] = {1, 1, 1, 1, 1, 1};
  float masterLin = 1.0f;
  float wet = 1.0f;
  float dry = 0.0f; // Default 0 for realtime processing
  bool bypass = false; // Emergency debug bypass
  std::mutex paramMutex;

  void init(float sampleRate) {
    std::lock_guard<std::mutex> lock(paramMutex);
    if (sampleRate <= 4000.0f) sampleRate = 48000.0f; // Prevent weird fs
    float edges[5] = {500, 1000, 2000, 4000, 8000};
    xo.init(sampleRate, edges);
    xo.reset(); // Crucial: clear memory
    for (int i = 0; i < 6; i++) {
      comp[i].init(sampleRate);
      comp[i].ratio = 4.0f;
      comp[i].attackMs = 20.0f;
      comp[i].releaseMs = 250.0f;
      float defaultThr[6] = {-18, -22, -26, -30, -34, -36};
      comp[i].thresholdDb = defaultThr[i];
      comp[i].updateConstants();
      comp[i].env = 0.0f; // Clear state
      makeupLin[i] = 1.0f;
    }
    masterLin = 1.0f;
  }

  void updateLoss(const float loss6[6]) {
    std::lock_guard<std::mutex> lock(paramMutex);
    for (int i = 0; i < 6; i++) {
      float g = 0.5f * loss6[i];
      float makeupDb = clampf(g, 0.0f, 25.0f);
      makeupLin[i] = db_to_lin(makeupDb);
    }
  }

  void resetAll() {
    // If called from a place that already holds the lock, this would deadlock.
    // Use a try_lock or internal_reset. 
    xo.reset();
    for (int i = 0; i < 6; i++) {
        comp[i].env = 0.0f;
    }
    lim = SoftLimiter(); // Reset limiter state if any
  }

  inline float processSample(float x, bool& outError) {
    if (bypass) return x;
    
    float b[6];
    xo.split(x, b);
    float sumOn = 0.0f;

    for (int i = 0; i < 6; i++) {
      float bandOut = comp[i].process(b[i]);
      // Denormal protection before gain
      if (std::fabs(bandOut) < 1e-12f) bandOut = 0.0f;
      sumOn += bandOut * makeupLin[i];
    }
    
    float mixed = (dry * x) + (wet * sumOn);
    float out = lim.process(mixed * masterLin);
    
    if (!std::isfinite(out)) {
      outError = true;
      return 0.0f; 
    }
    return out;
  }
};

class OboeEngine : public oboe::AudioStreamDataCallback,
                   public oboe::AudioStreamErrorCallback {
public:
  RealtimeProcessor processor;
  std::shared_ptr<oboe::AudioStream> recordingStream;
  std::shared_ptr<oboe::AudioStream> playingStream;
  oboe::Usage mUsage = oboe::Usage::VoiceCommunication;

  std::vector<float> captureBuffer;
  std::vector<float> captureBufferOut;
  std::vector<float> outDataBuffer;
  bool isCapturing = false;
  std::mutex captureMutex;

  bool start(int inputDeviceId) {
    if (playingStream)
      return true; // Already running

    processor.init(48000.0f); // Default

    // 1. Open Output Stream FIRST
    oboe::AudioStreamBuilder outBuilder;
    outBuilder.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared) // Use Shared to avoid competition
        ->setUsage(mUsage)
        ->setContentType(mUsage == oboe::Usage::VoiceCommunication 
                         ? oboe::ContentType::Speech 
                         : oboe::ContentType::Music)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(1)
        ->setErrorCallback(this);

    oboe::Result result = outBuilder.openStream(playingStream);
    if (result != oboe::Result::OK) {
      LOGE("Failed to open playing stream: %s", oboe::convertToText(result));
      return false;
    }

    int32_t sampleRate = playingStream->getSampleRate();
    LOGI("Output stream opened: rate=%d, channels=%d", sampleRate, playingStream->getChannelCount());
    processor.init((float)sampleRate);
    outDataBuffer.resize(2048); // Pre-allocate safe buffer size

    // 2. Open Input Stream
    oboe::AudioStreamBuilder inBuilder;
    inBuilder.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setUsage(mUsage)
        ->setInputPreset(oboe::InputPreset::VoiceCommunication)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(1)
        ->setDeviceId(inputDeviceId)
        ->setSampleRate(sampleRate)
        ->setDataCallback(this) // Input stream will trigger callbacks
        ->setErrorCallback(this);

    result = inBuilder.openStream(recordingStream);
    if (result != oboe::Result::OK) {
      LOGE("Failed to open recording stream: %s", oboe::convertToText(result));
      playingStream->close();
      playingStream.reset();
      return false;
    }

    result = playingStream->requestStart();
    if (result != oboe::Result::OK) {
      LOGE("Failed to start playing stream: %s", oboe::convertToText(result));
      return false;
    }

    result = recordingStream->requestStart();
    if (result != oboe::Result::OK) {
      LOGE("Failed to start recording stream: %s", oboe::convertToText(result));
      return false;
    }
    return true;
  }

  void stop() {
    if (recordingStream) {
      recordingStream->requestStop();
      recordingStream->close();
      recordingStream.reset();
    }
    if (playingStream) {
      playingStream->requestStop();
      playingStream->close();
      playingStream.reset();
    }
  }

  void startCapture() {
    std::lock_guard<std::mutex> lock{captureMutex};
    captureBuffer.clear();
    captureBufferOut.clear();
    captureBuffer.reserve(48000 * 5);
    captureBufferOut.reserve(48000 * 5);
    isCapturing = true;
    LOGI("Capture started (Dual)");
  }

  void stopCapture() {
    std::lock_guard<std::mutex> lock{captureMutex};
    isCapturing = false;
    LOGI("Capture stopped. Samples: %zu", captureBuffer.size());
  }

  int saveCapture(const char *filePath, int source) {
    std::lock_guard<std::mutex> lock{captureMutex};
    std::ofstream outFile(filePath, std::ios::binary);
    if (!outFile) {
      LOGE("Failed to open file for saving capture: %s", filePath);
      return 1;
    }
    const std::vector<float> &targetBuffer = (source == 0) ? captureBuffer : captureBufferOut;
    outFile.write(reinterpret_cast<const char *>(targetBuffer.data()),
                  targetBuffer.size() * sizeof(float));
    outFile.close();
    LOGI("Capture (%s) saved to %s", (source == 0 ? "input" : "output"), filePath);
    return 0;
  }

  int getCaptureSize() {
    std::lock_guard<std::mutex> lock{captureMutex};
    return static_cast<int>(captureBuffer.size());
  }

  void updateLoss(const float loss[6]) { processor.updateLoss(loss); }

  void setUsage(int usage) {
    mUsage = static_cast<oboe::Usage>(usage);
    LOGI("Audio Usage set to: %d", usage);
  }

  // Input Callback
  oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream,
                                        void *audioData,
                                        int32_t numFrames) override {
    if (!playingStream)
      return oboe::DataCallbackResult::Stop;

    float *inData = static_cast<float *>(audioData);
    if (outDataBuffer.size() < (size_t)numFrames) {
        outDataBuffer.resize(numFrames);
    }
    float* outData = outDataBuffer.data();

    int failsafeCount = 0;
    float peakIn = 0.0f, peakOut = 0.0f;
    
    // Lock once per block for performance and stability
    {
      std::lock_guard<std::mutex> lock(processor.paramMutex);
      for (int i = 0; i < numFrames; ++i) {
        bool error = false;
        outData[i] = processor.processSample(inData[i], error);
        if (error) failsafeCount++;
        
        float absIn = std::fabs(inData[i]);
        float absOut = std::fabs(outData[i]);
        if (absIn > peakIn) peakIn = absIn;
        if (absOut > peakOut) peakOut = absOut;
      }
    }

    // Emergency Reset if filters exploded
    if (failsafeCount > 0) {
      processor.resetAll();
    }

    static int logThrottle = 0;
    if (logThrottle++ % 100 == 0) {
      LOGI("onAudioReady: frames=%d, peakIn=%f, peakOut=%f, errors=%d", 
            numFrames, peakIn, peakOut, failsafeCount);
    }

    // Capture logic
    if (isCapturing) {
      std::lock_guard<std::mutex> lock{captureMutex};
      captureBuffer.insert(captureBuffer.end(), inData, inData + numFrames);
      captureBufferOut.insert(captureBufferOut.end(), outData, outData + numFrames);
      // Limit to 10 seconds to avoid OOM
      if (captureBuffer.size() > 48000 * 10) {
        isCapturing = false;
        LOGI("Capture limit reached (10s)");
      }
    }

    // Write processed data to output stream with a small timeout (10ms)
    // This helps synchronize and avoids dropping frames if output is slightly behind.
    auto result = playingStream->write(outData, numFrames, 10 * 1000 * 1000); 
    if (!result) {
       LOGE("Playing stream write error: %s", oboe::convertToText(result.error()));
       if (result.error() == oboe::Result::ErrorDisconnected || 
           result.error() == oboe::Result::ErrorInvalidState) {
         LOGI("Stopping stream due to fatal write error");
         return oboe::DataCallbackResult::Stop;
       }
    } else if (result.value() != numFrames) {
       // LOGW("Playing stream write partial: %d/%d", result.value(), numFrames);
    }

    return oboe::DataCallbackResult::Continue;
  }

  bool onError(oboe::AudioStream *audioStream, oboe::Result error) override {
    LOGE("Audio stream error: %s", oboe::convertToText(error));
    // When ErrorDisconnected occurs, the stream is already dead.
    // The UI will detect this via is_playing_ffi() or the next write error.
    return false; 
  }

  bool isPlaying() {
    return (playingStream && playingStream->getState() == oboe::StreamState::Started) &&
           (recordingStream && recordingStream->getState() == oboe::StreamState::Started);
  }
};

// Global instance
OboeEngine gEngine;

// --- FFI EXPORTS ---
extern "C" {

__attribute__((visibility("default"))) __attribute__((used)) int
start_rt_stream_ffi(int inputDeviceId) {
  bool ok = gEngine.start(inputDeviceId);
  return ok ? 0 : 1;
}

__attribute__((visibility("default"))) __attribute__((used)) int
stop_rt_stream_ffi() {
  gEngine.stop();
  return 0;
}

__attribute__((visibility("default"))) __attribute__((used)) int
update_rt_params_ffi(const float *loss6) {
  if (!loss6)
    return 1;
  gEngine.updateLoss(loss6);
  return 0;
}

__attribute__((visibility("default"))) __attribute__((used)) void
debug_start_capture_ffi() {
  gEngine.startCapture();
}

__attribute__((visibility("default"))) __attribute__((used)) void
debug_stop_capture_ffi() {
  gEngine.stopCapture();
}

__attribute__((visibility("default"))) __attribute__((used)) int
debug_save_capture_ffi(const char *filePath, int source) {
  if (!filePath)
    return 1;
  return gEngine.saveCapture(filePath, source);
}

__attribute__((visibility("default"))) __attribute__((used)) int
debug_get_capture_size_ffi() {
  return gEngine.getCaptureSize();
}

__attribute__((visibility("default"))) __attribute__((used)) void
set_audio_usage_ffi(int usage) {
  gEngine.setUsage(usage);
}

__attribute__((visibility("default"))) __attribute__((used)) bool
is_playing_ffi() {
  return gEngine.isPlaying();
}

} // extern "C"
