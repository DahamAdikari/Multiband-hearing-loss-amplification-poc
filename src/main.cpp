// D:\2. Projects\hearing_phase4_poc\src\main.cpp
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static inline float clampf(float x, float lo, float hi) {
  return std::max(lo, std::min(hi, x));
}
static inline float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }
static inline float lin_to_db(float lin) {
  return 20.0f * std::log10(std::max(lin, 1e-12f));
}

// -------------------- WAV (16-bit PCM mono) --------------------
struct Wav {
  int sampleRate = 48000;
  std::vector<float> x; // [-1,1]
};

static uint32_t read_u32(std::ifstream &f) {
  uint32_t v;
  f.read(reinterpret_cast<char *>(&v), 4);
  return v;
}
static uint16_t read_u16(std::ifstream &f) {
  uint16_t v;
  f.read(reinterpret_cast<char *>(&v), 2);
  return v;
}

static bool read_wav_mono16(const std::string &path, Wav &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;

  char riff[4];
  f.read(riff, 4);
  (void)read_u32(f); // riffSize
  char wave[4];
  f.read(wave, 4);

  if (std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0)
    return false;

  uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0;
  uint32_t dataSize = 0;
  std::streampos dataPos = 0;

  while (f && !dataPos) {
    char id[4];
    f.read(id, 4);
    uint32_t sz = read_u32(f);
    if (!f)
      break;

    if (std::strncmp(id, "fmt ", 4) == 0) {
      audioFormat = read_u16(f);
      numChannels = read_u16(f);
      sampleRate = read_u32(f);
      (void)read_u32(f); // byteRate
      (void)read_u16(f); // blockAlign
      bitsPerSample = read_u16(f);
      if (sz > 16)
        f.seekg(sz - 16, std::ios::cur);
    } else if (std::strncmp(id, "data", 4) == 0) {
      dataSize = sz;
      dataPos = f.tellg();
      f.seekg(sz, std::ios::cur);
    } else {
      f.seekg(sz, std::ios::cur);
    }
  }

  if (!dataPos)
    return false;
  if (audioFormat != 1)
    return false; // PCM
  if (numChannels != 1)
    return false; // mono
  if (bitsPerSample != 16)
    return false; // 16-bit

  out.sampleRate = (int)sampleRate;

  f.clear();
  f.seekg(dataPos);
  size_t nSamples = dataSize / 2;
  out.x.resize(nSamples);
  for (size_t i = 0; i < nSamples; i++) {
    int16_t s16 = 0;
    f.read(reinterpret_cast<char *>(&s16), 2);
    out.x[i] = (float)s16 / 32768.0f;
  }
  return true;
}

static bool write_wav_mono16(const std::string &path,
                             const std::vector<float> &x, int sampleRate) {
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return false;

  uint32_t dataSize = (uint32_t)(x.size() * 2);
  uint32_t riffSize = 36 + dataSize;

  f.write("RIFF", 4);
  f.write(reinterpret_cast<const char *>(&riffSize), 4);
  f.write("WAVE", 4);

  f.write("fmt ", 4);
  uint32_t fmtSize = 16;
  f.write(reinterpret_cast<const char *>(&fmtSize), 4);
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint32_t sr = (uint32_t)sampleRate;
  uint16_t bitsPerSample = 16;
  uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  uint32_t byteRate = sr * blockAlign;

  f.write(reinterpret_cast<const char *>(&audioFormat), 2);
  f.write(reinterpret_cast<const char *>(&numChannels), 2);
  f.write(reinterpret_cast<const char *>(&sr), 4);
  f.write(reinterpret_cast<const char *>(&byteRate), 4);
  f.write(reinterpret_cast<const char *>(&blockAlign), 2);
  f.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

  f.write("data", 4);
  f.write(reinterpret_cast<const char *>(&dataSize), 4);

  for (float s : x) {
    s = clampf(s, -1.0f, 1.0f);
    int16_t v = (int16_t)std::lrintf(s * 32767.0f);
    f.write(reinterpret_cast<const char *>(&v), 2);
  }
  return true;
}

// -------------------- Biquad (Butterworth LP/HP) --------------------
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  void reset() { z1 = z2 = 0; }

  inline float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void setLowpass(float fs, float fc, float Q) {
    fc = clampf(fc, 10.0f, fs * 0.45f);
    Q = std::max(Q, 0.1f);
    constexpr float PI = 3.14159265358979323846f;

    float w0 = 2.0f * PI * (fc / fs);
    float cosw0 = std::cos(w0);
    float sinw0 = std::sin(w0);
    float alpha = sinw0 / (2.0f * Q);

    float b0n = (1 - cosw0) * 0.5f;
    float b1n = (1 - cosw0);
    float b2n = (1 - cosw0) * 0.5f;
    float a0n = 1 + alpha;
    float a1n = -2 * cosw0;
    float a2n = 1 - alpha;

    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }

  void setHighpass(float fs, float fc, float Q) {
    fc = clampf(fc, 10.0f, fs * 0.45f);
    Q = std::max(Q, 0.1f);
    constexpr float PI = 3.14159265358979323846f;

    float w0 = 2.0f * PI * (fc / fs);
    float cosw0 = std::cos(w0);
    float sinw0 = std::sin(w0);
    float alpha = sinw0 / (2.0f * Q);

    float b0n = (1 + cosw0) * 0.5f;
    float b1n = -(1 + cosw0);
    float b2n = (1 + cosw0) * 0.5f;
    float a0n = 1 + alpha;
    float a1n = -2 * cosw0;
    float a2n = 1 - alpha;

    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }
};

// Linkwitz-Riley 4th order = two cascaded Butterworth 2nd order
struct LR4 {
  Biquad s1, s2;
  void reset() {
    s1.reset();
    s2.reset();
  }
  inline float process(float x) { return s2.process(s1.process(x)); }
};

struct Crossover6 {
  float fs = 48000.0f;
  float e[5] = {500, 1000, 2000, 4000, 8000};

  LR4 lp[5], hp[5];

  void init(float sampleRate, const float edges[5]) {
    fs = sampleRate;
    for (int i = 0; i < 5; i++)
      e[i] = edges[i];

    float Q = 0.70710678f; // Butterworth
    for (int i = 0; i < 5; i++) {
      lp[i].s1.setLowpass(fs, e[i], Q);
      lp[i].s2.setLowpass(fs, e[i], Q);
      hp[i].s1.setHighpass(fs, e[i], Q);
      hp[i].s2.setHighpass(fs, e[i], Q);
      lp[i].reset();
      hp[i].reset();
    }
  }

  inline void split(float x, float b[6]) {
    float low1 = lp[0].process(x);
    float high1 = hp[0].process(x);

    float low2 = lp[1].process(high1);
    float high2 = hp[1].process(high1);

    float low3 = lp[2].process(high2);
    float high3 = hp[2].process(high2);

    float low4 = lp[3].process(high3);
    float high4 = hp[3].process(high3);

    float low5 = lp[4].process(high4);
    float high5 = hp[4].process(high4);

    b[0] = low1;
    b[1] = low2;
    b[2] = low3;
    b[3] = low4;
    b[4] = low5;
    b[5] = high5;
  }
};

// -------------------- Compressor --------------------
struct Compressor {
  float fs = 48000.0f;
  float thresholdDb = -25.0f;
  float ratio = 4.0f;
  float attackMs = 20.0f;
  float releaseMs = 250.0f;

  float env = 0.0f;

  void init(float sampleRate) {
    fs = sampleRate;
    env = 0.0f;
  }

  inline float process(float x) {
    float ax = std::fabs(x);

    float attackCoef = std::exp(-1.0f / (fs * (attackMs * 0.001f)));
    float releaseCoef = std::exp(-1.0f / (fs * (releaseMs * 0.001f)));

    if (ax > env)
      env = attackCoef * env + (1 - attackCoef) * ax;
    else
      env = releaseCoef * env + (1 - releaseCoef) * ax;

    float envDb = lin_to_db(env);

    float gainDb = 0.0f;
    if (envDb > thresholdDb) {
      float over = envDb - thresholdDb;
      float compressed = over / ratio;
      float outDb = thresholdDb + compressed;
      gainDb = outDb - envDb;
    }
    return x * db_to_lin(gainDb);
  }
};

// -------------------- Limiter --------------------
struct SoftLimiter {
  float thr = 0.95f;
  float strength = 10.0f;
  inline float process(float x) const {
    float ax = std::fabs(x);
    if (ax <= thr)
      return x;
    float s = (x >= 0) ? 1.0f : -1.0f;
    float ex = ax - thr;
    float y = thr + ex / (1.0f + strength * ex);
    return s * y;
  }
};

// -------------------- Audiogram loss -> makeup gain --------------------
static void loss_to_makeup_db(const float loss6[6], float makeup6[6]) {
  for (int i = 0; i < 6; i++) {
    float g = 0.5f * loss6[i]; // half-gain rule
    makeup6[i] = clampf(g, 0.0f, 25.0f);
  }
}

static bool parse_csv6(const std::string &s, float out6[6]) {
  std::string tmp = s;
  for (char &c : tmp)
    if (c == ';')
      c = ',';
  std::vector<float> vals;
  size_t start = 0;
  while (start < tmp.size()) {
    size_t end = tmp.find(',', start);
    if (end == std::string::npos)
      end = tmp.size();
    std::string token = tmp.substr(start, end - start);
    if (!token.empty())
      vals.push_back(std::stof(token));
    start = end + 1;
  }
  if (vals.size() != 6)
    return false;
  for (int i = 0; i < 6; i++)
    out6[i] = vals[i];
  return true;
}

// --------- Export-only band writer (one input WAV ->
// results/<stem>_multibands/*.wav) ----------
static bool export_bands_only(
    const Wav &wav, const std::string &inPath,
    const std::string
        &mode, // static|compress|broadband (broadband treated like "on" stage
               // on fullband; for bands use static/compress)
    const float makeupDb[6], Compressor comp[6], const std::string &outRoot,
    const std::string &stage // "in" or "on"
) {
  namespace fs = std::filesystem;
  fs::path inP(inPath);
  std::string stem = inP.stem().string();

  fs::path outDir = fs::path(outRoot) / (stem + "_multibands");
  fs::create_directories(outDir);

  float edges[5] = {500, 1000, 2000, 4000, 8000};
  Crossover6 xo;
  xo.init((float)wav.sampleRate, edges);

  std::vector<float> band[6];
  for (int i = 0; i < 6; i++)
    band[i].resize(wav.x.size());

  for (size_t n = 0; n < wav.x.size(); n++) {
    float b[6];
    xo.split(wav.x[n], b);

    for (int i = 0; i < 6; i++) {
      float v = b[i]; // raw split

      if (stage == "on") {
        float gi = db_to_lin(makeupDb[i]);
        if (mode == "static") {
          v = v * gi;
        } else if (mode == "compress") {
          v = comp[i].process(v) * gi;
        } else {
          // broadband doesn't make sense per-band; keep raw
          v = v * 1.0f;
        }
      }

      band[i][n] = v;
    }
  }

  for (int i = 0; i < 6; i++) {
    fs::path outFile = outDir / (stem + "_band" + std::to_string(i + 1) + "_" +
                                 stage + ".wav");
    if (!write_wav_mono16(outFile.string(), band[i], wav.sampleRate)) {
      std::cerr << "Error: could not write " << outFile.string() << "\n";
      return false;
    }
  }

  std::cout << "Exported 6 bands to: " << outDir.string() << "\n";
  std::cout << "Stage: " << stage
            << " (in=raw split, on=after gain/compress)\n";
  return true;
}

int main(int argc, char **argv) {
  std::string inPath;

  std::string outOffPath = "results/off.wav";
  std::string outOnPath = "results/on.wav";

  // mode:
  //   static    = multiband fixed gain
  //   compress  = multiband compressor + gain
  //   broadband = single gain for whole signal (for comparison)
  std::string mode = "compress";

  float loss6[6] = {0, 0, 0, 0, 0, 0};
  bool hasLoss = false;

  float ratio = 4.0f;
  float attackMs = 20.0f;
  float releaseMs = 250.0f;

  float thrDb[6] = {-18, -22, -26, -30, -34, -36};

  float masterDb = 0.0f;
  float wet = 1.0f;
  float dry = 1.0f;

  // export bands only
  bool exportBands = false;
  std::string exportRoot = "results";
  std::string exportStage = "in"; // in|on

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--in" && i + 1 < argc)
      inPath = argv[++i];
    else if (a == "--out_off" && i + 1 < argc)
      outOffPath = argv[++i];
    else if (a == "--out_on" && i + 1 < argc)
      outOnPath = argv[++i];
    else if (a == "--loss" && i + 1 < argc) {
      hasLoss = parse_csv6(argv[++i], loss6);
      if (!hasLoss) {
        std::cerr << "Bad --loss format.\n";
        return 1;
      }
    } else if (a == "--mode" && i + 1 < argc) {
      mode = argv[++i];
      if (mode != "compress" && mode != "static" && mode != "broadband") {
        std::cerr << "Bad --mode. Use compress, static, or broadband.\n";
        return 1;
      }
    } else if (a == "--ratio" && i + 1 < argc)
      ratio = std::stof(argv[++i]);
    else if (a == "--attack_ms" && i + 1 < argc)
      attackMs = std::stof(argv[++i]);
    else if (a == "--release_ms" && i + 1 < argc)
      releaseMs = std::stof(argv[++i]);
    else if (a == "--thr" && i + 1 < argc) {
      float t6[6];
      if (!parse_csv6(argv[++i], t6)) {
        std::cerr << "Bad --thr format.\n";
        return 1;
      }
      for (int k = 0; k < 6; k++)
        thrDb[k] = t6[k];
    } else if (a == "--master" && i + 1 < argc)
      masterDb = std::stof(argv[++i]);
    else if (a == "--wet" && i + 1 < argc)
      wet = std::stof(argv[++i]);
    else if (a == "--dry" && i + 1 < argc)
      dry = std::stof(argv[++i]);

    // export
    else if (a == "--export_bands")
      exportBands = true;
    else if (a == "--export_root" && i + 1 < argc)
      exportRoot = argv[++i];
    else if (a == "--stage" && i + 1 < argc) {
      exportStage = argv[++i];
      if (exportStage != "in" && exportStage != "on") {
        std::cerr << "Bad --stage. Use in or on.\n";
        return 1;
      }
    } else if (a == "--help") {
      std::cout
          << "Usage (normal):\n"
             "  hearing_phase4_poc --in <input.wav> [--loss "
             "\"10,15,25,35,45,50\"]\n"
             "                   [--mode compress|static|broadband]\n"
             "                   [--out_off off.wav] [--out_on on.wav]\n"
             "                   [--ratio 4] [--attack_ms 20] [--release_ms "
             "250]\n"
             "                   [--thr \"-18,-22,-26,-30,-34,-36\"] [--master "
             "0]\n"
             "                   [--dry 1] [--wet 1]\n\n"
             "Usage (export bands only):\n"
             "  hearing_phase4_poc --in <input.wav> --export_bands [--stage "
             "in|on]\n"
             "                   [--export_root results] [--mode "
             "compress|static] [--loss \"...\"]\n\n"
             "Bands:\n"
             "  B1:0-500, B2:500-1k, B3:1k-2k, B4:2k-4k, B5:4k-8k, B6:8k+\n";
      return 0;
    }
  }

  if (inPath.empty()) {
    std::cerr << "Error: --in <input.wav> required\n";
    return 1;
  }

  Wav wav;
  if (!read_wav_mono16(inPath, wav)) {
    std::cerr << "Error: could not read WAV (mono 16-bit PCM only): " << inPath
              << "\n";
    return 1;
  }

  const float fs = (float)wav.sampleRate;

  // per-band makeup gains
  float makeupDb[6] = {0, 0, 0, 0, 0, 0};
  if (hasLoss)
    loss_to_makeup_db(loss6, makeupDb);

  // broadband gain from average loss (half-gain, capped)
  float broadbandGainDb = 0.0f;
  if (hasLoss) {
    float sumLoss = 0.0f;
    for (int i = 0; i < 6; i++)
      sumLoss += loss6[i];
    float avgLoss = sumLoss / 6.0f;
    broadbandGainDb = clampf(0.5f * avgLoss, 0.0f, 25.0f);
  }
  float broadbandGainLin = db_to_lin(broadbandGainDb);

  // compressors per band (used only for multiband compress)
  Compressor comp[6];
  for (int i = 0; i < 6; i++) {
    comp[i].init(fs);
    comp[i].ratio = std::max(1.0f, ratio);
    comp[i].attackMs = std::max(1.0f, attackMs);
    comp[i].releaseMs = std::max(10.0f, releaseMs);
    comp[i].thresholdDb = thrDb[i];
  }

  // Export-only and exit
  if (exportBands) {
    if (!export_bands_only(wav, inPath, mode, makeupDb, comp, exportRoot,
                           exportStage))
      return 1;
    return 0;
  }

  // Normal pipeline
  float edges[5] = {500, 1000, 2000, 4000, 8000};
  Crossover6 xo;
  xo.init(fs, edges);

  SoftLimiter lim;
  float masterLin = db_to_lin(masterDb);
  wet = clampf(wet, 0.0f, 1.5f);
  dry = clampf(dry, 0.0f, 1.0f);

  std::vector<float> yOff(wav.x.size());
  std::vector<float> yOn(wav.x.size());

  for (size_t n = 0; n < wav.x.size(); n++) {
    float x = wav.x[n];

    // OFF = just master + limiter (baseline)
    yOff[n] = lim.process(x * masterLin);

    // ON depends on mode
    if (mode == "broadband") {
      float enhanced = x * broadbandGainLin;
      float mixed = dry * x + wet * enhanced;
      yOn[n] = lim.process(mixed * masterLin);
      continue;
    }

    // multiband (static / compress)
    float b[6];
    xo.split(x, b);

    float sumOn = 0.0f;
    for (int i = 0; i < 6; i++) {
      float bi = b[i];
      float gi = db_to_lin(makeupDb[i]);

      float oi;
      if (mode == "static") {
        oi = bi * gi;
      } else { // compress
        float ci = comp[i].process(bi);
        oi = ci * gi;
      }
      sumOn += oi;
    }

    float mixed = dry * x + wet * sumOn;
    yOn[n] = lim.process(mixed * masterLin);
  }

  if (!write_wav_mono16(outOffPath, yOff, wav.sampleRate)) {
    std::cerr << "Error: could not write " << outOffPath << "\n";
    return 1;
  }
  if (!write_wav_mono16(outOnPath, yOn, wav.sampleRate)) {
    std::cerr << "Error: could not write " << outOnPath << "\n";
    return 1;
  }

  std::cout << "Done.\n";
  std::cout << "Mode:       " << mode << "\n";
  std::cout << "Input:      " << inPath << "\n";
  std::cout << "Output OFF: " << outOffPath << "\n";
  std::cout << "Output ON:  " << outOnPath << "\n";
  std::cout << "Bands:      B1:0-500, B2:500-1k, B3:1k-2k, B4:2k-4k, B5:4k-8k, "
               "B6:8k+\n";
  if (mode == "broadband") {
    std::cout << "Broadband gain (dB): " << broadbandGainDb << "\n";
  }
  return 0;
}

// -------------------- FFI Wrapper for Dart/Android --------------------
extern "C" __attribute__((visibility("default"))) __attribute__((used)) int
process_audio_file_ffi(const char *inPath, const char *outPath,
                       const float *loss6, float ratio, float attackMs,
                       float releaseMs, const float *thrDb, float masterDb,
                       float wet, float dry) {
  if (!inPath || !outPath || !loss6 || !thrDb) {
    return 1; // Null pointer passed
  }

  Wav wav;
  if (!read_wav_mono16(std::string(inPath), wav)) {
    std::cerr << "Error: could not read WAV: " << inPath << "\n";
    return 2; // Read error
  }

  const float fs = (float)wav.sampleRate;

  // per-band makeup gains
  float makeupDb[6] = {0, 0, 0, 0, 0, 0};
  loss_to_makeup_db(loss6, makeupDb);

  // compressors per band
  Compressor comp[6];
  for (int i = 0; i < 6; i++) {
    comp[i].init(fs);
    comp[i].ratio = std::max(1.0f, ratio);
    comp[i].attackMs = std::max(1.0f, attackMs);
    comp[i].releaseMs = std::max(10.0f, releaseMs);
    comp[i].thresholdDb = thrDb[i];
  }

  float edges[5] = {500, 1000, 2000, 4000, 8000};
  Crossover6 xo;
  xo.init(fs, edges);

  SoftLimiter lim;
  float masterLin = db_to_lin(masterDb);

  // clamp wet/dry
  wet = std::max(0.0f, std::min(1.5f, wet));
  dry = std::max(0.0f, std::min(1.0f, dry));

  std::vector<float> yOn(wav.x.size());

  for (size_t n = 0; n < wav.x.size(); n++) {
    float x = wav.x[n];

    // multiband
    float b[6];
    xo.split(x, b);

    float sumOn = 0.0f;
    for (int i = 0; i < 6; i++) {
      float bi = b[i];
      float gi = db_to_lin(makeupDb[i]);

      float ci = comp[i].process(bi);
      float oi = ci * gi;
      sumOn += oi;
    }

    float mixed = dry * x + wet * sumOn;
    yOn[n] = lim.process(mixed * masterLin);
  }

  if (!write_wav_mono16(std::string(outPath), yOn, wav.sampleRate)) {
    return 3; // Write error
  }

  return 0; // Success
}