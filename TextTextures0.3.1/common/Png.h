// Png.h: a small, dependency-free PNG encoder (8-bit RGBA, deflate with a fixed Huffman table and run matches).
#pragma once
#include <cstdint>
#include <vector>

namespace tt
{
// rgba: width * height * 4 bytes, straight (non-premultiplied) alpha, rows top to bottom.
std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgba);
}
