// GdtWriter.cpp: GDT entries for text textures, built over Treyarch's own key sets (GdtTemplates.h).
#include "GdtWriter.h"
#include "GdtTemplates.h"
#include <cctype>
#include <cstring>
#include <map>

namespace tt
{
namespace
{
bool SameText(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// Values in the templates are already in GDT file form (escaped), overrides are escaped here.
std::string Emit(const char* const (*tpl)[2], size_t count, const std::string& name, const char* gdf,
                 const std::map<std::string, std::string>& overrides)
{
    std::string out = "\t\"" + name + "\" ( \"" + gdf + "\" )\n\t{\n";
    for (size_t i = 0; i < count; ++i)
    {
        auto o = overrides.find(tpl[i][0]);
        out += "\t\t\"";
        out += tpl[i][0];
        out += "\" \"";
        out += o != overrides.end() ? GdtEscape(o->second) : std::string(tpl[i][1]);
        out += "\"\n";
    }
    out += "\t}\n";
    return out;
}

bool ReadQuoted(const std::string& t, size_t& p, std::string& out)
{
    if (p >= t.size() || t[p] != '"') return false;
    out.clear();
    for (size_t i = p + 1; i < t.size(); ++i)
    {
        if (t[i] == '\\' && i + 1 < t.size())
        {
            out += t[++i];
            continue;
        }
        if (t[i] == '"')
        {
            p = i + 1;
            return true;
        }
        out += t[i];
    }
    return false;
}
void SkipSpace(const std::string& t, size_t& p)
{
    while (p < t.size() && isspace((unsigned char)t[p])) ++p;
}
} // namespace

std::string SanitizeAssetName(const std::string& name)
{
    std::string out;
    for (char c : name)
    {
        char l = (char)tolower((unsigned char)c);
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_') out += l;
        else if (l == ' ' || l == '-' || l == '.') out += '_';
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 60) out.resize(60);
    return out;
}

std::string GdtEscape(const std::string& value)
{
    std::string out;
    for (char c : value)
    {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

std::string BuildImageEntry(const TextureAsset& a)
{
    bool alpha = a.kind != MaterialKind::Opaque;
    std::map<std::string, std::string> o = {
        {"baseImage", a.baseImageRelative},
        {"compressionMethod", "compressed high color"}, // BC7: keeps letter edges sharp, with or without alpha
        {"semantic", "diffuseMap"},
        {"mipBase", "1/1"},
        {"mipMode", a.kind == MaterialKind::Cutout ? "PunchThru" : alpha ? "Alpha" : "Average"}, // keeps letters from thinning out in mips
        {"clampU", a.clamp ? "1" : "0"},
        {"clampV", a.clamp ? "1" : "0"},
        {"streamable", "1"},
        {"imageType", "Texture"},
    };
    return Emit(kImageTemplate, sizeof kImageTemplate / sizeof kImageTemplate[0], a.image, "image.gdf", o);
}

std::string BuildGlowImageEntry(const TextureAsset& a)
{
    if (a.glowImage.empty() || a.glowImage == a.image || a.glowImageRelative.empty()) return std::string();
    TextureAsset g = a;
    g.image = a.glowImage;
    g.baseImageRelative = a.glowImageRelative;
    g.kind = MaterialKind::Opaque; // black where nothing glows, no alpha
    return BuildImageEntry(g);
}

std::string BuildMaterialEntry(const TextureAsset& a)
{
    // The template is a stock lit_decal material; these are the keys that differ between the stock kinds.
    std::map<std::string, std::string> o = {
        {"colorMap", a.image},
        {"normalMap", "$normal"},
        {"cosinePowerMap", "$gray"},
        {"specColorMap", ""},
        {"surfaceType", a.surfaceType.empty() ? "<none>" : a.surfaceType},
        {"glossSurfaceType", "paint"},
    };
    switch (a.kind)
    {
    case MaterialKind::Opaque:
        o["materialCategory"] = "Geometry";
        o["materialType"] = "lit";
        o["usage"] = a.usage.empty() ? "interior wall" : a.usage;
        break;
    case MaterialKind::Decal:
        o["materialCategory"] = "Decal";
        o["materialType"] = "lit_decal";
        o["usage"] = a.usage.empty() ? "decal" : a.usage;
        break;
    case MaterialKind::Cutout:
        o["materialCategory"] = "Geometry";
        o["materialType"] = "lit_alphatest";
        o["usage"] = a.usage.empty() ? "decal" : a.usage;
        break;
    }
    if (!a.glowImage.empty())
    {
        // Treyarch's glowing signs: lit_emissive (mtl_p7_sin_sign_neon_*), decal_emissive (t7_decal_signage_zur_directory),
        // lit_emissive_transparent (t7_decal_signage_glass_*): colorMap00 is the glow, scaleRGB its strength.
        o["materialType"] = a.kind == MaterialKind::Opaque ? "lit_emissive" : a.kind == MaterialKind::Decal ? "decal_emissive" : "lit_emissive_transparent";
        o["colorMap00"] = a.glowImage;
        o["scaleRGB"] = a.glowScale.empty() ? "8" : a.glowScale;
        // APE's "Use gameplay intensity": without it the glow is too dim to see in game.
        o["emissiveIncompetence"] = "1";
    }
    return Emit(kMaterialTemplate, sizeof kMaterialTemplate / sizeof kMaterialTemplate[0], a.material, "material.gdf", o);
}

std::vector<GdtEntrySpan> ScanGdtEntries(const std::string& t)
{
    std::vector<GdtEntrySpan> entries;
    size_t pos = t.find('{');
    if (pos == std::string::npos) return entries;
    ++pos;
    while (pos < t.size())
    {
        SkipSpace(t, pos);
        if (pos >= t.size() || t[pos] == '}') break;
        GdtEntrySpan e;
        e.begin = pos;
        size_t p = pos;
        bool ok = ReadQuoted(t, p, e.name);
        if (ok)
        {
            SkipSpace(t, p);
            ok = p < t.size() && (t[p] == '(' || t[p] == '[');
            if (ok)
            {
                char close = t[p] == '(' ? ')' : ']';
                e.derived = close == ']';
                ++p;
                SkipSpace(t, p);
                ok = ReadQuoted(t, p, e.gdf);
                SkipSpace(t, p);
                ok = ok && p < t.size() && t[p] == close;
                ++p;
            }
        }
        if (ok)
        {
            SkipSpace(t, p);
            ok = p < t.size() && t[p] == '{';
        }
        if (!ok)
        {
            ++pos;
            continue;
        }
        ++p;
        int depth = 1;
        std::string skip;
        while (p < t.size())
        {
            if (t[p] == '"')
            {
                if (!ReadQuoted(t, p, skip)) p = t.size();
                continue;
            }
            if (t[p] == '{') ++depth;
            if (t[p] == '}' && --depth == 0) break;
            ++p;
        }
        if (p >= t.size()) break;
        e.end = p + 1;
        pos = e.end;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool UpsertGdtEntries(std::string& text, const std::vector<std::pair<std::string, std::string>>& namesAndGdfs,
                      const std::string& entries, std::string& error)
{
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        text = "{\n" + entries + "}\n";
        return true;
    }
    if (text[first] != '{' || text.rfind('}') == std::string::npos || text.rfind('}') <= first)
    {
        error = "The GDT file is not a GDT (it does not start with '{' and end with '}'); it was left unchanged.";
        return false;
    }
    auto spans = ScanGdtEntries(text);
    for (auto it = spans.rbegin(); it != spans.rend(); ++it)
    {
        bool match = false;
        for (auto& ng : namesAndGdfs)
            if (SameText(it->name, ng.first) && (it->derived || SameText(it->gdf, ng.second))) match = true;
        if (!match) continue;
        size_t begin = it->begin;
        while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
        size_t end = it->end;
        if (end < text.size() && text[end] == '\r') ++end;
        if (end < text.size() && text[end] == '\n') ++end;
        text.erase(begin, end - begin);
    }
    size_t close = text.rfind('}');
    // Start the new entries on their own line.
    std::string insert = entries;
    if (close > 0 && text[close - 1] != '\n') insert = "\n" + insert;
    text.insert(close, insert);
    return true;
}
} // namespace tt
