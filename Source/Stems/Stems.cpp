#include "Stems.h"

#include "Common/Download.h"
#include "Demucs.h"
#include "Engine/Renderer.h"
#include "Lyrics/Lyrics.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace choplab
{
namespace
{
constexpr const char* kModelFileName = "ggml-model-htdemucs-4s-f16.bin";
constexpr juce::int64 kModelBytes = 80 * 1024 * 1024; // the weights alone are 80.08 MB

// demucs.cpp opens the model with fopen(), which on Windows wants the path in the ANSI code page.
// A user name like "José" survives that; one in another script falls back to the 8.3 short path.
std::string pathForFopen (const juce::File& file)
{
   #if JUCE_WINDOWS
    auto toAnsi = [] (const wchar_t* wide) -> std::string
    {
        BOOL lossy = FALSE;
        const int size = WideCharToMultiByte (CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, nullptr, 0, nullptr, &lossy);
        if (size <= 1 || lossy)
            return {};
        std::string s ((size_t) size, '\0');
        WideCharToMultiByte (CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, s.data(), size, nullptr, &lossy);
        if (lossy)
            return {};
        s.resize ((size_t) size - 1);
        return s;
    };
    const auto full = file.getFullPathName();
    if (auto s = toAnsi (full.toWideCharPointer()); ! s.empty())
        return s;
    std::wstring shortPath (32768, L'\0');
    const DWORD length = GetShortPathNameW (full.toWideCharPointer(), shortPath.data(), (DWORD) shortPath.size());
    if (length > 0 && length < shortPath.size())
        return toAnsi (shortPath.c_str());
    return {};
   #else
    return file.getFullPathName().toStdString();
   #endif
}

// demucs.cpp's loader believes whatever sizes the file gives it, so a damaged file could crash the
// host. Walk the whole file first: "dmc4", then per tensor its dimensions, name and f16 data.
bool modelFileIsSound (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk() || in.readInt() != 0x646d6334)
        return false;
    const auto total = in.getTotalLength();
    int tensors = 0;
    while (in.getPosition() < total)
    {
        if (total - in.getPosition() < 8)
            return false;
        const int dims = in.readInt();
        const int nameLength = in.readInt();
        if (dims < 0 || dims > 4 || nameLength < 1 || nameLength > 256)
            return false;
        juce::int64 elements = 1;
        for (int i = 0; i < dims; ++i)
        {
            const int size = in.readInt();
            if (size < 1 || size > (1 << 24))
                return false;
            elements *= size;
            if (elements > ((juce::int64) 1 << 28))
                return false;
        }
        const auto next = in.getPosition() + nameLength + elements * 2;
        if (next > total || ! in.setPosition (next))
            return false;
        ++tensors;
    }
    return tensors > 0;
}

// Separation threads shouldn't compete with the DAW's own threads.
void lowerWorkerPriority()
{
   #if JUCE_WINDOWS
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
   #endif
}

// Low-passes before downsampling so nothing above the new Nyquist folds back down.
juce::AudioBuffer<float> toRate (const juce::AudioBuffer<float>& in, double fromRate, double toRate)
{
    if (std::abs (fromRate - toRate) < 0.5)
        return in;
    juce::AudioBuffer<float> source (in);
    if (fromRate > toRate)
        for (int c = 0; c < source.getNumChannels(); ++c)
            for (int stage = 0; stage < 2; ++stage)
            {
                juce::IIRFilter lp;
                lp.setCoefficients (juce::IIRCoefficients::makeLowPass (fromRate, toRate * 0.46, 0.707));
                lp.processSamples (source.getWritePointer (c), source.getNumSamples());
            }
    return resampleAudio (source, fromRate / toRate);
}
} // namespace

juce::String stemName (int stem)
{
    switch (stem)
    {
        case stemVocals: return "Vocals";
        case stemDrums: return "Drums";
        case stemBass: return "Bass";
        case stemOther: return "Other";
        case stemInstrumental: return "Instrumental";
        default: return {};
    }
}

juce::File stemsModelFile()
{
    return lyricsModelFolder().getChildFile (kModelFileName);
}

juce::URL stemsModelUrl()
{
    return juce::URL ("https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/" + juce::String (kModelFileName));
}

bool stemsModelLooksComplete()
{
    return stemsModelFile().getSize() >= kModelBytes;
}

bool stemsSupportedOnThisCpu()
{
    return lyricsSupportedOnThisCpu(); // same AVX2/FMA build settings
}

bool downloadStemsModel (const StemsProgress& progress, juce::String& error)
{
    return downloadFile (stemsModelUrl(), stemsModelFile(), kModelBytes, "the stem separation model", progress, error);
}

int stemWorkerCount (juce::int64 numSamples)
{
    // Each thread holds about 1.5 GB while it works, on top of the audio buffers (about 24 floats
    // per sample at 44.1 kHz). Keep it all under half the RAM, and on Windows under three quarters
    // of what's free right now.
    juce::int64 budgetMB = juce::SystemStats::getMemorySizeInMegabytes() / 2;
   #if JUCE_WINDOWS
    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof (memory);
    if (GlobalMemoryStatusEx (&memory))
        budgetMB = juce::jmin (budgetMB, (juce::int64) (memory.ullAvailPhys / (1024 * 1024)) * 3 / 4);
   #endif
    budgetMB -= 300 + numSamples * 24 * (juce::int64) sizeof (float) / (1024 * 1024);
    const int byMemory = (int) juce::jmax ((juce::int64) 1, budgetMB / 1500);
    const int byCores = juce::jmax (1, juce::SystemStats::getNumPhysicalCpus() - 1);
    return juce::jlimit (1, 8, juce::jmin (byCores, byMemory));
}

StemsResult separateStems (const juce::AudioBuffer<float>& audio, double sampleRate, const StemsProgress& progress, int workers,
                           const juce::File& modelFile)
{
    StemsResult result;
    const int numSamples = audio.getNumSamples();
    const int channels = audio.getNumChannels();
    if (numSamples <= 0 || channels <= 0)
    {
        result.error = "There's no audio to separate";
        return result;
    }
    if (! stemsSupportedOnThisCpu())
    {
        result.error = "Stem separation needs a CPU with AVX2 (Intel 2013+ / AMD 2015+)";
        return result;
    }
    const auto model = modelFile == juce::File() ? stemsModelFile() : modelFile;
    if (! modelFileIsSound (model))
    {
        result.error = model.existsAsFile() ? "The stem separation model is damaged. Delete " + model.getFullPathName() + " and try again."
                                            : "The stem separation model isn't downloaded";
        return result;
    }
    const auto modelPath = pathForFopen (model);
    if (modelPath.empty())
    {
        result.error = "Can't open the stem model from " + model.getFullPathName()
                       + " (Windows can't spell that folder for it). Try a Windows account whose name uses plain letters.";
        return result;
    }

    juce::AudioBuffer<float> stereo (2, numSamples);
    stereo.copyFrom (0, 0, audio, 0, 0, numSamples);
    stereo.copyFrom (1, 0, audio, juce::jmin (1, channels - 1), 0, numSamples);
    const auto input = toRate (stereo, sampleRate, demucs::kSampleRate);
    const int n = input.getNumSamples();

    std::vector<float> out;
    const auto error = demucs::separate (modelPath, input.getReadPointer (0), input.getReadPointer (1), n,
                                         workers > 0 ? workers : stemWorkerCount (n), progress, lowerWorkerPriority, out);
    if (error == "cancelled")
    {
        result.cancelled = true;
        return result;
    }
    if (! error.empty())
    {
        result.error = juce::String (error);
        return result;
    }

    // The model's order is drums, bass, other, vocals.
    constexpr int kFromModel[] { stemDrums, stemBass, stemOther, stemVocals };
    for (int source = 0; source < demucs::kNumSources; ++source)
    {
        juce::AudioBuffer<float> stem (2, n);
        for (int c = 0; c < 2; ++c)
            stem.copyFrom (c, 0, out.data() + ((size_t) (source * 2 + c)) * (size_t) n, n);
        stem = toRate (stem, demucs::kSampleRate, sampleRate);

        auto& dest = result.stems[(size_t) kFromModel[source]];
        dest.setSize (channels, numSamples);
        dest.clear();
        const int copy = juce::jmin (numSamples, stem.getNumSamples());
        if (channels == 1)
        {
            dest.addFrom (0, 0, stem, 0, 0, copy, 0.5f);
            dest.addFrom (0, 0, stem, 1, 0, copy, 0.5f);
        }
        else
        {
            for (int c = 0; c < channels; ++c)
                dest.copyFrom (c, 0, stem, juce::jmin (c, 1), 0, copy);
        }
    }

    auto& instrumental = result.stems[stemInstrumental];
    instrumental.makeCopyOf (result.stems[stemDrums]);
    for (int c = 0; c < channels; ++c)
    {
        instrumental.addFrom (c, 0, result.stems[stemBass], c, 0, numSamples);
        instrumental.addFrom (c, 0, result.stems[stemOther], c, 0, numSamples);
    }
    return result;
}

} // namespace choplab
