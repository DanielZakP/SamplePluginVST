#include "Demucs.h"

#include "model.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace choplab::demucs
{
namespace
{
static_assert (demucscpp::SUPPORTED_SAMPLE_RATE == kSampleRate);

// The same segmenting as demucs itself (demucs.cpp's model_apply.cpp, Python's apply_model with
// split=True, shifts=1), except that the segments run in parallel and the shift is fixed so the
// result is the same every time.
const int kSegment = (int) (demucscpp::SEGMENT_LEN_SECS * demucscpp::SUPPORTED_SAMPLE_RATE);
const int kStride = (int) ((1 - demucscpp::OVERLAP) * kSegment);
const int kMaxShift = (int) (demucscpp::MAX_SHIFT_SECS * demucscpp::SUPPORTED_SAMPLE_RATE);
const int kLead = kMaxShift - kMaxShift / 2; // zeros in front of the audio

struct Cancelled
{
};

int shiftedLength (int numSamples)
{
    return numSamples + kLead;
}
} // namespace

int segmentCount (int numSamples)
{
    const int length = shiftedLength (numSamples);
    return (length + kStride - 1) / kStride;
}

std::string separate (const std::string& modelPath, const float* left, const float* right, int numSamples, int workers,
                      const std::function<bool (float)>& progress, const std::function<void()>& onWorkerStart,
                      std::vector<float>& out)
{
    if (numSamples <= 0)
        return "There's no audio to separate";

    const size_t n = (size_t) numSamples;
    out.assign ((size_t) kNumSources * 2 * n, 0.0f);

    // Normalise by the mean and spread of the mono mix, as demucs does.
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += 0.5 * ((double) left[i] + (double) right[i]);
    const double mean = sum / (double) n;
    double squares = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double d = 0.5 * ((double) left[i] + (double) right[i]) - mean;
        squares += d * d;
    }
    const double spread = n > 1 ? std::sqrt (squares / (double) (n - 1)) : 0.0;
    if (spread < 1.0e-7)
        return {}; // silence: every stem is silent too

    auto model = std::make_unique<demucscpp::demucs_model>();
    if (! demucscpp::load_demucs_model (modelPath, model.get()))
        return "Couldn't load the stem separation model. Delete it from the Models folder and try again.";
    if (! model->is_4sources)
        return "The stem separation model in the Models folder is the wrong kind (needs the 4-stem htdemucs).";

    const int length = shiftedLength (numSamples);
    Eigen::MatrixXf shifted = Eigen::MatrixXf::Zero (2, length);
    for (int i = 0; i < numSamples; ++i)
    {
        shifted (0, kLead + i) = (float) (((double) left[i] - mean) / spread);
        shifted (1, kLead + i) = (float) (((double) right[i] - mean) / spread);
    }

    std::vector<float> weight ((size_t) kSegment, 0.0f);
    const int half = kSegment / 2;
    for (int k = 0; k < half; ++k)
    {
        weight[(size_t) k] = (float) (k + 1) / (float) half;
        weight[(size_t) (kSegment - 1 - k)] = (float) (k + 1) / (float) half;
    }
    std::vector<float> weightSum (n, 0.0f);

    const int total = segmentCount (numSamples);
    workers = std::clamp (workers, 1, total);

    Eigen::initParallel();
    std::atomic<int> nextSegment { 0 }, finishedSegments { 0 }, runningWorkers { workers };
    std::atomic<bool> stop { false };
    std::vector<std::atomic<float>> partial ((size_t) workers);
    for (auto& p : partial)
        p = 0.0f;
    std::mutex outLock, doneLock;
    std::condition_variable done;
    std::string error;

    auto work = [&] (int w)
    {
        if (onWorkerStart)
            onWorkerStart();
        try
        {
            demucscpp::demucs_segment_buffers buffers (2, kSegment, kNumSources);
            demucscpp::stft_buffers stftBuffers (buffers.padded_segment_samples);
            demucscpp::ProgressCallback cb = [&] (float p, const std::string&)
            {
                partial[(size_t) w] = std::clamp (p, 0.0f, 1.0f);
                if (stop)
                    throw Cancelled {};
            };

            for (;;)
            {
                const int index = nextSegment++;
                if (index >= total || stop)
                    break;

                const int offset = index * kStride;
                const int chunk = std::min (kSegment, length - offset);
                const int pad = (kSegment - chunk) / 2;
                buffers.mix.setZero();
                buffers.mix.block (0, pad, 2, chunk) = shifted.block (0, offset, 2, chunk);

                partial[(size_t) w] = 0.0f;
                demucscpp::model_inference (*model, buffers, stftBuffers, cb, 0.0f, 1.0f);

                {
                    const std::lock_guard<std::mutex> lock (outLock);
                    const int first = std::max (0, kLead - offset);
                    const int last = std::min (chunk, kLead + numSamples - offset);
                    for (int s = 0; s < kNumSources; ++s)
                        for (int c = 0; c < 2; ++c)
                        {
                            float* dest = out.data() + ((size_t) (s * 2 + c)) * n;
                            for (int k = first; k < last; ++k)
                                dest[offset + k - kLead] += weight[(size_t) k] * buffers.targets_out (s, c, k + pad);
                        }
                    for (int k = first; k < last; ++k)
                        weightSum[(size_t) (offset + k - kLead)] += weight[(size_t) k];
                }
                partial[(size_t) w] = 0.0f;
                ++finishedSegments;
            }
        }
        catch (const Cancelled&)
        {
        }
        catch (const std::bad_alloc&)
        {
            const std::lock_guard<std::mutex> lock (outLock);
            error = "Ran out of memory separating the stems. Close other programs, or separate a shorter sample.";
            stop = true;
        }
        catch (const std::exception& e)
        {
            const std::lock_guard<std::mutex> lock (outLock);
            error = std::string ("Stem separation failed: ") + e.what();
            stop = true;
        }

        {
            const std::lock_guard<std::mutex> lock (doneLock);
            --runningWorkers;
        }
        done.notify_all();
    };

    std::vector<std::thread> threads;
    try
    {
        for (int w = 0; w < workers; ++w)
            threads.emplace_back (work, w);
    }
    catch (const std::system_error&)
    {
        // Couldn't start every thread; the ones that did start share the work.
        runningWorkers -= workers - (int) threads.size();
        if (threads.empty())
            return "Couldn't start the stem separation";
    }

    {
        std::unique_lock<std::mutex> lock (doneLock);
        while (runningWorkers > 0)
        {
            done.wait_for (lock, std::chrono::milliseconds (100));
            float fraction = (float) finishedSegments;
            for (auto& p : partial)
                fraction += p;
            if (progress && ! progress (std::clamp (fraction / (float) total, 0.0f, 1.0f)))
                stop = true;
        }
    }
    for (auto& t : threads)
        t.join();

    if (! error.empty())
        return error;
    if (stop || finishedSegments < total)
        return "cancelled";

    for (size_t i = 0; i < n; ++i)
    {
        const float scale = weightSum[i] > 0.0f ? (float) spread / weightSum[i] : 0.0f;
        for (int sc = 0; sc < kNumSources * 2; ++sc)
        {
            auto& v = out[(size_t) sc * n + i];
            v = v * scale + (float) mean;
        }
    }
    return {};
}

} // namespace choplab::demucs
