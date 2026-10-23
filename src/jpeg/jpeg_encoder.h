// jpeg_encoder.h - baseline JPEG encoder for the 240x240 commute canvas.
//
// Why we need one at all: the SmallTV's stock firmware displays JPEGs from its
// LittleFS image directory. The whole architecture rests on handing it a photo
// it believes came from a phone, so the last step of every render is an encode.
//
// Design constraints, in the order they mattered:
//
//   - No dynamic allocation anywhere. The caller owns the output buffer. On the
//     ESP32 the canvas is already 115 KB of the heap; the encoder must not add
//     an unpredictable second allocation on top of it.
//   - No intermediate image. Blocks are pulled straight out of the RGB565
//     canvas as they are needed, so working memory is a few hundred bytes of
//     stack regardless of image size.
//   - Identical output on the host and the ESP32, so the previews reviewed on
//     a laptop are the bytes the device actually sends.
//   - No libraries. This compiles with the same bare g++ as the rest of core/.
//
// Baseline sequential DCT, standard Annex K quantisation and Huffman tables,
// JFIF header. Deliberately not progressive: the SmallTV's decoder is an
// ESP8266 and baseline is the safe assumption until the probe says otherwise.
#pragma once
#include <cstddef>
#include <cstdint>

namespace jpeg {

// Encoded bytes are handed out as they are produced rather than assembled in
// one place, so a future version can stream straight into an HTTP socket
// without ever holding the whole file.
struct Sink {
    virtual ~Sink() = default;
    virtual bool write(const uint8_t* data, size_t len) = 0;
};

// Writes into a fixed caller-owned buffer. Overflow is reported, never
// asserted and never a buffer overrun: a frame too large for the budget is a
// recoverable condition (retry at lower quality), not a crash.
class BufferSink final : public Sink {
public:
    BufferSink(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    bool write(const uint8_t* data, size_t len) override;
    void reset() { len_ = 0; overflow_ = false; }

    const uint8_t* data() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

private:
    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool overflow_ = false;
};

// Counts bytes without storing them. Used to size a buffer before committing
// to one, and by the tests.
class CountingSink final : public Sink {
public:
    bool write(const uint8_t*, size_t len) override { len_ += len; return true; }
    size_t size() const { return len_; }

private:
    size_t len_ = 0;
};

// Supplies the image one horizontal strip at a time.
//
// This is what lets the ESP32 encode a 240x240 frame it could never hold. The
// encoder walks MCU rows top to bottom and never looks back, so it only ever
// needs the 8 or 16 rows it is currently working on. Measured on the board:
// a full frame needs 115,200 bytes and malloc fails at 110,580; a 16-row band
// needs 7,680.
struct BandSource {
    virtual ~BandSource() = default;

    // Make rows [y, y + rows) available and return a pointer to them, laid out
    // row-major, `width` pixels per row. `rows` never exceeds what remains of
    // the image. Returns nullptr on failure. The pointer must stay valid until
    // the next call -- a source is free to reuse one buffer.
    virtual const uint16_t* band(int y, int rows) = 0;
};

enum class Sampling : uint8_t {
    S444,  // no chroma subsampling -- correct default for a UI screen
    S420,  // half chroma in both axes; ~30% smaller, softer coloured text
};

struct Options {
    // 1..100, IJG scale.
    //
    // 90 was measured, not chosen. Against the twelve preview scenarios, on
    // the RGB565 lattice the panel actually shows:
    //
    //     quality   4:4:4            4:2:0
    //        85     17.3 KB 33.5 dB  13.7 KB 29.1 dB
    //        90     20.7 KB 36.0 dB  16.2 KB 29.7 dB
    //        95     27.5 KB 38.1 dB  21.0 KB 30.1 dB
    //
    // Two things fall out of that table. Chroma subsampling plateaus near
    // 30 dB however much quality you spend -- the loss is structural, not a
    // quantiser setting -- which is why 4:4:4 is the default. And above 90 the
    // 4:4:4 curve flattens while the file keeps growing, so 90 sits at the
    // knee: past it we would be paying bytes for nothing visible.
    int quality = 90;
    Sampling sampling = Sampling::S444;
};

// Encodes an RGB565 image. `px` is row-major, `width * height` entries.
// Returns false on a bad argument or a sink that refused a write.
//
// 4:4:4 is the default because this image is not a photograph. It is 12 px
// coloured text on a near-black panel, which is the exact content chroma
// subsampling damages most: the green "on time" and red "CANCELLED" rows lose
// their edges before the luminance shows any loss at all.
bool encodeRgb565(const uint16_t* px, int width, int height,
                  const Options& opt, Sink& sink);

// Same encoder, fed strip by strip. Produces byte-identical output to the
// whole-frame overload for the same pixels -- asserted in the tests, because
// that equivalence is the entire reason this path is safe to adopt.
bool encodeRgb565(BandSource& src, int width, int height,
                  const Options& opt, Sink& sink);

// Encodes into `sink`, stepping quality down until the result fits `maxBytes`.
// Returns the quality actually used, or -1 if even `minQuality` did not fit.
//
// This exists because the ESP32 has a fixed upload buffer and a frame that
// does not fit must degrade rather than fail: a slightly softer screen is
// always better than no screen. `sink` is reset between attempts.
int encodeRgb565WithBudget(const uint16_t* px, int width, int height,
                           const Options& opt, size_t maxBytes,
                           int minQuality, BufferSink& sink);

// Banded equivalent. Note each quality attempt re-reads the source, so a
// source that renders on demand will render again per attempt -- cheap for
// this screen, and the alternative is holding a frame we cannot allocate.
int encodeRgb565WithBudget(BandSource& src, int width, int height,
                           const Options& opt, size_t maxBytes,
                           int minQuality, BufferSink& sink);

} // namespace jpeg
