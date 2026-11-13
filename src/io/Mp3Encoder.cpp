#include "io/Mp3Encoder.h"
#include <lame.h>

namespace aligna
{
struct Mp3Encoder::Impl
{
    lame_global_flags* gfp = nullptr;
    std::unique_ptr<juce::FileOutputStream> out;
    juce::int64 tagOffset = 0; // where the Xing/LAME frame placeholder begins (after ID3v2)
    std::vector<unsigned char> scratch;
};

int Mp3Encoder::outputRateFor(double inRate)
{
    const int r = (int) std::lround(inRate);
    return (r == 32000 || r == 44100 || r == 48000) ? r : 44100;
}

bool Mp3Encoder::writeBytes(const unsigned char* data, int n, juce::String& error)
{
    if (n <= 0) return true;
    if (! impl->out->write(data, (size_t) n))
    {
        error = "Could not write to the MP3 file.";
        return false;
    }
    return true;
}

bool Mp3Encoder::open(const juce::File& file, double inRate, int kbps, const juce::String& title, juce::String& error)
{
    close();
    if (kbps != 192 && kbps != 256 && kbps != 320)
    {
        error = "Unsupported MP3 bitrate: " + juce::String(kbps) + " kbps.";
        return false;
    }
    if (inRate < 8000.0 || inRate > 384000.0)
    {
        error = "Unsupported sample rate for MP3 export.";
        return false;
    }

    auto im = std::make_unique<Impl>();
    im->gfp = lame_init();
    if (im->gfp == nullptr)
    {
        error = "Could not initialise the MP3 encoder.";
        return false;
    }
    impl = std::move(im);
    auto* g = impl->gfp;

    lame_set_in_samplerate(g, (int) std::lround(inRate));
    lame_set_out_samplerate(g, outputRateFor(inRate)); // LAME resamples internally when different
    lame_set_num_channels(g, 2);
    lame_set_brate(g, kbps);
    lame_set_VBR(g, vbr_off);
    lame_set_quality(g, 2);
    lame_set_write_id3tag_automatic(g, 0);

    id3tag_init(g);
    id3tag_v2_only(g);
    id3tag_add_v2(g);
    {
        const juce::String t = title.isEmpty() ? juce::String("Untitled") : title;
        bool latin1 = true;
        for (auto p = t.getCharPointer(); *p != 0; ++p)
            if ((juce::juce_wchar) *p > 255) latin1 = false;
        if (latin1)
        {
            std::string bytes; // ISO-8859-1, as LAME expects for id3tag_set_title
            for (auto p = t.getCharPointer(); *p != 0; ++p) bytes.push_back((char) (unsigned char) *p);
            id3tag_set_title(g, bytes.c_str());
        }
        else
        {
            // Not representable in Latin-1: store as UTF-16 with BOM.
            std::vector<unsigned short> u16 { 0xFEFF };
            for (auto p = t.toUTF16(); *p != 0; ++p) u16.push_back((unsigned short) *p);
            u16.push_back(0);
            id3tag_set_textinfo_utf16(g, "TIT2", u16.data());
        }
    }

    if (lame_init_params(g) < 0)
    {
        error = "Could not configure the MP3 encoder.";
        close();
        return false;
    }

    impl->out = std::make_unique<juce::FileOutputStream>(file);
    if (impl->out->failedToOpen())
    {
        error = "Could not open " + file.getFullPathName() + " for writing.";
        close();
        return false;
    }
    impl->out->setPosition(0);
    impl->out->truncate();

    const size_t idSize = lame_get_id3v2_tag(g, nullptr, 0);
    if (idSize > 0)
    {
        std::vector<unsigned char> buf(idSize);
        const size_t got = lame_get_id3v2_tag(g, buf.data(), buf.size());
        if (! writeBytes(buf.data(), (int) got, error)) { close(); return false; }
    }
    impl->tagOffset = impl->out->getPosition();

    // LAME emits a placeholder Xing/LAME frame as the first audio frame; finish() overwrites it at tagOffset.
    return true;
}

bool Mp3Encoder::write(const juce::AudioBuffer<float>& stereo, int numSamples, juce::String& error)
{
    if (impl == nullptr || impl->gfp == nullptr || impl->out == nullptr)
    {
        error = "MP3 encoder is not open.";
        return false;
    }
    if (numSamples <= 0) return true;
    if (stereo.getNumChannels() < 2 || stereo.getNumSamples() < numSamples)
    {
        error = "Invalid audio block for MP3 encoding.";
        return false;
    }

    impl->scratch.resize((size_t) (1.25 * numSamples + 7200));
    const int n = lame_encode_buffer_ieee_float(impl->gfp, stereo.getReadPointer(0), stereo.getReadPointer(1),
                                                numSamples, impl->scratch.data(), (int) impl->scratch.size());
    if (n < 0)
    {
        error = "MP3 encoding failed (code " + juce::String(n) + ").";
        return false;
    }
    return writeBytes(impl->scratch.data(), n, error);
}

bool Mp3Encoder::finish(juce::String& error)
{
    if (impl == nullptr || impl->gfp == nullptr || impl->out == nullptr)
    {
        error = "MP3 encoder is not open.";
        return false;
    }
    unsigned char tail[7200];
    const int n = lame_encode_flush(impl->gfp, tail, (int) sizeof(tail));
    if (n < 0)
    {
        error = "MP3 flush failed (code " + juce::String(n) + ").";
        return false;
    }
    if (! writeBytes(tail, n, error)) return false;

    // Rewrite the Xing/LAME info frame at the start of the audio data.
    const size_t tagSize = lame_get_lametag_frame(impl->gfp, nullptr, 0);
    if (tagSize > 0)
    {
        std::vector<unsigned char> buf(tagSize);
        const size_t got = lame_get_lametag_frame(impl->gfp, buf.data(), buf.size());
        if (got > 0)
        {
            if (! impl->out->setPosition(impl->tagOffset)
                || ! writeBytes(buf.data(), (int) got, error))
            {
                if (error.isEmpty()) error = "Could not rewrite the MP3 header.";
                return false;
            }
        }
    }
    impl->out->flush();
    const bool ok = impl->out->getStatus().wasOk();
    if (! ok) error = "Could not finish the MP3 file.";
    close();
    return ok;
}

void Mp3Encoder::close()
{
    if (impl == nullptr) return;
    impl->out.reset();
    if (impl->gfp != nullptr) lame_close(impl->gfp);
    impl.reset();
}

Mp3Encoder::Mp3Encoder() = default;
Mp3Encoder::~Mp3Encoder() { close(); }
}
