// VorbisSource — an Ogg Vorbis reader built on libvorbisfile.
//
// Compiled only when DAW_VORBIS found the library (CMakeLists.txt defines
// DAW_HAVE_VORBIS). libvorbisfile already does the Ogg paging, the decoder
// setup and the sample-accurate seek; this class is the thin IAudioSource
// adapter: convert each decoded block to interleaved stereo float, clamp the
// frame count against the file, and hand it out in the same 8192-frame blocks
// WavSource uses.
//
// Kit-free (std C++ + libvorbisfile) so it builds and unit-tests on the host.
#pragma once

#include "IAudioSource.h"

#include <vorbis/vorbisfile.h>

#include <string>
#include <vector>

namespace daw {

class VorbisSource : public IAudioSource {
public:
    VorbisSource() = default;
    ~VorbisSource() override;

    VorbisSource(const VorbisSource&) = delete;
    VorbisSource& operator=(const VorbisSource&) = delete;

    // Open the file and read its identification header. Returns true on
    // success.
    bool Open(const std::string& path);

    // --- IAudioSource ---
    bool    IsValid() const override { return fValid; }
    float   FrameRate() const override { return fSampleRate; }
    int     SourceChannels() const override { return fChannels; }
    int64_t TotalFrames() const override { return fTotalFrames; }
    bool    Seek(int64_t frame) override;
    bool    ReadChunk(const float** outStereo, size_t* outFrames) override;

private:
    OggVorbis_File fVf;
    bool    fOpen        = false;   // ov_fopen succeeded (ov_clear is needed)
    bool    fValid       = false;
    bool    fEof         = false;
    bool    fPastEnd     = false;   // cursor parked on/past the last frame
    int     fChannels    = 0;
    float   fSampleRate  = 0.0f;
    int64_t fTotalFrames = 0;

    // Decoded planar floats hand out per channel; libvorbisfile owns that
    // buffer until the next ov_read_float, so it is copied into fStereo before
    // returning (the IAudioSource contract says the block stays valid until
    // the next ReadChunk).
    std::vector<float> fStereo;
};

} // namespace daw
