#pragma once

// Synthetic drum and chord sounds with known timing, shared by the test programs.

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <random>
#include <vector>

namespace
{
constexpr double kRate = 44100.0;
constexpr double kPi = 3.141592653589793;

struct Synth
{
    juce::AudioBuffer<float> buf;
    std::mt19937 rng { 1234 };
    std::vector<double> hitTimes;

    explicit Synth (double seconds) : buf (2, (int) std::ceil (seconds * kRate)) { buf.clear(); }

    void add (int i, float v)
    {
        if (i >= 0 && i < buf.getNumSamples())
            for (int c = 0; c < 2; ++c)
                buf.addSample (c, i, v);
    }

    void kick (double t, float gain = 0.9f)
    {
        hitTimes.push_back (t);
        const int s = (int) std::round (t * kRate);
        double phase = 0.0;
        for (int i = 0; i < (int) (0.35 * kRate); ++i)
        {
            const double tt = i / kRate;
            const double f = 50.0 + 90.0 * std::exp (-tt * 35.0);
            phase += 2.0 * kPi * f / kRate;
            add (s + i, gain * (float) (std::sin (phase) * std::exp (-tt * 9.0)));
        }
    }

    void snare (double t, float gain = 0.5f)
    {
        hitTimes.push_back (t);
        const int s = (int) std::round (t * kRate);
        std::normal_distribution<float> n (0.0f, 1.0f);
        float lp = 0.0f;
        for (int i = 0; i < (int) (0.2 * kRate); ++i)
        {
            const double tt = i / kRate;
            const float noise = n (rng);
            lp += 0.5f * (noise - lp);
            const float body = (float) std::sin (2.0 * kPi * 190.0 * tt) * (float) std::exp (-tt * 30.0);
            add (s + i, gain * ((noise - lp) * 0.6f * (float) std::exp (-tt * 22.0) + 0.5f * body));
        }
    }

    void hat (double t, float gain = 0.15f)
    {
        hitTimes.push_back (t);
        const int s = (int) std::round (t * kRate);
        std::normal_distribution<float> n (0.0f, 1.0f);
        float prev = 0.0f;
        for (int i = 0; i < (int) (0.05 * kRate); ++i)
        {
            const float noise = n (rng);
            const float hp = noise - prev;
            prev = noise;
            add (s + i, gain * hp * (float) std::exp (-(i / kRate) * 70.0));
        }
    }

    // Piano-ish tone: a few decaying harmonics
    void note (double t, double dur, int midi, float gain = 0.12f)
    {
        const int s = (int) std::round (t * kRate);
        const double f0 = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
        for (int i = 0; i < (int) (dur * kRate); ++i)
        {
            const double tt = i / kRate;
            double v = 0.0;
            for (int h = 1; h <= 5; ++h)
                v += std::sin (2.0 * kPi * f0 * h * tt) / (h * h);
            const double env = std::min (1.0, tt * 200.0) * std::exp (-tt * 1.2) * std::min (1.0, (dur - tt) * 50.0);
            add (s + i, gain * (float) (v * env));
        }
    }

    void chord (double t, double dur, std::initializer_list<int> notes)
    {
        for (int n : notes)
            note (t, dur, n);
    }
};

void drumBar (Synth& s, double barStart, double beat)
{
    s.kick (barStart);
    s.kick (barStart + 1.5 * beat, 0.7f);
    s.kick (barStart + 2.0 * beat);
    s.snare (barStart + 1.0 * beat);
    s.snare (barStart + 3.0 * beat);
    for (int e = 0; e < 8; ++e)
        if (e % 2 == 1)
            s.hat (barStart + e * 0.5 * beat);
}

} // namespace
