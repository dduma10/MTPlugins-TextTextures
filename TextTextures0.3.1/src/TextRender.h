// TextRender.h: renders sign text into an RGBA texture. Glyph coverage comes from GDI (Windows); outline,
// compositing and the preview are plain C++.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tt
{
struct TextStyle
{
    std::wstring text;              // lines separated by '\n'
    std::wstring font = L"Arial";
    int fontPx = 96;                // glyph cell height in texture pixels (ignored by autoFit)
    bool autoFit = true;            // largest size that fits inside the padding
    bool bold = true, italic = false, underline = false;
    int align = 1;                  // 0 left, 1 centre, 2 right
    int valign = 1;                 // 0 top, 1 middle, 2 bottom
    int padding = 32;               // pixels kept clear on every side
    int outlinePx = 0;              // outline thickness in pixels, 0 = none
    uint32_t textArgb = 0xFFFFFFFF; // 0xAARRGGBB
    uint32_t outlineArgb = 0xFF000000;
    uint32_t backgroundArgb = 0xFF202020; // alpha 0 = transparent background
};

struct Image
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;      // straight alpha
};

// Glyph coverage (0..255) of the style's text at width x height. usedPx receives the font size drawn.
// Windows only; elsewhere returns false.
bool RasterizeCoverage(const TextStyle& style, int width, int height, std::vector<uint8_t>& coverage, int& usedPx,
                       std::string& error);

// Grows coverage outward by `radius` pixels (an octagon, close to round), keeping soft edges.
std::vector<uint8_t> DilateCoverage(const std::vector<uint8_t>& coverage, int width, int height, int radius);

// background, then outline, then text, "over" in straight alpha. Fully transparent pixels take the colour of
// the nearest layer so mipmaps and filtering do not pull dark fringes into the letters.
Image Composite(const TextStyle& style, const std::vector<uint8_t>& textCoverage, const std::vector<uint8_t>& outlineCoverage,
                int width, int height);

enum class GlowMode
{
    Off = 0,
    Letters = 1,           // only the letters glow
    LettersOutline = 2,    // letters and their outline
    WholeSign = 3,         // everything (a backlit lightbox sign)
};
// The glow (emissive) map: what glows in its colour, everything else black. Opaque.
Image GlowMap(const TextStyle& style, const std::vector<uint8_t>& textCoverage, const std::vector<uint8_t>& outlineCoverage,
              int width, int height, GlowMode mode);

// Everything above in one call.
bool RenderTexture(const TextStyle& style, int width, int height, Image& out, int& usedPx, std::string& error);
// The texture and, when mode is not Off, its glow map (same size).
bool RenderTextureAndGlow(const TextStyle& style, int width, int height, GlowMode mode, Image& out, Image& glow, int& usedPx,
                          std::string& error);

// The image over a checkerboard, scaled down (area average) to fit maxW x maxH. Opaque.
Image MakePreview(const Image& img, int maxW, int maxH);

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
} // namespace tt
