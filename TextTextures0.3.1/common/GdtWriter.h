// GdtWriter.h: builds the image + material entries for a text texture and merges them into a GDT's text.
// No Windows or Radiant dependencies, so the tests build it alone.
#pragma once
#include <string>
#include <vector>

namespace tt
{
enum class MaterialKind
{
    Opaque = 0,  // Geometry / lit: a solid sign
    Decal = 1,   // Decal / lit_decal: transparent background, sits on a wall without z-fighting
    Cutout = 2,  // Geometry / lit_alphatest: transparent background, hard edges, for free-standing letters
};

struct TextureAsset
{
    std::string material;          // material asset name, e.g. "sign_welcome"
    std::string image;             // image asset name, e.g. "i_sign_welcome"
    std::string baseImageRelative; // path from the BO3 root, single backslashes: texture_assets\text_textures\sign_welcome.png
    MaterialKind kind = MaterialKind::Opaque;
    std::string surfaceType = "<none>";
    std::string usage;             // Radiant texture browser category; empty = by kind
    bool clamp = true;             // false for textures that repeat (wires)
    // Glow: an emissive material (lit_emissive / decal_emissive / lit_emissive_transparent, by kind) whose
    // colorMap00 is glowImage. Empty = no glow.
    std::string glowImage;            // image asset name, e.g. "i_sign_welcome_glow" (may equal image)
    std::string glowImageRelative;    // its file, from the BO3 root ("" when glowImage is image)
    std::string glowScale = "8";      // scaleRGB: how bright the glow is (Treyarch's signs: 4 to 32, mostly 8)
};

// Lower-case letters, digits and underscores only; empty when nothing usable is left.
std::string SanitizeAssetName(const std::string& name);
// "C:\x\y" -> "C:\\x\\y" for a GDT string value
std::string GdtEscape(const std::string& value);

std::string BuildImageEntry(const TextureAsset& a);
// The image entry of a.glowImage (opaque, no alpha); "" when there is none or it is a.image.
std::string BuildGlowImageEntry(const TextureAsset& a);
std::string BuildMaterialEntry(const TextureAsset& a);

// Every top-level entry of a GDT: name, gdf ("material.gdf"), [begin, end) of the whole entry.
struct GdtEntrySpan
{
    std::string name, gdf;
    size_t begin = 0, end = 0;
    bool derived = false;
};
std::vector<GdtEntrySpan> ScanGdtEntries(const std::string& text);

// Returns the GDT text with any earlier entries of these names replaced by `entries` (appended before the final
// brace). An empty or brace-less text becomes a new GDT. false + error when the text is not a usable GDT.
bool UpsertGdtEntries(std::string& text, const std::vector<std::pair<std::string, std::string>>& namesAndGdfs,
                      const std::string& entries, std::string& error);
} // namespace tt
