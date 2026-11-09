// Png.cpp: PNG encoding. Rows use filter 0 and the deflate stream uses the fixed Huffman table with LZ77 matches
// against the previous pixel and the pixel above, which is all a flat-colour text texture needs to stay small.
#include "Png.h"
#include <cstring>

namespace tt
{
namespace
{
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct BitWriter
{
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int bits = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    // value's low `count` bits, least significant first (deflate's order for everything but Huffman codes)
    void Put(uint32_t value, int count)
    {
        acc |= value << bits;
        bits += count;
        while (bits >= 8)
        {
            out.push_back(uint8_t(acc));
            acc >>= 8;
            bits -= 8;
        }
    }
    // a Huffman code, most significant bit first
    void PutCode(uint32_t code, int length)
    {
        uint32_t rev = 0;
        for (int i = 0; i < length; ++i) rev |= ((code >> i) & 1u) << (length - 1 - i);
        Put(rev, length);
    }
    void Flush()
    {
        if (bits > 0) out.push_back(uint8_t(acc));
        acc = 0;
        bits = 0;
    }
};

void PutLiteral(BitWriter& w, int v)
{
    if (v < 144) w.PutCode(0x30 + v, 8);
    else if (v < 256) w.PutCode(0x190 + (v - 144), 9);
    else if (v < 280) w.PutCode(v - 256, 7);
    else w.PutCode(0xC0 + (v - 280), 8);
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                           6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void PutMatch(BitWriter& w, int length, int distance)
{
    int li = 28;
    while (kLenBase[li] > length) --li;
    PutLiteral(w, 257 + li);
    if (kLenExtra[li]) w.Put(uint32_t(length - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > distance) --di;
    w.PutCode(uint32_t(di), 5);
    if (kDistExtra[di]) w.Put(uint32_t(distance - kDistBase[di]), kDistExtra[di]);
}

std::vector<uint8_t> Deflate(const std::vector<uint8_t>& data, int rowBytes)
{
    std::vector<uint8_t> out;
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter w(out);
    w.Put(1, 1); // final block
    w.Put(1, 2); // fixed Huffman
    const int n = (int)data.size();
    const int candidates[2] = {4, rowBytes};
    int i = 0;
    while (i < n)
    {
        int bestLen = 0, bestDist = 0;
        for (int d : candidates)
        {
            if (d <= 0 || d > 32768 || i - d < 0) continue;
            int len = 0;
            while (len < 258 && i + len < n && data[i + len] == data[i + len - d]) ++len;
            if (len > bestLen)
            {
                bestLen = len;
                bestDist = d;
            }
        }
        if (bestLen >= 3)
        {
            PutMatch(w, bestLen, bestDist);
            i += bestLen;
        }
        else
            PutLiteral(w, data[i++]);
    }
    PutLiteral(w, 256);
    w.Flush();
    uint32_t a = 1, b = 0;
    for (uint8_t c : data)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(adler >> s));
    return out;
}

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void Chunk(std::vector<uint8_t>& png, const char type[4], const std::vector<uint8_t>& body)
{
    PutU32(png, (uint32_t)body.size());
    std::vector<uint8_t> tagged(type, type + 4);
    tagged.insert(tagged.end(), body.begin(), body.end());
    png.insert(png.end(), tagged.begin(), tagged.end());
    PutU32(png, Crc32(tagged.data(), tagged.size()));
}
} // namespace

std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgba)
{
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PutU32(ihdr, (uint32_t)width);
    PutU32(ihdr, (uint32_t)height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit, RGBA, deflate, filter 0, no interlace
    Chunk(png, "IHDR", ihdr);
    const int rowBytes = width * 4 + 1;
    std::vector<uint8_t> raw((size_t)rowBytes * height);
    for (int y = 0; y < height; ++y)
    {
        raw[(size_t)y * rowBytes] = 0;
        memcpy(&raw[(size_t)y * rowBytes + 1], rgba + (size_t)y * width * 4, (size_t)width * 4);
    }
    Chunk(png, "IDAT", Deflate(raw, rowBytes));
    Chunk(png, "IEND", {});
    return png;
}
} // namespace tt
