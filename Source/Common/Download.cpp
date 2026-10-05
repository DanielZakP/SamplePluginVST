#include "Download.h"

namespace choplab
{

bool downloadFile (const juce::URL& url, const juce::File& target, juce::int64 approxBytes, const juce::String& what,
                   const std::function<bool (float)>& progress, juce::String& error)
{
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    if (! target.getParentDirectory().createDirectory())
    {
        error = "Couldn't create " + target.getParentDirectory().getFullPathName();
        return false;
    }

    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (30000)
                                             .withNumRedirectsToFollow (10)
                                             .withStatusCode (&status));
    if (stream == nullptr || status >= 400)
    {
        error = "Couldn't download " + what + " from " + url.getDomain() + (status >= 400 ? " (HTTP " + juce::String (status) + ")" : juce::String())
                + ". Check your internet connection.";
        return false;
    }

    const auto total = stream->getTotalLength();
    part.deleteFile();
    juce::int64 received = 0;
    {
        juce::FileOutputStream out (part);
        if (! out.openedOk())
        {
            error = "Couldn't write " + part.getFullPathName();
            return false;
        }

        juce::HeapBlock<char> buffer (1 << 16);
        for (;;)
        {
            const int n = stream->read (buffer, 1 << 16);
            if (n <= 0)
                break;
            if (! out.write (buffer, (size_t) n))
            {
                error = "Ran out of disk space while downloading " + what;
                part.deleteFile();
                return false;
            }
            received += n;
            const float fraction = (float) received / (float) (total > 0 ? total : approxBytes);
            if (progress && ! progress (juce::jlimit (0.0f, 1.0f, fraction)))
            {
                out.flush();
                part.deleteFile();
                error = "Download cancelled";
                return false;
            }
        }
        out.flush();
    }

    if ((total > 0 && received != total) || received < approxBytes * 8 / 10)
    {
        part.deleteFile();
        error = "The download of " + what + " was cut off. Try again.";
        return false;
    }

    target.deleteFile();
    if (! part.moveFileTo (target))
    {
        error = "Couldn't save " + target.getFullPathName();
        return false;
    }
    return true;
}

} // namespace choplab
