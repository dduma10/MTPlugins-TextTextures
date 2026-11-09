// TextRender.cpp: see TextRender.h.
#include "TextRender.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tt
{
std::wstring Utf8ToWide(const std::string& s)
{
    std::wstring out;
    size_t i = 0;
    while (i < s.size())
    {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD;
        int extra = 0;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
        ++i;
        for (int k = 0; k < extra; ++k)
        {
            if (i >= s.size() || ((unsigned char)s[i] >> 6) != 2) { cp = 0xFFFD; break; }
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
            ++i;
        }
        if (sizeof(wchar_t) == 2 && cp >= 0x10000)
        {
            cp -= 0x10000;
            out += wchar_t(0xD800 + (cp >> 10));
            out += wchar_t(0xDC00 + (cp & 0x3FF));
        }
        else
            out += wchar_t(cp);
    }
    return out;
}

std::string WideToUtf8(const std::wstring& w)
{
    std::string out;
    for (size_t i = 0; i < w.size(); ++i)
    {
        uint32_t cp = (uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size())
        {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}

#ifdef _WIN32
namespace
{
struct Gdi
{
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    bool Open(int width, int height)
    {
        w = width;
        h = height;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return false;
        oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)width * height * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        return true;
    }
    ~Gdi()
    {
        if (dc && oldBmp) SelectObject(dc, oldBmp);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
};

HFONT MakeFont(const TextStyle& s, int px)
{
    LOGFONTW lf{};
    lf.lfHeight = -px;
    lf.lfWeight = s.bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = s.italic ? TRUE : FALSE;
    lf.lfUnderline = s.underline ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH;
    wcsncpy(lf.lfFaceName, s.font.c_str(), LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

// DrawText wants CR LF; the text box gives LF.
std::wstring ForDrawText(const std::wstring& t)
{
    std::wstring out;
    for (size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] == L'\r') continue;
        if (t[i] == L'\n') out += L"\r\n";
        else out += t[i];
    }
    return out;
}

UINT Flags(const TextStyle& s)
{
    UINT f = DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS;
    f |= s.align == 0 ? DT_LEFT : s.align == 2 ? DT_RIGHT : DT_CENTER;
    return f;
}

// Size of the laid-out text at `px` inside a box `availW` wide.
SIZE Measure(HDC dc, const TextStyle& s, const std::wstring& text, int px, int availW)
{
    HFONT f = MakeFont(s, px);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, availW, 0};
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, Flags(s) | DT_CALCRECT);
    SelectObject(dc, old);
    DeleteObject(f);
    return SIZE{r.right - r.left, r.bottom - r.top};
}
} // namespace

bool RasterizeCoverage(const TextStyle& style, int width, int height, std::vector<uint8_t>& coverage, int& usedPx,
                       std::string& error)
{
    coverage.assign((size_t)width * height, 0);
    usedPx = 0;
    std::wstring text = ForDrawText(style.text);
    bool blank = text.find_first_not_of(L" \t\r\n") == std::wstring::npos;
    if (blank) return true;
    // Draw at twice the size and average down when that stays under 16 MB of bitmap: smoother small text.
    int ss = (size_t)width * height <= (size_t)1024 * 1024 ? 2 : 1;
    int W = width * ss, H = height * ss;
    Gdi g;
    if (!g.Open(W, H))
    {
        error = "Windows could not create a drawing surface of that size.";
        return false;
    }
    int pad = std::max(0, std::min(style.padding, std::min(width, height) / 2 - 1)) * ss;
    int availW = std::max(1, W - 2 * pad), availH = std::max(1, H - 2 * pad);
    int px = std::max(2, style.fontPx * ss);
    if (style.autoFit)
    {
        int lo = 2, hi = std::max(2, availH);
        while (lo < hi)
        {
            int mid = (lo + hi + 1) / 2;
            SIZE sz = Measure(g.dc, style, text, mid, availW);
            if (sz.cx <= availW && sz.cy <= availH) lo = mid;
            else hi = mid - 1;
        }
        px = lo;
    }
    SIZE sz = Measure(g.dc, style, text, px, availW);
    int top = pad;
    if (style.valign == 1) top = pad + (availH - sz.cy) / 2;
    else if (style.valign == 2) top = pad + availH - sz.cy;
    RECT r{pad, top, pad + availW, top + sz.cy};
    HFONT f = MakeFont(style, px);
    HGDIOBJ old = SelectObject(g.dc, f);
    DrawTextW(g.dc, text.c_str(), (int)text.size(), &r, Flags(style));
    SelectObject(g.dc, old);
    DeleteObject(f);
    GdiFlush();
    const uint8_t* src = static_cast<const uint8_t*>(g.bits);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            int sum = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx)
                {
                    const uint8_t* p = src + ((size_t)(y * ss + sy) * W + (x * ss + sx)) * 4;
                    sum += std::max(p[0], std::max(p[1], p[2]));
                }
            coverage[(size_t)y * width + x] = uint8_t(sum / (ss * ss));
        }
    usedPx = px / ss;
    return true;
}
#else
bool RasterizeCoverage(const TextStyle&, int width, int height, std::vector<uint8_t>& coverage, int& usedPx, std::string& error)
{
    coverage.assign((size_t)width * height, 0);
    usedPx = 0;
    error = "Text rasterisation needs Windows.";
    return false;
}
#endif

std::vector<uint8_t> DilateCoverage(const std::vector<uint8_t>& src, int w, int h, int radius)
{
    std::vector<uint8_t> a = src, b(src.size());
    for (int step = 0; step < radius; ++step)
    {
        // Alternate a plus-shaped and a square 3x3 step: the result grows as an octagon.
        bool square = step % 2 == 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                uint8_t m = a[(size_t)y * w + x];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (!dx && !dy) continue;
                        if (!square && dx && dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        m = std::max(m, a[(size_t)ny * w + nx]);
                    }
                b[(size_t)y * w + x] = m;
            }
        a.swap(b);
    }
    return a;
}

namespace
{
struct Px
{
    double r, g, b, a;
};
Px FromArgb(uint32_t c, double coverage)
{
    return {((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, ((c >> 24) & 255) / 255.0 * coverage};
}
Px Over(Px s, Px d)
{
    double a = s.a + d.a * (1 - s.a);
    if (a <= 0) return {s.r, s.g, s.b, 0};
    return {(s.r * s.a + d.r * d.a * (1 - s.a)) / a, (s.g * s.a + d.g * d.a * (1 - s.a)) / a, (s.b * s.a + d.b * d.a * (1 - s.a)) / a, a};
}
uint8_t To8(double v) { return uint8_t(std::lround(std::min(1.0, std::max(0.0, v)) * 255)); }
} // namespace

Image Composite(const TextStyle& s, const std::vector<uint8_t>& text, const std::vector<uint8_t>& outline, int w, int h)
{
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize((size_t)w * h * 4);
    bool hasOutline = s.outlinePx > 0 && outline.size() == text.size();
    // Colour for pixels nothing covers: the background's when it shows, else the layer that meets the letters.
    uint32_t bleed = ((s.backgroundArgb >> 24) & 255) ? s.backgroundArgb : hasOutline ? s.outlineArgb : s.textArgb;
    Px bleedPx = FromArgb(bleed, 1);
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        Px p = FromArgb(s.backgroundArgb, 1);
        if (hasOutline) p = Over(FromArgb(s.outlineArgb, outline[i] / 255.0), p);
        p = Over(FromArgb(s.textArgb, text[i] / 255.0), p);
        if (p.a <= 0) p = {bleedPx.r, bleedPx.g, bleedPx.b, 0};
        uint8_t* o = &img.rgba[i * 4];
        o[0] = To8(p.r);
        o[1] = To8(p.g);
        o[2] = To8(p.b);
        o[3] = To8(p.a);
    }
    return img;
}

bool RenderTexture(const TextStyle& style, int width, int height, Image& out, int& usedPx, std::string& error)
{
    std::vector<uint8_t> cov;
    if (!RasterizeCoverage(style, width, height, cov, usedPx, error)) return false;
    std::vector<uint8_t> outline;
    if (style.outlinePx > 0) outline = DilateCoverage(cov, width, height, std::min(style.outlinePx, 64));
    out = Composite(style, cov, outline, width, height);
    return true;
}

Image GlowMap(const TextStyle& s, const std::vector<uint8_t>& text, const std::vector<uint8_t>& outline, int w, int h, GlowMode mode)
{
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign((size_t)w * h * 4, 0);
    if (mode == GlowMode::Off) return img;
    bool hasOutline = s.outlinePx > 0 && outline.size() == text.size();
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        Px p = mode == GlowMode::WholeSign ? FromArgb(s.backgroundArgb, 1) : Px{0, 0, 0, 0};
        if (hasOutline && mode != GlowMode::Letters) p = Over(FromArgb(s.outlineArgb, outline[i] / 255.0), p);
        else if (hasOutline) p = Over(Px{0, 0, 0, outline[i] / 255.0}, p); // an outline that does not glow hides the glow under it
        p = Over(FromArgb(s.textArgb, text[i] / 255.0), p);
        uint8_t* o = &img.rgba[i * 4];
        o[0] = To8(p.r * p.a); // over black
        o[1] = To8(p.g * p.a);
        o[2] = To8(p.b * p.a);
        o[3] = 255;
    }
    return img;
}

bool RenderTextureAndGlow(const TextStyle& style, int width, int height, GlowMode mode, Image& out, Image& glow, int& usedPx, std::string& error)
{
    std::vector<uint8_t> cov;
    if (!RasterizeCoverage(style, width, height, cov, usedPx, error)) return false;
    std::vector<uint8_t> outline;
    if (style.outlinePx > 0) outline = DilateCoverage(cov, width, height, std::min(style.outlinePx, 64));
    out = Composite(style, cov, outline, width, height);
    glow = mode == GlowMode::Off ? Image{} : GlowMap(style, cov, outline, width, height, mode);
    return true;
}

Image MakePreview(const Image& img, int maxW, int maxH)
{
    Image p;
    if (img.width <= 0 || img.height <= 0) return p;
    double scale = std::min(1.0, std::min(double(maxW) / img.width, double(maxH) / img.height));
    p.width = std::max(1, (int)std::lround(img.width * scale));
    p.height = std::max(1, (int)std::lround(img.height * scale));
    p.rgba.resize((size_t)p.width * p.height * 4);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
        {
            int x0 = (int)((double)x * img.width / p.width), x1 = std::max(x0 + 1, (int)((double)(x + 1) * img.width / p.width));
            int y0 = (int)((double)y * img.height / p.height), y1 = std::max(y0 + 1, (int)((double)(y + 1) * img.height / p.height));
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < y1 && sy < img.height; ++sy)
                for (int sx = x0; sx < x1 && sx < img.width; ++sx)
                {
                    const uint8_t* s = &img.rgba[((size_t)sy * img.width + sx) * 4];
                    double al = s[3] / 255.0;
                    r += s[0] * al;
                    g += s[1] * al;
                    b += s[2] * al;
                    a += al;
                    ++n;
                }
            double checker = ((x / 8 + y / 8) % 2) ? 0x9A : 0x6E;
            uint8_t* o = &p.rgba[((size_t)y * p.width + x) * 4];
            double cov = n ? a / n : 0;
            o[0] = To8((n ? r / n : 0) / 255.0 + checker / 255.0 * (1 - cov));
            o[1] = To8((n ? g / n : 0) / 255.0 + checker / 255.0 * (1 - cov));
            o[2] = To8((n ? b / n : 0) / 255.0 + checker / 255.0 * (1 - cov));
            o[3] = 255;
        }
    return p;
}
} // namespace tt
