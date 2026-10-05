#include "Analysis.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace choplab
{

namespace
{
constexpr double kAnalysisRate = 22050.0;
constexpr float kEps = 1.0e-12f;
constexpr double kTwoPi = 6.283185307179586;

std::vector<float> makeHann (int n)
{
    std::vector<float> w ((size_t) n);
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos ((float) (kTwoPi * i / n));
    return w;
}

std::vector<float> mixToMono (const juce::AudioBuffer<float>& src)
{
    const int n = src.getNumSamples();
    const int chans = juce::jmax (1, src.getNumChannels());
    std::vector<float> mono ((size_t) n, 0.0f);

    for (int c = 0; c < src.getNumChannels(); ++c)
    {
        const float* d = src.getReadPointer (c);
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += d[i];
    }

    const float scale = 1.0f / (float) chans;
    for (auto& s : mono)
        s *= scale;

    return mono;
}

std::vector<float> resampleForAnalysis (std::vector<float> in, double sourceRate, double targetRate)
{
    if (std::abs (sourceRate - targetRate) < 0.5 || in.empty())
        return in;

    if (sourceRate > targetRate)
    {
        // 6th-order low-pass so content above the new Nyquist doesn't fold back into the chroma range
        for (int stage = 0; stage < 3; ++stage)
        {
            juce::IIRFilter lp;
            lp.setCoefficients (juce::IIRCoefficients::makeLowPass (sourceRate, targetRate * 0.42, 0.707));
            lp.processSamples (in.data(), (int) in.size());
        }
    }

    const double ratio = sourceRate / targetRate;
    const int numOut = (int) std::floor ((double) in.size() / ratio);
    std::vector<float> out ((size_t) juce::jmax (0, numOut));
    in.resize (in.size() + 8, 0.0f);

    juce::LagrangeInterpolator interp;
    interp.process (ratio, in.data(), out.data(), numOut);
    return out;
}

float percentile (std::vector<float> v, float p)
{
    if (v.empty())
        return 0.0f;
    const size_t idx = (size_t) juce::jlimit (0.0, (double) v.size() - 1.0, std::round (p * (double) (v.size() - 1)));
    std::nth_element (v.begin(), v.begin() + (ptrdiff_t) idx, v.end());
    return v[idx];
}

float sampleAt (const std::vector<float>& v, double pos)
{
    if (pos < 0.0 || v.empty())
        return 0.0f;
    const auto i = (size_t) pos;
    if (i + 1 >= v.size())
        return i < v.size() ? v[i] : 0.0f;
    const float frac = (float) (pos - (double) i);
    return v[i] + frac * (v[i + 1] - v[i]);
}

//==============================================================================
void computeOnsetFeatures (AnalysisFeatures& f)
{
    constexpr int order = 10;
    constexpr int N = 1 << order;
    const int hop = f.hop;
    const auto& x = f.mono;
    const int numFrames = (int) x.size() / hop + 1;

    f.fps = f.analysisRate / hop;
    f.onsetEnv.assign ((size_t) numFrames, 0.0f);
    f.lowEnv.assign ((size_t) numFrames, 0.0f);
    f.highEnv.assign ((size_t) numFrames, 0.0f);
    f.frameDb.assign ((size_t) numFrames, -120.0f);

    f.midEnv.assign ((size_t) numFrames, 0.0f);

    // Mel-spaced bands, so a kick's few low bins count as much as a snare's hundreds of high ones.
    constexpr int numBands = 40;
    auto toMel = [] (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); };
    const double melLo = toMel (30.0), melHi = toMel (f.analysisRate * 0.5);
    std::vector<int> bandOf ((size_t) N / 2 + 1, -1);
    std::vector<int> bandCount (numBands, 0);
    std::vector<double> bandCentre (numBands, 0.0);
    for (int k = 1; k <= N / 2; ++k)
    {
        const double hz = k * f.analysisRate / N;
        const int b = (int) std::floor ((toMel (hz) - melLo) / (melHi - melLo) * numBands);
        if (b >= 0 && b < numBands)
        {
            bandOf[(size_t) k] = b;
            ++bandCount[(size_t) b];
            bandCentre[(size_t) b] += hz;
        }
    }
    for (int b = 0; b < numBands; ++b)
        if (bandCount[(size_t) b] > 0)
            bandCentre[(size_t) b] /= bandCount[(size_t) b];

    juce::dsp::FFT fft (order);
    const auto window = makeHann (N);
    std::vector<float> buf ((size_t) N * 2);
    std::vector<float> prev (numBands, 0.0f), cur (numBands, 0.0f);
    const float magScale = 4.0f / (float) N;

    for (int t = 0; t < numFrames; ++t)
    {
        const int start = t * hop - N / 2; // frame t is centred on sample t * hop
        double energy = 0.0;

        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const int idx = start + i;
            if (idx >= 0 && idx < (int) x.size())
            {
                const float s = x[(size_t) idx];
                energy += (double) s * s;
                buf[(size_t) i] = s * window[(size_t) i];
            }
        }

        f.frameDb[(size_t) t] = (float) (10.0 * std::log10 (energy / N + 1.0e-12));

        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        std::fill (cur.begin(), cur.end(), 0.0f);
        for (int k = 1; k <= N / 2; ++k)
            if (bandOf[(size_t) k] >= 0)
                cur[(size_t) bandOf[(size_t) k]] += buf[(size_t) k] * magScale;
        for (int b = 0; b < numBands; ++b)
            cur[(size_t) b] = bandCount[(size_t) b] > 0 ? std::log1p (100.0f * cur[(size_t) b] / (float) bandCount[(size_t) b]) : 0.0f;

        // SuperFlux-style: compare against the max of neighbouring bands in the previous frame so
        // vibrato and slides don't read as new onsets. Frame 0 is compared against silence, so a
        // hit right at the start of the sample still counts.
        float full = 0, low = 0, mid = 0, high = 0;
        for (int b = 0; b < numBands; ++b)
        {
            if (bandCount[(size_t) b] == 0)
                continue;
            float ref = prev[(size_t) b];
            if (b > 0) ref = std::max (ref, prev[(size_t) b - 1]);
            if (b + 1 < numBands) ref = std::max (ref, prev[(size_t) b + 1]);
            const float d = cur[(size_t) b] - ref;
            if (d <= 0.0f)
                continue;
            full += d;
            if (bandCentre[(size_t) b] < 150.0)
                low += d;
            else if (bandCentre[(size_t) b] > 5000.0)
                high += d;
            else
                mid += d;
        }
        f.onsetEnv[(size_t) t] = full;
        f.lowEnv[(size_t) t] = low;
        f.midEnv[(size_t) t] = mid;
        f.highEnv[(size_t) t] = high;

        std::swap (prev, cur);
    }
}

//==============================================================================
struct Peak
{
    float midi;
    float amp;
};

void computeHarmonicFeatures (AnalysisFeatures& f)
{
    constexpr int order = 12;
    constexpr int N = 1 << order;
    const int hop = f.chromaHop;
    const auto& x = f.mono;
    const int numFrames = (int) x.size() / hop + 1;
    const int maxBin = (int) std::ceil (5000.0 * N / f.analysisRate);
    const float magScale = 4.0f / (float) N;

    f.chromaFps = f.analysisRate / hop;

    juce::dsp::FFT fft (order);
    const auto window = makeHann (N);
    std::vector<float> buf ((size_t) N * 2);
    std::vector<std::vector<float>> mags ((size_t) numFrames, std::vector<float> ((size_t) maxBin, 0.0f));

    for (int t = 0; t < numFrames; ++t)
    {
        const int start = t * hop - N / 2;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const int idx = start + i;
            if (idx >= 0 && idx < (int) x.size())
                buf[(size_t) i] = x[(size_t) idx] * window[(size_t) i];
        }
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k < maxBin; ++k)
            mags[(size_t) t][(size_t) k] = buf[(size_t) k] * magScale;
    }

    // Harmonic/percussive separation by median filtering: sustained partials are smooth
    // across time, drum hits are smooth across frequency.
    constexpr int timeHalf = 4, freqHalf = 8;
    const int minPeakBin = juce::jmax (2, (int) std::floor (55.0 * N / f.analysisRate));
    const int maxPeakBin = juce::jmin (maxBin - 2, (int) std::ceil (2500.0 * N / f.analysisRate));

    std::vector<std::vector<Peak>> peaks ((size_t) numFrames);
    f.harmonicRatio.assign ((size_t) numFrames, 0.0f);
    std::vector<float> harm ((size_t) maxBin), tmp;
    tmp.reserve (2 * freqHalf + 1);

    for (int t = 0; t < numFrames; ++t)
    {
        const auto& m = mags[(size_t) t];
        double totalE = 0.0, harmE = 0.0;
        float frameMax = 0.0f;

        for (int k = 0; k < maxBin; ++k)
        {
            tmp.clear();
            for (int j = juce::jmax (0, t - timeHalf); j <= juce::jmin (numFrames - 1, t + timeHalf); ++j)
                tmp.push_back (mags[(size_t) j][(size_t) k]);
            std::nth_element (tmp.begin(), tmp.begin() + (ptrdiff_t) tmp.size() / 2, tmp.end());
            const float H = tmp[tmp.size() / 2];

            tmp.clear();
            for (int j = juce::jmax (0, k - freqHalf); j <= juce::jmin (maxBin - 1, k + freqHalf); ++j)
                tmp.push_back (m[(size_t) j]);
            std::nth_element (tmp.begin(), tmp.begin() + (ptrdiff_t) tmp.size() / 2, tmp.end());
            const float P = tmp[tmp.size() / 2];

            const float mask = (H * H) / (H * H + P * P + kEps);
            harm[(size_t) k] = m[(size_t) k] * mask;

            totalE += (double) m[(size_t) k] * m[(size_t) k];
            harmE += (double) harm[(size_t) k] * harm[(size_t) k];
            if (k >= minPeakBin && k <= maxPeakBin)
                frameMax = std::max (frameMax, harm[(size_t) k]);
        }

        f.harmonicRatio[(size_t) t] = totalE > 1.0e-10 ? (float) (harmE / totalE) : 0.0f;

        if (frameMax < 1.0e-5f)
            continue;

        for (int k = minPeakBin; k <= maxPeakBin; ++k)
        {
            const float h = harm[(size_t) k];
            if (h > harm[(size_t) k - 1] && h >= harm[(size_t) k + 1] && h > frameMax * 0.03f)
            {
                const float a = std::log (harm[(size_t) k - 1] + kEps);
                const float b = std::log (h + kEps);
                const float c = std::log (harm[(size_t) k + 1] + kEps);
                const float denom = a - 2.0f * b + c;
                const float p = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
                const double freq = ((double) k + juce::jlimit (-0.5f, 0.5f, p)) * f.analysisRate / N;
                peaks[(size_t) t].push_back ({ (float) (69.0 + 12.0 * std::log2 (freq / 440.0)), h });
            }
        }
    }

    // Tuning: circular mean of every peak's distance from the nearest equal-tempered pitch.
    double sx = 0.0, sy = 0.0;
    for (const auto& fr : peaks)
        for (const auto& p : fr)
        {
            const double dev = p.midi - std::round (p.midi);
            sx += p.amp * std::cos (kTwoPi * dev);
            sy += p.amp * std::sin (kTwoPi * dev);
        }
    f.tuningSemitones = (std::abs (sx) + std::abs (sy) > 0.0) ? std::atan2 (sy, sx) / kTwoPi : 0.0;

    f.chroma.assign ((size_t) numFrames, Chroma {});
    f.strongestPitch.assign ((size_t) numFrames, -1.0f);

    for (int t = 0; t < numFrames; ++t)
    {
        float best = 0.0f;
        for (const auto& p : peaks[(size_t) t])
        {
            const double pitch = p.midi - f.tuningSemitones;
            const int pc = ((int) std::lround (pitch) % 12 + 12) % 12;
            f.chroma[(size_t) t][(size_t) pc] += std::sqrt (p.amp);
            if (p.amp > best)
            {
                best = p.amp;
                f.strongestPitch[(size_t) t] = (float) pitch;
            }
        }
    }
}

//==============================================================================
// Onset curve prepared for periodicity analysis: local mean removed, half-wave
// rectified, lightly smoothed and normalised.
std::vector<float> rhythmCurve (const AnalysisFeatures& f)
{
    const auto& o = f.onsetEnv;
    const int n = (int) o.size();
    std::vector<float> x ((size_t) n, 0.0f);
    if (n == 0)
        return x;

    const int half = juce::jmax (1, (int) std::round (f.fps * 0.15));
    std::vector<double> cum ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        cum[(size_t) i + 1] = cum[(size_t) i] + o[(size_t) i];

    for (int i = 0; i < n; ++i)
    {
        const int a = juce::jmax (0, i - half), b = juce::jmin (n, i + half + 1);
        const double mean = (cum[(size_t) b] - cum[(size_t) a]) / (b - a);
        x[(size_t) i] = (float) juce::jmax (0.0, (double) o[(size_t) i] - mean);
    }

    std::vector<float> s ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        float acc = 0.5f * x[(size_t) i];
        if (i > 0) acc += 0.25f * x[(size_t) i - 1];
        if (i + 1 < n) acc += 0.25f * x[(size_t) i + 1];
        s[(size_t) i] = acc;
    }

    // Normalise by a high percentile rather than the max, so one huge hit (typically the very
    // first frame, which is measured against silence) can't flatten everything else.
    std::vector<float> nonZero;
    for (auto v : s)
        if (v > 0.0f)
            nonZero.push_back (v);
    const float scale = percentile (nonZero, 0.98f);
    if (scale > 0.0f)
        for (auto& v : s)
            v = std::min (1.5f, v / scale);
    return s;
}

struct GridFit
{
    double score = 0.0;
    double phaseFrames = 0.0;
};

// How well a constant-tempo beat grid lines up with the onsets, and at which phase.
GridFit fitGrid (const std::vector<float>& curve, double fps, double bpm)
{
    GridFit best;
    const double period = 60.0 / bpm * fps;
    const double n = (double) curve.size() - 1.0;
    if (period < 2.0 || n < period)
        return best;

    for (double phase = 0.0; phase < period; phase += 0.25)
    {
        double sum = 0.0;
        int count = 0;
        for (double t = phase; t < n; t += period)
        {
            sum += sampleAt (curve, t);
            ++count;
        }
        const double score = count > 0 ? sum / count : 0.0;
        if (score > best.score)
            best = { score, phase };
    }
    return best;
}

// True if the sample opens with a hit (as loops do) rather than silence or a fade.
bool startsWithHit (const AnalysisFeatures& f)
{
    const auto& o = f.onsetEnv;
    if (o.size() < 4)
        return false;
    const float peak = *std::max_element (o.begin(), o.end());
    return peak > 0.0f && std::max ({ o[0], o[1], o[2], o[3] }) > 0.15f * peak;
}

float zeroMeanAutocorr (const std::vector<float>& z, int lag)
{
    const int n = (int) z.size();
    if (lag <= 0 || n - lag < 2)
        return 0.0f;
    double acc = 0.0;
    for (int i = 0; i + lag < n; ++i)
        acc += (double) z[(size_t) i] * z[(size_t) i + (size_t) lag];
    return (float) (acc / (n - lag));
}

void zScore (std::vector<float>& v)
{
    if (v.empty())
        return;
    const double mean = std::accumulate (v.begin(), v.end(), 0.0) / (double) v.size();
    double var = 0.0;
    for (auto x : v)
        var += (x - mean) * (x - mean);
    const double sd = std::sqrt (var / (double) v.size());
    for (auto& x : v)
        x = sd > 1.0e-9 ? (float) ((x - mean) / sd) : 0.0f;
}

float cosineSimilarity (const Chroma& a, const Chroma& b)
{
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < 12; ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return (aa > 0 && bb > 0) ? (float) (ab / std::sqrt (aa * bb)) : 0.0f;
}

float pearson (const Chroma& a, const std::array<float, 12>& b, int rotation)
{
    double ma = 0, mb = 0;
    for (size_t i = 0; i < 12; ++i)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= 12.0;
    mb /= 12.0;

    double num = 0, da = 0, db = 0;
    for (int i = 0; i < 12; ++i)
    {
        const double x = a[(size_t) i] - ma;
        const double y = b[(size_t) ((i - rotation + 12) % 12)] - mb;
        num += x * y;
        da += x * x;
        db += y * y;
    }
    return (da > 0 && db > 0) ? (float) (num / std::sqrt (da * db)) : 0.0f;
}

// Krumhansl-Kessler probe-tone profiles and Temperley's corpus profiles; averaging
// the two correlations is more robust than either one alone.
constexpr std::array<float, 12> kKKMajor { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
constexpr std::array<float, 12> kKKMinor { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
constexpr std::array<float, 12> kTMajor { 0.748f, 0.060f, 0.488f, 0.082f, 0.670f, 0.460f, 0.096f, 0.715f, 0.104f, 0.366f, 0.057f, 0.400f };
constexpr std::array<float, 12> kTMinor { 0.712f, 0.084f, 0.474f, 0.618f, 0.049f, 0.460f, 0.105f, 0.747f, 0.404f, 0.067f, 0.133f, 0.330f };

Chroma sumChroma (const AnalysisFeatures& f, int fromFrame, int toFrame)
{
    Chroma c {};
    fromFrame = juce::jlimit (0, (int) f.chroma.size(), fromFrame);
    toFrame = juce::jlimit (fromFrame, (int) f.chroma.size(), toFrame);
    for (int t = fromFrame; t < toFrame; ++t)
        for (size_t i = 0; i < 12; ++i)
            c[i] += f.chroma[(size_t) t][i];
    return c;
}

// Pin an onset down to ~1 ms. Works on the first difference of the signal, which boosts the
// broadband attack of a hit and nearly removes the low sine tail of whatever was ringing before
// it (otherwise a hat on top of a decaying 808 gets placed on the 808's waveform ripples).
juce::int64 refineOnset (const std::vector<float>& mono, double sr, double approxSeconds, juce::int64 lowerBound)
{
    const auto len = (juce::int64) mono.size();
    const int block = juce::jmax (1, (int) std::round (sr * 0.001));
    const auto centre = (juce::int64) std::llround (approxSeconds * sr);
    const auto a = juce::jlimit ((juce::int64) 1, len, juce::jmax (lowerBound + 1, centre - (juce::int64) (sr * 0.035)));
    const auto b = juce::jlimit (a, len, centre + (juce::int64) (sr * 0.035));
    const int numBlocks = (int) ((b - a) / block);
    if (numBlocks < 3)
        return juce::jlimit (lowerBound, len, centre);

    std::vector<float> env ((size_t) numBlocks, 0.0f);
    for (int j = 0; j < numBlocks; ++j)
    {
        double e = 0.0;
        for (int i = 0; i < block; ++i)
        {
            const auto idx = (size_t) (a + j * block + i);
            const float d = mono[idx] - mono[idx - 1];
            e += (double) d * d;
        }
        env[(size_t) j] = (float) std::sqrt (e / block);
    }

    int steepest = 1;
    float bestRise = -1.0f;
    for (int j = 1; j < numBlocks; ++j)
    {
        const float rise = env[(size_t) j] - env[(size_t) j - 1];
        if (rise > bestRise)
        {
            bestRise = rise;
            steepest = j;
        }
    }

    // Walk back from the steepest rise to where the level stops falling, or drops to the level of
    // whatever was already ringing: that's where the hit starts.
    const float floorLevel = env[(size_t) steepest] * 0.1f;
    int j = steepest - 1;
    while (j > 0 && env[(size_t) j - 1] < env[(size_t) j] && env[(size_t) j - 1] > floorLevel && steepest - j < 30)
        --j;

    auto pos = a + (juce::int64) j * block;

    // Snap back to a zero crossing (within 1 ms) to avoid a click at the slice start.
    const auto searchStart = juce::jmax (lowerBound + 1, pos - (juce::int64) (sr * 0.001));
    for (auto i = pos; i > searchStart; --i)
        if ((mono[(size_t) i] >= 0.0f) != (mono[(size_t) i - 1] >= 0.0f))
            return i;

    return juce::jlimit (lowerBound, len, pos);
}

const char* const kSharpNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const kKeyNames[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

} // namespace

//==============================================================================
AnalysisFeatures computeFeatures (const juce::AudioBuffer<float>& source, double sampleRate)
{
    AnalysisFeatures f;
    f.sourceRate = sampleRate;
    f.sourceLength = source.getNumSamples();
    f.analysisRate = kAnalysisRate;
    f.mono = resampleForAnalysis (mixToMono (source), sampleRate, kAnalysisRate);

    computeOnsetFeatures (f);
    computeHarmonicFeatures (f);
    return f;
}

//==============================================================================
namespace
{
struct LineFit
{
    double period = 0.0; // seconds per beat
    double phase = 0.0;  // seconds, time of some beat
    int used = 0;
};

// Least-squares fit of a beat grid to the (sample-accurate) onset times. Each strong onset is
// assigned to its nearest 8th note; onsets far from any 8th (swung 16ths, flams) are ignored.
LineFit fitOnsetsToGrid (const std::vector<OnsetCandidate>& onsets, double sampleRate, double period, double phase)
{
    LineFit fit { period, phase, 0 };
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        const double step = fit.period / 2.0;
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        int used = 0;
        for (const auto& o : onsets)
        {
            if (o.strength < 0.15f)
                continue;
            const double t = (double) o.position / sampleRate;
            const double n = std::round ((t - fit.phase) / step);
            if (std::abs (t - (fit.phase + n * step)) > step * 0.2)
                continue;
            const double w = o.strength;
            sw += w;
            sx += w * n;
            sy += w * t;
            sxx += w * n * n;
            sxy += w * n * t;
            ++used;
        }
        const double denom = sw * sxx - sx * sx;
        if (used < 4 || std::abs (denom) < 1.0e-12)
            break;
        const double slope = (sw * sxy - sx * sy) / denom;
        if (slope <= 0.0 || std::abs (slope * 2.0 - period) / period > 0.02)
            break;
        fit.period = slope * 2.0;
        fit.phase = (sy - slope * sx) / sw;
        fit.used = used;
    }
    return fit;
}

float windowMax (const std::vector<float>& env, double centre)
{
    float m = 0.0f;
    for (int i = (int) std::round (centre) - 2; i <= (int) std::round (centre) + 2; ++i)
        if (i >= 0 && i < (int) env.size())
            m = std::max (m, env[(size_t) i]);
    return m;
}

// Snares and claps landing halfway between the beats (with kicks on the beats) means the tempo
// was read at half speed, the classic drum & bass / fast-tempo mistake.
bool snaresSitBetweenBeats (const AnalysisFeatures& f, double bpm)
{
    const double period = 60.0 / bpm * f.fps;
    const int n = (int) f.lowEnv.size();
    if (n < period * 4)
        return false;

    // Phase from the kicks
    double bestPhase = 0.0, bestLow = -1.0;
    for (double phase = 0.0; phase < period; phase += 0.5)
    {
        double sum = 0.0;
        int c = 0;
        for (double t = phase; t < n - 1; t += period, ++c)
            sum += windowMax (f.lowEnv, t);
        if (c > 0 && sum / c > bestLow)
        {
            bestLow = sum / c;
            bestPhase = phase;
        }
    }

    double lowOn = 0, lowOff = 0, midOn = 0, midOff = 0;
    int count = 0;
    for (double t = bestPhase; t + period * 0.5 < n - 1; t += period, ++count)
    {
        lowOn += windowMax (f.lowEnv, t);
        lowOff += windowMax (f.lowEnv, t + period * 0.5);
        midOn += windowMax (f.midEnv, t);
        midOff += windowMax (f.midEnv, t + period * 0.5);
    }
    return count >= 4 && lowOn > lowOff * 2.5 && midOff > midOn * 0.75;
}
} // namespace

//==============================================================================
TempoResult estimateTempo (const AnalysisFeatures& f, const std::vector<OnsetCandidate>& onsets)
{
    TempoResult result;
    const auto curve = rhythmCurve (f);
    const int n = (int) curve.size();
    if (n < f.fps * 1.5)
        return result;

    constexpr double minBpm = 50.0, maxBpm = 220.0;
    const int maxLag = juce::jmin (n - 1, (int) std::ceil (4.0 * 60.0 * f.fps / minBpm) + 2);

    std::vector<float> acf ((size_t) maxLag + 1, 0.0f);
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double acc = 0.0;
        for (int t = 0; t + lag < n; ++t)
            acc += (double) curve[(size_t) t] * curve[(size_t) t + (size_t) lag];
        acf[(size_t) lag] = (float) (acc / (n - lag));
    }

    auto combScore = [&] (double bpm)
    {
        const double lag = 60.0 * f.fps / bpm;
        double s = 0.0, w = 0.0;
        for (int m = 1; m <= 4; ++m)
        {
            if (m * lag >= maxLag)
                break;
            s += sampleAt (acf, m * lag) / m;
            w += 1.0 / m;
        }
        return w > 0 ? s / w : 0.0;
    };

    auto prior = [] (double bpm)
    {
        const double d = std::log2 (bpm / 115.0);
        return std::exp (-0.5 * d * d);
    };

    double bestBpm = 120.0, bestScore = -1.0, sumScore = 0.0;
    int count = 0;
    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.05)
    {
        const double s = combScore (bpm);
        sumScore += s;
        ++count;
        if (s * prior (bpm) > bestScore)
        {
            bestScore = s * prior (bpm);
            bestBpm = bpm;
        }
    }

    if (bestScore <= 0.0)
        return result;

    // Fine-tune: first a whole-file grid fit on the onset curve (~0.2% accurate), then a
    // straight-line fit through the sample-accurate onset times (~0.02% accurate).
    double refined = bestBpm, refinedScore = -1.0, phaseFrames = 0.0;
    for (double bpm = bestBpm * 0.975; bpm <= bestBpm * 1.025; bpm += 0.01)
    {
        const auto fit = fitGrid (curve, f.fps, bpm);
        if (fit.score > refinedScore)
        {
            refinedScore = fit.score;
            refined = bpm;
            phaseFrames = fit.phaseFrames;
        }
    }

    if (refined < 110.0 && snaresSitBetweenBeats (f, refined))
        refined *= 2.0;

    const auto line = fitOnsetsToGrid (onsets, f.sourceRate, 60.0 / refined, phaseFrames / f.fps);
    if (line.used >= 6)
        refined = 60.0 / line.period;

    // Loops: if the file is a whole number of bars long, that gives the exact tempo.
    const double duration = f.durationSeconds();
    if (startsWithHit (f) && duration > 1.0)
    {
        for (int unit : { 4, 3 })
        {
            const int nBeats = (int) std::lround (duration * refined / 60.0 / unit) * unit;
            if (nBeats < unit)
                continue;
            const double loopBpm = 60.0 * nBeats / duration;
            if (std::abs (loopBpm - refined) / refined < 0.004)
            {
                refined = loopBpm;
                result.loopDetected = true;
                break;
            }
        }
    }

    // Most produced music sits on a whole-number tempo.
    const double rounded = std::round (refined);
    if (std::abs (rounded - refined) < (result.loopDetected ? 0.03 : 0.1))
        refined = rounded;

    result.bpm = refined;
    const double mean = sumScore / juce::jmax (1, count);
    result.confidence = (float) juce::jlimit (0.0, 1.0, (combScore (bestBpm) / juce::jmax (1.0e-9, mean) - 1.0) / 2.0);
    return result;
}

//==============================================================================
GridResult estimateGrid (const AnalysisFeatures& f, const std::vector<OnsetCandidate>& onsets, double bpm,
                         TimeSignature forcedTimeSig, bool autoMeter, bool loopHint)
{
    GridResult g;
    g.timeSig = forcedTimeSig;

    const auto curve = rhythmCurve (f);
    const int n = (int) curve.size();
    if (bpm <= 0.0 || n < 4)
        return g;

    const double period = 60.0 / bpm * f.fps;
    auto fit = fitGrid (curve, f.fps, bpm);

    // Samples are almost always cut on a beat: prefer a grid that starts at 0 if it fits nearly as well.
    if (sampleAt (curve, 0.0) > 0.2f || loopHint)
    {
        double s0 = 0.0;
        int c = 0;
        for (double t = 0.0; t < n - 1; t += period, ++c)
            s0 += sampleAt (curve, t);
        if (c > 0 && s0 / c >= fit.score * 0.85)
            fit.phaseFrames = 0.0;
    }

    // Sharpen the phase using the sample-accurate onsets (tempo stays as given).
    if (fit.phaseFrames > 0.0)
    {
        const double step = 30.0 / bpm; // 8th note in seconds
        double sum = 0, wsum = 0;
        for (const auto& o : onsets)
        {
            if (o.strength < 0.15f)
                continue;
            const double t = (double) o.position / f.sourceRate;
            const double r = t - (fit.phaseFrames / f.fps + std::round ((t - fit.phaseFrames / f.fps) / step) * step);
            if (std::abs (r) < step * 0.2)
            {
                sum += o.strength * r;
                wsum += o.strength;
            }
        }
        if (wsum > 0.0)
            fit.phaseFrames = juce::jmax (0.0, fit.phaseFrames + sum / wsum * f.fps);
    }

    std::vector<double> beats;
    for (double t = fit.phaseFrames; t < n - 1; t += period)
        beats.push_back (t);

    const int K = (int) beats.size();
    g.downbeatSeconds = fit.phaseFrames / f.fps;
    if (K < 4)
        return g;

    // Per-beat evidence for "this is beat 1": kicks and chord changes land there, while
    // snare/clap hits mostly land on the backbeat.
    std::vector<float> low ((size_t) K), snare ((size_t) K), change ((size_t) K);
    std::vector<Chroma> beatChroma ((size_t) K);

    double harmonicPresence = 0.0;
    for (int k = 0; k < K; ++k)
    {
        low[(size_t) k] = windowMax (f.lowEnv, beats[(size_t) k]);
        snare[(size_t) k] = windowMax (f.midEnv, beats[(size_t) k]);

        const double t0 = beats[(size_t) k] / f.fps;
        const double t1 = (k + 1 < K ? beats[(size_t) k + 1] : beats[(size_t) k] + period) / f.fps;
        beatChroma[(size_t) k] = sumChroma (f, (int) std::floor (t0 * f.chromaFps), (int) std::ceil (t1 * f.chromaFps));
    }

    for (int k = 1; k < K; ++k)
        change[(size_t) k] = 1.0f - cosineSimilarity (beatChroma[(size_t) k], beatChroma[(size_t) k - 1]);
    {
        std::vector<float> rest (change.begin() + 1, change.end());
        change[0] = percentile (rest, 0.5f);
    }

    for (auto r : f.harmonicRatio)
        harmonicPresence += r;
    harmonicPresence = f.harmonicRatio.empty() ? 0.0 : harmonicPresence / (double) f.harmonicRatio.size();
    const float changeWeight = (float) juce::jlimit (0.0, 1.0, harmonicPresence * 2.5);

    zScore (low);
    zScore (snare);
    zScore (change);

    if (autoMeter)
    {
        auto repetition = [&] (int lag)
        {
            return (zeroMeanAutocorr (low, lag) + 0.5f * zeroMeanAutocorr (snare, lag) + changeWeight * zeroMeanAutocorr (change, lag))
                   / (1.5f + changeWeight);
        };

        const float a2 = repetition (2), a3 = repetition (3), a4 = repetition (4);
        float margin = 0.15f;
        if (loopHint)
        {
            const int totalBeats = (int) std::lround (f.durationSeconds() * bpm / 60.0);
            if (totalBeats % 3 == 0 && totalBeats % 4 != 0)
                margin = 0.05f;
            else if (totalBeats % 4 == 0 && totalBeats % 3 != 0)
                margin = 0.3f;
        }

        if (K >= 12 && a3 > a4 + margin && a3 > a2 + 0.05f)
        {
            g.timeSig = { 3, 4 };
            g.meterConfidence = juce::jlimit (0.0f, 1.0f, a3 - a4);
        }
        else
        {
            g.timeSig = { 4, 4 };
            g.meterConfidence = K < 8 ? 0.2f : juce::jlimit (0.0f, 1.0f, 0.5f + (a4 - a3));
        }
    }

    const double barBeats = g.timeSig.quarterBeatsPerBar();
    const int M = (int) std::lround (barBeats);
    if (std::abs (barBeats - M) > 1.0e-6 || M < 2 || K < M)
        return g; // odd x/8 bars: keep the first beat, the user can move bar 1 by hand

    int bestPhase = 0;
    float bestContrast = -1.0e9f;
    for (int p = 0; p < M; ++p)
    {
        double on = 0, off = 0;
        int nOn = 0, nOff = 0;
        for (int k = 0; k < K; ++k)
        {
            const float v = low[(size_t) k] - 0.5f * snare[(size_t) k] + changeWeight * change[(size_t) k];
            if ((k - p) % M == 0)
            {
                on += v;
                ++nOn;
            }
            else
            {
                off += v;
                ++nOff;
            }
        }

        float contrast = (float) ((nOn ? on / nOn : 0.0) - (nOff ? off / nOff : 0.0));
        if (p == 0 && fit.phaseFrames < 1.5)
            contrast += loopHint ? 0.6f : 0.25f; // sample starts on a beat: probably on beat 1
        if (contrast > bestContrast)
        {
            bestContrast = contrast;
            bestPhase = p;
        }
    }

    g.downbeatSeconds = beats[(size_t) bestPhase] / f.fps;
    return g;
}

//==============================================================================
KeyResult estimateKey (const AnalysisFeatures& f)
{
    KeyResult r;
    const auto total = sumChroma (f, 0, (int) f.chroma.size());
    const double sum = std::accumulate (total.begin(), total.end(), 0.0);
    if (sum <= 1.0e-9)
        return r;

    for (size_t i = 0; i < 12; ++i)
        r.profile[i] = (float) (total[i] / sum);

    std::vector<KeyGuess> guesses;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        guesses.push_back ({ tonic, false, 0.5f * (pearson (r.profile, kKKMajor, tonic) + pearson (r.profile, kTMajor, tonic)) });
        guesses.push_back ({ tonic, true, 0.5f * (pearson (r.profile, kKKMinor, tonic) + pearson (r.profile, kTMinor, tonic)) });
    }
    std::sort (guesses.begin(), guesses.end(), [] (const KeyGuess& a, const KeyGuess& b) { return a.score > b.score; });
    r.best = guesses[0];
    r.alt = guesses[1];

    // How tonal is this? Mean harmonic ratio over frames that aren't silent.
    double hr = 0.0;
    int frames = 0;
    for (size_t t = 0; t < f.harmonicRatio.size(); ++t)
    {
        const auto onsetFrame = (size_t) std::round ((double) t * f.chromaHop / f.hop);
        if (onsetFrame < f.frameDb.size() && f.frameDb[onsetFrame] > -50.0f)
        {
            hr += f.harmonicRatio[t];
            ++frames;
        }
    }
    const double tonalness = frames > 0 ? hr / frames : 0.0;

    r.hasKey = r.best.score > 0.6f && tonalness > 0.2;
    const float clarity = juce::jlimit (0.0f, 1.0f, (r.best.score - 0.6f) / 0.3f);
    const float separation = juce::jlimit (0.3f, 1.0f, (r.best.score - r.alt.score) / 0.12f);
    r.confidence = r.hasKey ? clarity * separation : 0.0f;
    return r;
}

//==============================================================================
std::vector<OnsetCandidate> detectOnsets (const AnalysisFeatures& f, const juce::AudioBuffer<float>& source)
{
    std::vector<OnsetCandidate> out;
    const auto& o = f.onsetEnv;
    const int n = (int) o.size();
    if (n < 3)
        return out;

    std::vector<float> nonZero;
    for (auto v : o)
        if (v > 0.0f)
            nonZero.push_back (v);
    const float scale = percentile (nonZero, 0.98f);
    if (scale <= 0.0f)
        return out;

    std::vector<float> on ((size_t) n);
    for (int i = 0; i < n; ++i)
        on[(size_t) i] = o[(size_t) i] / scale;

    struct Raw
    {
        int frame;
        float prominence;
    };
    std::vector<Raw> raw;

    for (int t = 0; t < n - 1; ++t)
    {
        const float v = on[(size_t) t];
        bool isPeak = true;
        for (int j = juce::jmax (0, t - 3); j <= juce::jmin (n - 1, t + 3) && isPeak; ++j)
            if (j != t && (on[(size_t) j] > v || (j < t && ! (on[(size_t) j] < v))))
                isPeak = false;
        if (! isPeak)
            continue;

        float level = -120.0f;
        for (int j = t; j <= juce::jmin (n - 1, t + 3); ++j)
            level = std::max (level, f.frameDb[(size_t) j]);
        if (level < -60.0f)
            continue;

        double mean = 0.0;
        int c = 0;
        for (int j = juce::jmax (0, t - 8); j <= juce::jmin (n - 1, t + 4); ++j, ++c)
            mean += on[(size_t) j];
        const float prom = v - (float) (mean / c);
        if (prom > 0.04f)
        {
            raw.push_back ({ t, prom });
        }
    }

    // Strength relative to the strongest hits: the mean of the top 10% (at least 3). Not the single
    // biggest, because the very first frame is measured against silence and would make everything
    // else look weak; and not a plain percentile, because in sustained material most candidates
    // are small wobbles and a percentile would promote them.
    std::vector<float> proms;
    for (const auto& r : raw)
        if (r.frame > 1)
            proms.push_back (r.prominence);
    std::sort (proms.begin(), proms.end(), std::greater<float>());
    const size_t top = juce::jmin (proms.size(), juce::jmax ((size_t) 3, proms.size() / 10));
    const float promScale = top > 0 ? juce::jmax (1.0e-6f, std::accumulate (proms.begin(), proms.begin() + (std::ptrdiff_t) top, 0.0f) / (float) top)
                                     : 1.0f;

    const auto mono = mixToMono (source);
    juce::int64 lowerBound = 0;
    for (const auto& r : raw)
    {
        const auto pos = r.frame <= 1 && f.frameDb[0] > -60.0f ? (juce::int64) 0
                                                                 : refineOnset (mono, f.sourceRate, r.frame / f.fps, lowerBound);
        out.push_back ({ pos, juce::jlimit (0.0f, 1.0f, r.prominence / promScale) });
        lowerBound = pos + 1;
    }
    return out;
}

//==============================================================================
SliceDescription describeSlice (const AnalysisFeatures& f, const juce::AudioBuffer<float>& source, juce::int64 start, juce::int64 end)
{
    SliceDescription d;
    start = juce::jlimit ((juce::int64) 0, (juce::int64) source.getNumSamples(), start);
    end = juce::jlimit (start, (juce::int64) source.getNumSamples(), end);
    if (end - start < 16)
    {
        d.type = "Silence";
        return d;
    }

    float peak = 0.0f;
    double sq = 0.0;
    for (int c = 0; c < source.getNumChannels(); ++c)
    {
        const float* p = source.getReadPointer (c);
        for (auto i = start; i < end; ++i)
        {
            peak = std::max (peak, std::abs (p[i]));
            sq += (double) p[i] * p[i];
        }
    }
    d.peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);
    d.rmsDb = (float) juce::Decibels::gainToDecibels (std::sqrt (sq / ((double) (end - start) * juce::jmax (1, source.getNumChannels()))), -100.0);

    if (d.peakDb < -60.0f)
    {
        d.type = "Silence";
        return d;
    }

    // Spectral shape of the first ~250 ms (where a hit's character lives).
    constexpr int order = 10, N = 1 << order;
    const double ar = f.analysisRate;
    const auto a0 = (int) std::floor ((double) start * ar / f.sourceRate);
    const auto a1 = juce::jmin ((int) f.mono.size(), (int) std::ceil ((double) end * ar / f.sourceRate));
    const int analyseEnd = juce::jmin (a1, a0 + (int) (0.25 * ar));

    juce::dsp::FFT fft (order);
    const auto window = makeHann (N);
    std::vector<float> buf ((size_t) N * 2), power ((size_t) N / 2 + 1, 0.0f);
    int frames = 0;
    for (int s = a0; s < juce::jmax (a0 + 1, analyseEnd); s += N / 2, ++frames)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N && s + i < analyseEnd; ++i)
            if (s + i >= 0)
                buf[(size_t) i] = f.mono[(size_t) (s + i)] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= N / 2; ++k)
            power[(size_t) k] += buf[(size_t) k] * buf[(size_t) k];
    }

    double total = 0, lowE = 0, highE = 0, centroidNum = 0, logSum = 0, linSum = 0;
    int flatBins = 0;
    for (int k = 1; k <= N / 2; ++k)
    {
        const double freq = k * ar / N;
        const double p = power[(size_t) k];
        total += p;
        centroidNum += freq * p;
        if (freq < 150.0)
            lowE += p;
        else if (freq > 5000.0)
            highE += p;
        if (freq > 100.0 && freq < 8000.0)
        {
            logSum += std::log (p + 1.0e-12);
            linSum += p;
            ++flatBins;
        }
    }

    const double lowRatio = total > 0 ? lowE / total : 0.0;
    const double highRatio = total > 0 ? highE / total : 0.0;
    const double centroid = total > 0 ? centroidNum / total : 0.0;
    const double flatness = (flatBins > 0 && linSum > 0) ? std::exp (logSum / flatBins) / (linSum / flatBins) : 0.0;

    // Harmonic content across the whole slice.
    const int c0 = (int) std::floor ((double) start / f.sourceRate * f.chromaFps);
    const int c1 = juce::jmax (c0 + 1, (int) std::ceil ((double) end / f.sourceRate * f.chromaFps));
    double hr = 0.0;
    int hrCount = 0;
    std::vector<float> pitches;
    for (int t = juce::jmax (0, c0); t < juce::jmin ((int) f.harmonicRatio.size(), c1); ++t)
    {
        hr += f.harmonicRatio[(size_t) t];
        ++hrCount;
        if (f.strongestPitch[(size_t) t] > 0.0f)
            pitches.push_back (f.strongestPitch[(size_t) t]);
    }
    hr = hrCount > 0 ? hr / hrCount : 0.0;
    const double duration = (double) (end - start) / f.sourceRate;

    if (lowRatio > 0.45 && centroid < 900.0)
        d.type = (hr > 0.5 && duration > 0.25) ? "Bass" : "Kick";
    else if (highRatio > 0.45 && hr < 0.45)
        d.type = "Hi-hat";
    else if (flatness > 0.2 && hr < 0.4 && centroid > 900.0)
        d.type = "Snare/Clap";
    else if (hr > 0.5)
        d.type = "Tonal";
    else
        d.type = "Mixed";

    // Chord or note, if there's enough pitched material to say anything.
    const auto chroma = sumChroma (f, c0, c1);
    const double chromaSum = std::accumulate (chroma.begin(), chroma.end(), 0.0);
    if (hr < 0.3 || chromaSum < 1.0e-4)
        return d;

    std::array<int, 12> order12;
    std::iota (order12.begin(), order12.end(), 0);
    std::sort (order12.begin(), order12.end(), [&] (int a, int b) { return chroma[(size_t) a] > chroma[(size_t) b]; });
    const double top = chroma[(size_t) order12[0]], second = chroma[(size_t) order12[1]];

    if (top / chromaSum > 0.45 && second / top < 0.5 && ! pitches.empty())
    {
        std::nth_element (pitches.begin(), pitches.begin() + (ptrdiff_t) pitches.size() / 2, pitches.end());
        d.harmony = midiNoteName ((int) std::lround (pitches[pitches.size() / 2]));
        return d;
    }

    float bestScore = 0.0f;
    juce::String bestName;
    for (int root = 0; root < 12; ++root)
    {
        for (bool minor : { false, true })
        {
            Chroma tpl {};
            tpl[(size_t) root] = 1.0f;
            tpl[(size_t) ((root + (minor ? 3 : 4)) % 12)] = 1.0f;
            tpl[(size_t) ((root + 7) % 12)] = 1.0f;
            const float s = cosineSimilarity (chroma, tpl);
            if (s > bestScore)
            {
                bestScore = s;
                bestName = juce::String (kSharpNames[root]) + (minor ? "m" : "");
            }
        }
    }

    if (bestScore >= 0.8f)
        d.harmony = bestName;
    else if (bestScore >= 0.65f)
        d.harmony = bestName + "?";
    return d;
}

//==============================================================================
GridPosition gridPositionAt (double seconds, double bpm, double downbeatSeconds, TimeSignature ts)
{
    GridPosition g;
    if (bpm <= 0.0 || ts.numerator <= 0 || ts.denominator <= 0)
        return g;

    const double secondsPer16th = 60.0 / bpm / 4.0;
    const double sixteenths = (seconds - downbeatSeconds) / secondsPer16th;
    const double nearest = std::round (sixteenths);
    g.offsetMs = (sixteenths - nearest) * secondsPer16th * 1000.0;
    g.onGrid = std::abs (g.offsetMs) <= juce::jmin (30.0, secondsPer16th * 1000.0 / 3.0);

    const auto s16 = (juce::int64) (g.onGrid ? nearest : std::floor (sixteenths));
    const int per16thUnit = juce::jmax (1, 16 / ts.denominator);
    const juce::int64 barLen = (juce::int64) ts.numerator * per16thUnit;

    const juce::int64 barIndex = s16 >= 0 ? s16 / barLen : -((-s16 + barLen - 1) / barLen);
    const auto within = (int) (s16 - barIndex * barLen);

    g.bar = (int) barIndex + 1;
    g.beat = within / per16thUnit + 1;
    g.sixteenth = within % per16thUnit + 1;

    if (! g.onGrid)
        g.describe = "Off grid";
    else if (within == 0)
        g.describe = "Bar start";
    else if (g.sixteenth == 1)
        g.describe = "Beat " + juce::String (g.beat);
    else if (per16thUnit == 4)
        g.describe = juce::String (g.beat) + (g.sixteenth == 2 ? " e" : g.sixteenth == 3 ? " &" : " a");
    else
        g.describe = juce::String (g.beat) + " &";

    return g;
}

juce::String pitchClassName (int pc) { return kKeyNames[(pc % 12 + 12) % 12]; }

juce::String keyName (const KeyGuess& k) { return pitchClassName (k.tonic) + (k.minor ? " minor" : " major"); }

juce::String midiNoteName (int note)
{
    if (note < 0 || note > 127)
        return {};
    return juce::String (kSharpNames[note % 12]) + juce::String (note / 12);
}

} // namespace choplab
