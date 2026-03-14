#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <portaudio.h>

static inline float clampf(float x, float lo, float hi) {
    return std::max(lo, std::min(hi, x));
}
static inline float db_to_lin(float db) {
    return std::pow(10.0f, db / 20.0f);
}
static inline float lin_to_db(float lin) {
    return 20.0f * std::log10(std::max(lin, 1e-12f));
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
    inline float process(float x) {
        return s2.process(s1.process(x));
    }
};

struct Crossover6 {
    float fs = 48000.0f;
    float e[5] = {500, 1000, 2000, 4000, 8000};

    LR4 lp[5], hp[5];

    void init(float sampleRate, const float edges[5]) {
        fs = sampleRate;
        for (int i = 0; i < 5; i++) e[i] = edges[i];

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
        float low1  = lp[0].process(x);
        float high1 = hp[0].process(x);

        float low2  = lp[1].process(high1);
        float high2 = hp[1].process(high1);

        float low3  = lp[2].process(high2);
        float high3 = hp[2].process(high2);

        float low4  = lp[3].process(high3);
        float high4 = hp[3].process(high3);

        float low5  = lp[4].process(high4);
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

        float attackCoef  = std::exp(-1.0f / (fs * (attackMs * 0.001f)));
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
        if (ax <= thr) return x;
        float s = (x >= 0) ? 1.0f : -1.0f;
        float ex = ax - thr;
        float y = thr + ex / (1.0f + strength * ex);
        return s * y;
    }
};

// -------------------- Hearing loss -> gain --------------------
static void loss_to_makeup_db(const float loss6[6], float makeup6[6]) {
    for (int i = 0; i < 6; i++) {
        float g = 0.5f * loss6[i];
        makeup6[i] = clampf(g, 0.0f, 25.0f);
    }
}

static bool parse_csv6(const std::string& s, float out6[6]) {
    std::string tmp = s;
    for (char& c : tmp) if (c == ';') c = ',';

    std::vector<float> vals;
    size_t start = 0;
    while (start < tmp.size()) {
        size_t end = tmp.find(',', start);
        if (end == std::string::npos) end = tmp.size();
        std::string token = tmp.substr(start, end - start);
        if (!token.empty()) vals.push_back(std::stof(token));
        start = end + 1;
    }

    if (vals.size() != 6) return false;
    for (int i = 0; i < 6; i++) out6[i] = vals[i];
    return true;
}

// -------------------- Real-time DSP State --------------------
struct RtProcessor {
    float fs = 48000.0f;

    float loss6[6]     = {0,0,0,0,0,0};
    float makeupDb[6]  = {0,0,0,0,0,0};
    float makeupLin[6] = {1,1,1,1,1,1};

    float ratio = 4.0f;
    float attackMs = 20.0f;
    float releaseMs = 250.0f;
    float thrDb[6] = {-18, -22, -26, -30, -34, -36};

    float masterDb = 0.0f;
    float wet = 1.0f;
    float dry = 0.0f; // for real-time, fully processed is usually easier to hear
    float masterLin = 1.0f;

    Crossover6 xo;
    Compressor comp[6];
    SoftLimiter lim;

    void init(float sampleRate) {
        fs = sampleRate;

        float edges[5] = {500, 1000, 2000, 4000, 8000};
        xo.init(fs, edges);

        loss_to_makeup_db(loss6, makeupDb);
        for (int i = 0; i < 6; i++) {
            makeupLin[i] = db_to_lin(makeupDb[i]);
            comp[i].init(fs);
            comp[i].ratio = std::max(1.0f, ratio);
            comp[i].attackMs = std::max(1.0f, attackMs);
            comp[i].releaseMs = std::max(10.0f, releaseMs);
            comp[i].thresholdDb = thrDb[i];
        }

        wet = clampf(wet, 0.0f, 1.5f);
        dry = clampf(dry, 0.0f, 1.0f);
        masterLin = db_to_lin(masterDb);
    }

    inline float process_sample(float x) {
        float b[6];
        xo.split(x, b);

        float sumOn = 0.0f;
        for (int i = 0; i < 6; i++) {
            float bi = b[i];
            float ci = comp[i].process(bi);
            float oi = ci * makeupLin[i];
            sumOn += oi;
        }

        float mixed = dry * x + wet * sumOn;
        return lim.process(mixed * masterLin);
    }
};

// -------------------- PortAudio callback --------------------
static int audio_callback(const void* inputBuffer,
                          void* outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo*,
                          PaStreamCallbackFlags,
                          void* userData) {
    auto* proc = reinterpret_cast<RtProcessor*>(userData);

    const float* in = reinterpret_cast<const float*>(inputBuffer);
    float* out = reinterpret_cast<float*>(outputBuffer);

    if (!out) return paAbort;

    for (unsigned long i = 0; i < framesPerBuffer; i++) {
        float x = 0.0f;
        if (in) x = in[i];

        float y = proc->process_sample(x);
        out[i] = y;
    }

    return paContinue;
}

int main(int argc, char** argv) {
    // Default settings
    float loss6[6] = {0, 5, 15, 30, 40, 50};
    float ratio = 4.0f;
    float attackMs = 20.0f;
    float releaseMs = 250.0f;
    float thrDb[6] = {-18, -22, -26, -30, -34, -36};
    float masterDb = 0.0f;
    float wet = 1.0f;
    float dry = 0.0f;
    float sampleRate = 48000.0f;
    unsigned long framesPerBuffer = 256;

    // Parse simple CLI args
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--loss" && i + 1 < argc) {
            if (!parse_csv6(argv[++i], loss6)) {
                std::cerr << "Bad --loss format\n";
                return 1;
            }
        } else if (a == "--ratio" && i + 1 < argc) {
            ratio = std::stof(argv[++i]);
        } else if (a == "--attack_ms" && i + 1 < argc) {
            attackMs = std::stof(argv[++i]);
        } else if (a == "--release_ms" && i + 1 < argc) {
            releaseMs = std::stof(argv[++i]);
        } else if (a == "--thr" && i + 1 < argc) {
            if (!parse_csv6(argv[++i], thrDb)) {
                std::cerr << "Bad --thr format\n";
                return 1;
            }
        } else if (a == "--master" && i + 1 < argc) {
            masterDb = std::stof(argv[++i]);
        } else if (a == "--wet" && i + 1 < argc) {
            wet = std::stof(argv[++i]);
        } else if (a == "--dry" && i + 1 < argc) {
            dry = std::stof(argv[++i]);
        } else if (a == "--sr" && i + 1 < argc) {
            sampleRate = std::stof(argv[++i]);
        } else if (a == "--fpb" && i + 1 < argc) {
            framesPerBuffer = static_cast<unsigned long>(std::stoul(argv[++i]));
        } else if (a == "--help") {
            std::cout
                << "Usage:\n"
                << "  realtime_multiband_stream "
                << "[--loss \"0,5,15,30,40,50\"] "
                << "[--ratio 4] [--attack_ms 20] [--release_ms 250]\n"
                << "  [--thr \"-18,-22,-26,-30,-34,-36\"] "
                << "[--master 0] [--wet 1] [--dry 0] [--sr 48000] [--fpb 256]\n";
            return 0;
        }
    }

    RtProcessor proc;
    for (int i = 0; i < 6; i++) {
        proc.loss6[i] = loss6[i];
        proc.thrDb[i] = thrDb[i];
    }
    proc.ratio = ratio;
    proc.attackMs = attackMs;
    proc.releaseMs = releaseMs;
    proc.masterDb = masterDb;
    proc.wet = wet;
    proc.dry = dry;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
        return 1;
    }

    proc.init(sampleRate);

    PaStream* stream = nullptr;
    err = Pa_OpenDefaultStream(
        &stream,
        1,              // input channels
        1,              // output channels
        paFloat32,      // format
        sampleRate,
        framesPerBuffer,
        audio_callback,
        &proc
    );

    if (err != paNoError) {
        std::cerr << "Open stream failed: " << Pa_GetErrorText(err) << "\n";
        Pa_Terminate();
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "Start stream failed: " << Pa_GetErrorText(err) << "\n";
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    std::cout << "Real-time multiband stream started.\n";
    std::cout << "Losses: ";
    for (int i = 0; i < 6; i++) {
        std::cout << loss6[i] << (i < 5 ? ", " : "");
    }
    std::cout << "\nPress ENTER to stop...\n";

    std::cin.get();
    std::cin.get();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    return 0;
}