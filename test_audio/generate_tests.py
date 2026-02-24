import wave, struct, math, random

SR = 48000

def write_wav(path, x, sr=SR):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)  # 16-bit PCM
        w.setframerate(sr)
        for s in x:
            s = max(-1.0, min(1.0, s))
            w.writeframes(struct.pack("<h", int(s * 32767)))

def sweep(duration=12.0, f0=100.0, f1=10000.0, amp=0.6):
    n = int(duration * SR)
    x = []
    # log sweep
    K = math.log(f1/f0)
    for i in range(n):
        t = i / SR
        ft = f0 * math.exp(K * t / duration)
        phase = 2 * math.pi * (f0 * duration / K) * (math.exp(K * t / duration) - 1)
        x.append(amp * math.sin(phase))
    return x

def band_limited_noise(n, low_hz, high_hz):
    # quick-and-dirty: sum random sines (enough for test)
    x = [0.0]*n
    bins = 40
    for b in range(bins):
        f = random.uniform(low_hz, high_hz)
        ph = random.uniform(0, 2*math.pi)
        a = 1.0/bins
        for i in range(n):
            x[i] += a*math.sin(2*math.pi*f*i/SR + ph)
    return x

def speech_like(duration=10.0):
    n = int(duration * SR)
    x = [0.0]*n

    # “vowel-ish” part: 120 Hz fundamental + 3 formants
    f0 = 120.0
    formants = [700.0, 1200.0, 2600.0]
    for i in range(n):
        t = i/SR
        glottal = math.sin(2*math.pi*f0*t)
        v = 0.35*glottal
        for k,f in enumerate(formants):
            v += (0.15/(k+1))*math.sin(2*math.pi*f*t)
        x[i] = v

    # add “consonant bursts” (high-freq noise) at intervals
    burst = band_limited_noise(n, 3000, 9000)
    for i in range(n):
        t = i/SR
        gate = 0.0
        # bursts every ~1s, 120ms long
        if int(t) != int(t-0.12) and (t % 1.0) < 0.12:
            gate = 1.0
        x[i] += 0.25 * gate * burst[i]

    # normalize a bit
    m = max(abs(s) for s in x)
    return [0.8*s/m for s in x]

def add_noise(x, snr_db=5.0):
    # add white noise to target SNR
    p_sig = sum(s*s for s in x)/len(x)
    p_noise = p_sig / (10**(snr_db/10))
    noise = [random.gauss(0, math.sqrt(p_noise)) for _ in x]
    y = [x[i] + noise[i] for i in range(len(x))]
    # normalize
    m = max(abs(s) for s in y)
    return [0.9*s/m for s in y]

if __name__ == "__main__":
    write_wav("sweep_100_10k.wav", sweep())
    sl = speech_like()
    write_wav("speechlike.wav", sl)
    write_wav("speechlike_noise.wav", add_noise(sl, snr_db=0.0))
    print("Generated: sweep_100_10k.wav, speechlike.wav, speechlike_noise.wav")