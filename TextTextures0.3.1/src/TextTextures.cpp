// TextTextures: type text in a Radiant pane and get a BO3 texture of it. The plugin renders the text to a PNG
// under texture_assets, writes image + material entries to a GDT (over Treyarch's own key sets), runs
// gdtdb /update, and can then put the material on the face of the selected brushes you are looking at, or
// place a new sign brush in front of the camera.
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"

#include "GdtWriter.h"
#include "MapEdit.h"
#include "Platform.h"
#include "Radiant.h"
#include "Png.h"
#include "Text3D.h"
#include "TextRender.h"
#include "Version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>

using namespace tt;

namespace
{
// ---- constants ---------------------------------------------------------------------------------------------------

const int kSizes[] = {64, 128, 256, 512, 1024, 2048};
const int kSizeCount = sizeof kSizes / sizeof kSizes[0];
// Surface types Treyarch's own materials use (impact effects and footsteps in game).
const char* const kSurfaces[] = {"<none>", "paintedmetal", "metal", "wood", "plaster", "concrete", "brick",
                                 "paper",  "plastic",      "glass", "cloth", "rock"};
const int kSurfaceCount = sizeof kSurfaces / sizeof kSurfaces[0];
const char* const kKinds[] = {"Solid sign (lit)", "Decal on a wall (lit_decal, see-through background)",
                              "Cut-out letters (lit_alphatest, see-through background)"};
const char* kUndoApply = "Apply text texture";
const char* kUndoPlace = "Place text sign";

// ---- state -------------------------------------------------------------------------------------------------------

ModToolsPluginApi g_api{};
ModToolsPane g_pane = nullptr;

struct Widgets
{
    ModToolsWidget root = nullptr, text = nullptr, font = nullptr, autoFit = nullptr, size = nullptr, sizeLabel = nullptr;
    ModToolsWidget bold = nullptr, italic = nullptr, underline = nullptr, align = nullptr, valign = nullptr;
    ModToolsWidget padding = nullptr, paddingLabel = nullptr, outline = nullptr, outlineLabel = nullptr;
    ModToolsWidget textSwatch = nullptr, outlineSwatch = nullptr, bgSwatch = nullptr, transparent = nullptr;
    ModToolsWidget width = nullptr, height = nullptr, kind = nullptr, surface = nullptr, name = nullptr, gdt = nullptr;
    ModToolsWidget glow = nullptr, glowScale = nullptr;
    ModToolsWidget preview = nullptr, previewInfo = nullptr, status = nullptr;
    ModToolsWidget create = nullptr, applyAfter = nullptr, applyMode = nullptr, apply = nullptr, signWidth = nullptr, place = nullptr;
    // 3D text model
    ModToolsWidget lineHeight = nullptr, depth = nullptr, sideSwatch = nullptr, frontMat = nullptr, sideMat = nullptr;
    ModToolsWidget backFace = nullptr, collision = nullptr, snap = nullptr, pivot = nullptr, build3d = nullptr, place3d = nullptr;
} w;

std::vector<std::wstring> g_fonts;
uint32_t g_textColor = 0xFFFFFFFF, g_outlineColor = 0xFF000000, g_bgColor = 0xFF1E3A5F, g_sideColor = 0xFF6E6E6E;
bool g_uiReady = false, g_loading = false;

// preview
bool g_dirty = true;
ULONGLONG g_lastChange = 0, g_lastPoll = 0;
std::string g_lastText;
int g_previewSerial = 0;
std::wstring g_lastPreviewFile;

// background build
std::atomic<bool> g_busy{false}, g_unloading{false};
std::thread g_worker;

// learned from the open map
int g_winding = 0;           // +1 Quake order, -1 BO3 order, 0 not known yet
std::string g_faceSuffix;    // what follows the rotation on a face line

// ---- helpers -----------------------------------------------------------------------------------------------------

const ModToolsUiApi* Ui() { return rp::host ? rp::host->ui : nullptr; }
template <typename T> bool HostHas(const T& field) { return rp::Has(&field) && field; }
template <typename T> bool UiHas(const T& field) { return rp::HasUi(&field) && field; }

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string Html(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '\n') o += "<br>";
        else o += c;
    }
    return o;
}
std::string Hex(uint32_t argb)
{
    char b[16];
    snprintf(b, sizeof b, "#%06X", argb & 0xFFFFFF);
    return b;
}
uint32_t ParseHex(const std::string& s, uint32_t def)
{
    std::string t = s;
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    if (t.size() != 6 && t.size() != 8) return def;
    char* end = nullptr;
    unsigned long v = strtoul(t.c_str(), &end, 16);
    if (!end || *end) return def;
    return t.size() == 6 ? 0xFF000000u | (uint32_t)v : (uint32_t)v;
}
// The ini cannot hold line breaks.
std::string EscapeIni(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c != '\r') o += c;
    }
    return o;
}
std::string UnescapeIni(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            ++i;
            o += s[i] == 'n' ? '\n' : s[i];
        }
        else
            o += s[i];
    }
    return o;
}

enum class Tone
{
    Info,
    Ok,
    Busy,
    Error
};
std::string g_statusText;
void Status(Tone tone, const std::string& text)
{
    g_statusText = text;
    rp::Log("status: %s", text.c_str());
    if (!w.status) return;
    const char* color = tone == Tone::Ok ? "#7bd88f" : tone == Tone::Error ? "#ff7b72" : tone == Tone::Busy ? "#e5c07b" : "#b8b8b8";
    std::string html = "<span style=\"color:" + std::string(color) + "\">" + Html(text) + "</span>";
    Ui()->SetLabelText(w.status, html.c_str());
}

bool g_plainText = false; // the text box is a QPlainTextEdit (else a line edit where "\n" starts a new line)
std::string ReadText()
{
    std::string t;
    if (g_plainText) ReadPlainTextDirect(w.text, t); // never ui->EditText: it assumes a QLineEdit
    else
    {
        std::string raw = rp::EditText(w.text);
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'n')
            {
                t += '\n';
                ++i;
            }
            else
                t += raw[i];
        }
    }
    std::string o;
    for (char c : t)
        if (c != '\r') o += c;
    return o;
}

int ComboIndex(ModToolsWidget combo, int def)
{
    int i = combo && UiHas(Ui()->ComboCurrent) ? Ui()->ComboCurrent(combo) : -1;
    return i < 0 ? def : i;
}
int SliderValue(ModToolsWidget s, int def) { return s && UiHas(Ui()->SliderValue) ? Ui()->SliderValue(s) : def; }
bool Checked(ModToolsWidget b) { return b && Ui()->IsChecked(b) != 0; }

// ---- settings ----------------------------------------------------------------------------------------------------

struct Settings
{
    TextStyle style;
    int width = 1024, height = 512;
    MaterialKind kind = MaterialKind::Opaque;
    GlowMode glow = GlowMode::Off;
    std::string glowScale = "8";
    std::string surface = "<none>";
    std::string name, gdt;
    ApplyMode applyMode = ApplyMode::CameraFace;
    double signWidth = 64;
};

std::wstring DefaultGdt() { return Join(Join(GameRoot(), L"source_data"), L"text_textures.gdt"); }

Settings ReadSettings()
{
    Settings s;
    s.style.text = Utf8ToWide(ReadText());
    int fi = ComboIndex(w.font, -1);
    if (fi >= 0 && fi < (int)g_fonts.size()) s.style.font = g_fonts[fi];
    s.style.autoFit = Checked(w.autoFit);
    s.style.fontPx = SliderValue(w.size, 96);
    s.style.bold = Checked(w.bold);
    s.style.italic = Checked(w.italic);
    s.style.underline = Checked(w.underline);
    s.style.align = ComboIndex(w.align, 1);
    s.style.valign = ComboIndex(w.valign, 1);
    s.style.padding = SliderValue(w.padding, 32);
    s.style.outlinePx = SliderValue(w.outline, 0);
    s.style.textArgb = g_textColor;
    s.style.outlineArgb = g_outlineColor;
    s.style.backgroundArgb = Checked(w.transparent) ? (g_bgColor & 0xFFFFFF) : (g_bgColor | 0xFF000000);
    s.width = kSizes[std::min(ComboIndex(w.width, 4), kSizeCount - 1)];
    s.height = kSizes[std::min(ComboIndex(w.height, 3), kSizeCount - 1)];
    s.kind = (MaterialKind)std::min(ComboIndex(w.kind, 0), 2);
    s.glow = (GlowMode)std::min(ComboIndex(w.glow, 0), 3);
    double gs = atof(rp::EditText(w.glowScale).c_str());
    s.glowScale = FormatNumber(gs > 0 && gs <= 1024 ? gs : 8);
    s.surface = kSurfaces[std::min(ComboIndex(w.surface, 0), kSurfaceCount - 1)];
    s.name = SanitizeAssetName(rp::EditText(w.name));
    s.gdt = rp::EditText(w.gdt);
    s.applyMode = ComboIndex(w.applyMode, 0) == 1 ? ApplyMode::AllFaces : ApplyMode::CameraFace;
    double sw = atof(rp::EditText(w.signWidth).c_str());
    s.signWidth = sw >= 1 && sw <= 16384 ? sw : 64;
    return s;
}

void SaveSettings()
{
    if (!g_uiReady || g_loading) return;
    Settings s = ReadSettings();
    rp::ConfigSet("style", "text", EscapeIni(WideToUtf8(s.style.text)));
    rp::ConfigSet("style", "font", WideToUtf8(s.style.font));
    rp::ConfigSet("style", "auto_fit", s.style.autoFit ? "1" : "0");
    rp::ConfigSet("style", "size", std::to_string(s.style.fontPx));
    rp::ConfigSet("style", "bold", s.style.bold ? "1" : "0");
    rp::ConfigSet("style", "italic", s.style.italic ? "1" : "0");
    rp::ConfigSet("style", "underline", s.style.underline ? "1" : "0");
    rp::ConfigSet("style", "align", std::to_string(s.style.align));
    rp::ConfigSet("style", "valign", std::to_string(s.style.valign));
    rp::ConfigSet("style", "padding", std::to_string(s.style.padding));
    rp::ConfigSet("style", "outline", std::to_string(s.style.outlinePx));
    rp::ConfigSet("style", "text_color", Hex(g_textColor));
    rp::ConfigSet("style", "outline_color", Hex(g_outlineColor));
    rp::ConfigSet("style", "background_color", Hex(g_bgColor));
    rp::ConfigSet("style", "transparent", Checked(w.transparent) ? "1" : "0");
    rp::ConfigSet("texture", "width", std::to_string(s.width));
    rp::ConfigSet("texture", "height", std::to_string(s.height));
    rp::ConfigSet("texture", "kind", std::to_string((int)s.kind));
    rp::ConfigSet("texture", "glow", std::to_string((int)s.glow));
    rp::ConfigSet("texture", "glow_brightness", rp::EditText(w.glowScale));
    rp::ConfigSet("texture", "surface", s.surface);
    rp::ConfigSet("output", "name", rp::EditText(w.name));
    rp::ConfigSet("output", "gdt", s.gdt);
    rp::ConfigSet("sign", "apply_mode", std::to_string((int)s.applyMode));
    rp::ConfigSet("sign", "apply_after_create", Checked(w.applyAfter) ? "1" : "0");
    rp::ConfigSet("sign", "width", rp::EditText(w.signWidth));
    if (w.build3d)
    {
        rp::ConfigSet("model", "line_height", rp::EditText(w.lineHeight));
        rp::ConfigSet("model", "depth", rp::EditText(w.depth));
        rp::ConfigSet("model", "side_color", Hex(g_sideColor));
        rp::ConfigSet("model", "front_material", rp::EditText(w.frontMat));
        rp::ConfigSet("model", "side_material", rp::EditText(w.sideMat));
        rp::ConfigSet("model", "back_face", Checked(w.backFace) ? "1" : "0");
        rp::ConfigSet("model", "collision", Checked(w.collision) ? "1" : "0");
        rp::ConfigSet("model", "snap", Checked(w.snap) ? "1" : "0");
        rp::ConfigSet("model", "pivot", std::to_string(ComboIndex(w.pivot, 0)));
        rp::ConfigSet("model", "place", Checked(w.place3d) ? "1" : "0");
    }
}

int SizeIndex(int px, int def)
{
    for (int i = 0; i < kSizeCount; ++i)
        if (kSizes[i] == px) return i;
    return def;
}

// ---- preview -----------------------------------------------------------------------------------------------------

std::wstring PreviewDir()
{
    wchar_t tmp[MAX_PATH + 1]{};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = Join(n ? std::wstring(tmp, n) : GameRoot(), L"TextTextures");
    EnsureDirectory(dir);
    return dir;
}
std::string FileUrl(const std::wstring& path)
{
    std::string p = WUtf8(path), url = "file:///";
    for (char c : p)
    {
        if (c == '\\') url += '/';
        else if (c == ' ') url += "%20";
        else if (c == '#') url += "%23";
        else if (c == '%') url += "%25";
        else url += c;
    }
    return url;
}

void UpdateSwatches()
{
    auto swatch = [](ModToolsWidget label, uint32_t argb, const char* extra) {
        if (!label) return;
        std::string html = "<span style=\"background-color:" + Hex(argb) + ";\">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</span>&nbsp;" +
                           Hex(argb) + extra;
        Ui()->SetLabelText(label, html.c_str());
    };
    swatch(w.textSwatch, g_textColor, "");
    swatch(w.outlineSwatch, g_outlineColor, "");
    swatch(w.bgSwatch, g_bgColor, "");
    swatch(w.sideSwatch, g_sideColor, "");
}

void UpdateLabels()
{
    if (!g_uiReady) return;
    bool autoFit = Checked(w.autoFit);
    Ui()->SetEnabled(w.size, autoFit ? 0 : 1);
    if (!autoFit) Ui()->SetLabelText(w.sizeLabel, (std::to_string(SliderValue(w.size, 96)) + " px").c_str());
    Ui()->SetLabelText(w.paddingLabel, (std::to_string(SliderValue(w.padding, 32)) + " px").c_str());
    int o = SliderValue(w.outline, 0);
    Ui()->SetLabelText(w.outlineLabel, o ? (std::to_string(o) + " px").c_str() : "off");
}

void RenderPreview()
{
    if (!g_uiReady || !w.preview) return;
    Settings s = ReadSettings();
    // Draw the preview small, with every size scaled down by the same factor, so it looks like the texture.
    double k = std::min(1.0, 400.0 / std::max(s.width, s.height));
    int pw = std::max(1, (int)(s.width * k + .5)), ph = std::max(1, (int)(s.height * k + .5));
    TextStyle st = s.style;
    st.fontPx = std::max(1, (int)(st.fontPx * k + .5));
    st.padding = (int)(st.padding * k + .5);
    if (st.outlinePx > 0) st.outlinePx = std::max(1, (int)(st.outlinePx * k + .5));
    Image img;
    int used = 0;
    std::string err;
    if (!RenderTexture(st, pw, ph, img, used, err))
    {
        Status(Tone::Error, "Preview failed: " + err);
        return;
    }
    if (s.style.autoFit) Ui()->SetLabelText(w.sizeLabel, ("auto (" + std::to_string((int)(used / k + .5)) + " px)").c_str());
    Image pv = MakePreview(img, 300, 220);
    std::vector<uint8_t> png = EncodePng(pv.width, pv.height, pv.rgba.data());
    std::wstring file = Join(PreviewDir(), L"preview_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++g_previewSerial) + L".png");
    std::string werr;
    if (!WriteFileAtomic(file, png.data(), png.size(), false, werr))
    {
        Status(Tone::Error, werr);
        return;
    }
    // A new file name each time: Qt caches images by URL.
    std::string html = "<img src=\"" + FileUrl(file) + "\" width=\"" + std::to_string(pv.width) + "\" height=\"" + std::to_string(pv.height) + "\">";
    Ui()->SetLabelText(w.preview, html.c_str());
    if (!g_lastPreviewFile.empty()) DeleteFileW(g_lastPreviewFile.c_str());
    g_lastPreviewFile = file;
    const char* kind = s.kind == MaterialKind::Opaque ? "solid" : s.kind == MaterialKind::Decal ? "decal" : "cut-out";
    std::string info = std::to_string(s.width) + " x " + std::to_string(s.height) + " texture, " + kind;
    if (s.kind != MaterialKind::Opaque && Checked(w.transparent) == false) info += " (tick Transparent background to see through it)";
    if (s.glow != GlowMode::Off)
        info += s.glow == GlowMode::Letters ? ", letters glow" : s.glow == GlowMode::LettersOutline ? ", letters and outline glow" : ", whole sign glows";
    Ui()->SetLabelText(w.previewInfo, ("<small>" + Html(info) + "</small>").c_str());
    g_dirty = false;
}

void MarkDirty()
{
    g_dirty = true;
    g_lastChange = GetTickCount64();
    UpdateLabels();
    SaveSettings();
}
void OnValueChanged(void*, int) { MarkDirty(); }
void OnEditFinished(void*, int) { SaveSettings(); }

// ---- colours -----------------------------------------------------------------------------------------------------

bool PickColor(uint32_t& argb, const char* title)
{
    if (UiHas(Ui()->PickColor))
    {
        uint32_t out = 0;
        if (!Ui()->PickColor(w.root, argb | 0xFF000000, title, &out)) return false;
        argb = out | 0xFF000000;
        return true;
    }
    static COLORREF custom[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = rp::host->MainWindow ? (HWND)rp::host->MainWindow(rp::self) : nullptr;
    cc.rgbResult = RGB((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    argb = 0xFF000000u | (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
    return true;
}
void OnPickText(void*)
{
    if (PickColor(g_textColor, "Text colour")) UpdateSwatches(), MarkDirty();
}
void OnPickOutline(void*)
{
    if (PickColor(g_outlineColor, "Outline colour")) UpdateSwatches(), MarkDirty();
}
void OnPickSide(void*)
{
    if (PickColor(g_sideColor, "Side colour of the 3D letters")) UpdateSwatches(), SaveSettings();
}
void OnPickBackground(void*)
{
    if (PickColor(g_bgColor, "Background colour")) UpdateSwatches(), MarkDirty();
}

void OnBrowseGdt(void*)
{
    wchar_t file[MAX_PATH]{};
    std::wstring current = Utf8W(rp::EditText(w.gdt));
    wcsncpy(file, current.c_str(), MAX_PATH - 1);
    std::wstring initial = Join(GameRoot(), L"source_data");
    OPENFILENAMEW d{};
    d.lStructSize = sizeof d;
    d.hwndOwner = rp::host->MainWindow ? (HWND)rp::host->MainWindow(rp::self) : nullptr;
    d.lpstrFilter = L"GDT files (*.gdt)\0*.gdt\0";
    d.lpstrFile = file;
    d.nMaxFile = MAX_PATH;
    d.lpstrInitialDir = initial.c_str();
    d.lpstrDefExt = L"gdt";
    d.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    d.lpstrTitle = L"GDT for text textures (an existing one, or a new name)";
    if (GetSaveFileNameW(&d))
    {
        Ui()->SetEditText(w.gdt, WUtf8(file).c_str());
        SaveSettings();
    }
}

// ---- what the open map tells us --------------------------------------------------------------------------------------

void LearnFromText(const std::string& text)
{
    if (!g_winding)
        if (int wd = DetectWinding(text))
        {
            g_winding = wd;
            rp::ConfigSet("map", "winding", std::to_string(wd));
            rp::Log("brush plane order learned from the map: %s", wd > 0 ? "Quake" : "BO3");
        }
    if (g_faceSuffix.empty())
    {
        std::string s = FaceSuffixFrom(text);
        if (!s.empty())
        {
            g_faceSuffix = s;
            rp::ConfigSet("map", "face_suffix", s);
        }
    }
}

// Looks at a few small entities' brushes when no selection has shown the plane order yet.
void LearnFromMap()
{
    if (g_winding && !g_faceSuffix.empty()) return;
    ModToolsEntity world = rp::host->WorldEntity ? rp::host->WorldEntity(rp::self) : nullptr;
    int looked = 0;
    rp::ForEachEntity([&](ModToolsEntity e) {
        if ((g_winding && !g_faceSuffix.empty()) || looked >= 64 || e == world) return;
        if (rp::EntityFixedSize(e)) return;
        bool hasBrush = false;
        rp::ForEachEntityBrush(e, [&](ModToolsBrush) { hasBrush = true; });
        if (!hasBrush) return;
        ++looked;
        std::string text;
        if (rp::WriteEntityText(e, text)) LearnFromText(text);
    });
}

// ---- apply to selection -----------------------------------------------------------------------------------------------

bool CameraView(Vec3& origin, Vec3& forward)
{
    float o[3]{}, a[3]{};
    if (!rp::host->GetCamera || !rp::host->GetCamera(rp::self, o, a)) return false;
    origin = {o[0], o[1], o[2]};
    forward = ForwardFromAngles(a[0], a[1]);
    return true;
}

bool ApplyToSelection(const std::string& material, ApplyMode mode, std::string& message)
{
    if (rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self) > 0 && !rp::ConfigGetBool("sign", "allow_in_prefab", true))
    {
        message = "Leave the prefab first.";
        return false;
    }
    std::vector<ModToolsSelectedBrush> sel = rp::GetSelection();
    if (sel.empty())
    {
        message = "Select one or more brushes, look at the face you want the text on, then click Apply.";
        return false;
    }
    Vec3 cam, fwd;
    if (!CameraView(cam, fwd))
    {
        message = "Could not read the 3D camera.";
        return false;
    }
    // Brush handles for the selected GUIDs.
    std::map<std::string, ModToolsBrush> byGuid;
    for (ModToolsBrushInst inst : rp::SelectedBrushInsts())
        if (ModToolsBrush b = rp::BrushOfInst(inst))
        {
            std::string g = Lower(rp::BrushGuid(b));
            if (!g.empty()) byGuid[g] = b;
        }
    if (byGuid.empty())
    {
        std::vector<std::string> guids;
        for (auto& s : sel) guids.push_back(s.brushGuid);
        auto found = rp::FindBrushesByGuid(guids);
        for (size_t i = 0; i < found.size(); ++i)
            if (found[i]) byGuid[Lower(guids[i])] = found[i];
    }
    ModToolsEntity world = rp::host->WorldEntity(rp::self);
    std::map<ModToolsEntity, std::vector<ModToolsBrush>> groups;
    std::map<ModToolsBrush, std::string> guidOf;
    size_t unresolved = 0;
    for (auto& s : sel)
    {
        std::string g = Lower(s.brushGuid);
        auto it = byGuid.find(g);
        if (it == byGuid.end())
        {
            ++unresolved;
            continue;
        }
        ModToolsEntity e = rp::host->BrushOwner(rp::self, it->second);
        groups[e].push_back(it->second);
        guidOf[it->second] = g;
    }
    int faces = 0;
    size_t brushes = 0, skipped = unresolved;
    bool undo = HostHas(rp::host->BeginUndo) && HostHas(rp::host->EndUndo);

    // World brushes: import retextured copies, then delete the originals, as one undo step.
    auto worldGroup = groups.find(world);
    if (worldGroup != groups.end())
    {
        auto& bs = worldGroup->second;
        std::string text;
        if (!rp::WriteEntityTextWith(world, bs, text))
        {
            message = "Radiant could not write the selected brushes.";
            return false;
        }
        LearnFromText(text);
        std::set<std::string> only;
        for (ModToolsBrush b : bs) only.insert(guidOf[b]);
        std::vector<std::string> changed;
        int n = ApplyMaterialInText(text, material, mode, cam, fwd, &only, &changed);
        if (n > 0)
        {
            std::set<std::string> changedSet(changed.begin(), changed.end());
            std::vector<ModToolsBrush> originals;
            for (ModToolsBrush b : bs)
                if (changedSet.count(guidOf[b])) originals.push_back(b);
            std::string import = WorldBrushImportText(text, &changedSet);
            if (undo) rp::host->BeginUndo(rp::self, kUndoApply);
            if (!rp::host->ImportMapText(rp::self, import.c_str(), kUndoApply))
            {
                if (undo) rp::host->EndUndo(rp::self);
                message = "Radiant refused the retextured brushes; nothing was changed.";
                rp::Log("ImportMapText failed for:\n%s", import.c_str());
                return false;
            }
            if (!rp::host->DeleteBrushes(rp::self, originals.data(), originals.size(), kUndoApply))
            {
                if (undo) rp::host->EndUndo(rp::self);
                if (HostHas(rp::host->TriggerMenuAction)) rp::host->TriggerMenuAction(rp::self, "Edit/Undo");
                message = "Radiant could not replace the original brushes; the change was undone.";
                return false;
            }
            if (undo) rp::host->EndUndo(rp::self);
            faces += n;
            brushes += changed.size();
        }
        skipped += bs.size() - changed.size();
        groups.erase(worldGroup);
    }

    // Brushes of other entities (func_group, script_brushmodel...): rebuild each entity with its new text.
    if (!groups.empty())
    {
        rp::host->DeselectAll(rp::self); // links the selected brushes back into their entities
        for (auto& g : groups)
        {
            ModToolsEntity e = g.first;
            if (!e || rp::EntityFixedSize(e))
            {
                skipped += g.second.size();
                continue;
            }
            std::string text;
            if (!rp::WriteEntityText(e, text))
            {
                skipped += g.second.size();
                continue;
            }
            LearnFromText(text);
            std::set<std::string> only;
            for (ModToolsBrush b : g.second) only.insert(guidOf[b]);
            std::vector<std::string> changed;
            int n = ApplyMaterialInText(text, material, mode, cam, fwd, &only, &changed);
            if (n == 0 || rp::ReplaceEntity(e, text, kUndoApply).empty())
            {
                skipped += g.second.size();
                continue;
            }
            faces += n;
            brushes += changed.size();
            skipped += g.second.size() - changed.size();
        }
    }
    if (faces == 0)
    {
        message = "No brush faces were changed (patches, models and other point entities are skipped).";
        return false;
    }
    if (HostHas(rp::host->MarkMapModified)) rp::host->MarkMapModified(rp::self);
    RefreshRadiantView();
    message = "Applied " + material + " to " + std::to_string(faces) + " face(s) on " + std::to_string(brushes) + " brush(es)";
    if (skipped) message += "; skipped " + std::to_string(skipped) + " selected item(s) that are not plain brushes";
    message += ". Ctrl+Z undoes it.";
    return true;
}

// The material named in the pane exists (in gdt.db, or written by this plugin this session).
std::set<std::string> g_created;
bool MaterialKnown(const std::string& material)
{
    if (g_created.count(material)) return true;
    std::string gdt;
    if (!GdtDbFind(GameRoot(), material, "material", gdt)) return true; // cannot check: let Radiant decide
    return !gdt.empty();
}

void OnApply(void*)
{
    if (g_busy) return;
    Settings s = ReadSettings();
    if (s.name.empty())
    {
        Status(Tone::Error, "Enter a material name.");
        return;
    }
    if (!MaterialKnown(s.name))
    {
        Status(Tone::Error, "Material " + s.name + " is not in BO3's asset database yet. Click Create texture first.");
        return;
    }
    std::string msg;
    bool ok = ApplyToSelection(s.name, s.applyMode, msg);
    Status(ok ? Tone::Ok : Tone::Error, msg);
}

void PlaceSign(const std::string& material, const Settings& s)
{
    Vec3 cam, fwd;
    if (!CameraView(cam, fwd))
    {
        Status(Tone::Error, "Could not read the 3D camera.");
        return;
    }
    LearnFromMap();
    double width = s.signWidth, height = s.signWidth * s.height / s.width;
    double distance = std::max(8, rp::ConfigGetInt("sign", "distance", 96));
    double thickness = std::max(1, rp::ConfigGetInt("sign", "thickness", 2));
    std::string back = rp::ConfigGet("sign", "back_material", "caulk");
    SignBrush sign = BuildSignBrush(cam, fwd, distance, width, height, thickness, material, back.empty() ? "caulk" : back,
                                    g_faceSuffix, NewGuidText(), g_winding ? g_winding : -1);
    bool undo = HostHas(rp::host->BeginUndo) && HostHas(rp::host->EndUndo);
    rp::host->DeselectAll(rp::self);
    if (undo) rp::host->BeginUndo(rp::self, kUndoPlace);
    int ok = rp::host->ImportMapText(rp::self, sign.mapText.c_str(), kUndoPlace);
    if (undo) rp::host->EndUndo(rp::self);
    if (!ok)
    {
        rp::Log("sign import refused:\n%s", sign.mapText.c_str());
        Status(Tone::Error, "Radiant refused the sign brush. See RadiantPlugins.log.");
        return;
    }
    if (HostHas(rp::host->MarkMapModified)) rp::host->MarkMapModified(rp::self);
    RefreshRadiantView();
    char size[96];
    snprintf(size, sizeof size, "%s x %s units", FormatNumber(width).c_str(), FormatNumber(height).c_str());
    Status(Tone::Ok, "Placed a " + std::string(size) + " " + material + " sign in front of the camera (selected; Ctrl+Z undoes it).");
}

void OnPlace(void*)
{
    if (g_busy) return;
    Settings s = ReadSettings();
    if (s.name.empty())
    {
        Status(Tone::Error, "Enter a material name.");
        return;
    }
    if (!MaterialKnown(s.name))
    {
        Status(Tone::Error, "Material " + s.name + " is not in BO3's asset database yet. Click Create texture first.");
        return;
    }
    PlaceSign(s.name, s);
}

// ---- create (background) ---------------------------------------------------------------------------------------------

struct Job
{
    Settings s;
    std::wstring root, gdtPath, imagePath, glowPath;
    TextureAsset asset;
    bool applyAfter = false;
};
struct JobResult
{
    bool ok = false;
    std::string message, material;
    bool applyAfter = false;
    ApplyMode mode = ApplyMode::CameraFace;
    // 3D model builds
    bool isModel = false, placeModel = false, snap = true;
    std::string model;
    double modelHeight = 0;
    ModelPivot pivot = ModelPivot::BackBottomCentre;
};

void SetBusy(bool busy)
{
    g_busy = busy;
    for (ModToolsWidget b : {w.create, w.apply, w.place, w.build3d})
        if (b) Ui()->SetEnabled(b, busy ? 0 : 1);
}

bool PlaceModel(const JobResult& r, std::string& message);
void Finish(const JobResult& r)
{
    if (g_worker.joinable()) g_worker.join();
    SetBusy(false);
    if (!r.ok)
    {
        Status(Tone::Error, r.message);
        return;
    }
    if (r.isModel)
    {
        g_created.insert(r.model);
        if (!r.placeModel)
        {
            RefreshRadiantView();
            Status(Tone::Ok, r.message + " Add it with Entity > misc_model, model " + r.model + ".");
            return;
        }
        std::string msg;
        bool ok = PlaceModel(r, msg);
        Status(ok ? Tone::Ok : Tone::Error, r.message + " " + msg);
        return;
    }
    g_created.insert(r.material);
    RefreshRadiantView();
    if (r.applyAfter)
    {
        std::string msg;
        if (rp::GetSelection().empty())
        {
            Status(Tone::Ok, r.message + " Nothing was selected, so it was not applied anywhere.");
            return;
        }
        bool ok = ApplyToSelection(r.material, r.mode, msg);
        Status(ok ? Tone::Ok : Tone::Error, r.message + " " + msg);
        return;
    }
    Status(Tone::Ok, r.message);
}

void RunJob(Job job)
{
    JobResult r;
    r.material = job.asset.material;
    r.applyAfter = job.applyAfter;
    r.mode = job.s.applyMode;
    auto post = [&r] {
        JobResult copy = r;
        rp::PostToMainThread([copy] { Finish(copy); });
    };
    // Names already used by another GDT would make two assets of one name.
    std::string target = NormalizePath(job.gdtPath, job.root);
    std::vector<std::pair<std::string, const char*>> checks = {{job.asset.material, "material"}, {job.asset.image, "image"}};
    if (!job.asset.glowImage.empty()) checks.push_back({job.asset.glowImage, "image"});
    for (auto& check : checks)
    {
        std::string other;
        if (GdtDbFind(job.root, check.first, check.second, other) && !other.empty() && NormalizePath(Utf8W(other), job.root) != target)
        {
            r.message = std::string(check.second) + " " + check.first + " already exists in " + other + ". Choose another name.";
            post();
            return;
        }
    }
    Image img, glow;
    int used = 0;
    std::string err;
    if (!RenderTextureAndGlow(job.s.style, job.s.width, job.s.height, job.s.glow, img, glow, used, err))
    {
        r.message = "Rendering failed: " + err;
        post();
        return;
    }
    std::vector<uint8_t> png = EncodePng(img.width, img.height, img.rgba.data());
    size_t slash = job.imagePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos || !EnsureDirectory(job.imagePath.substr(0, slash)))
    {
        r.message = "Could not create the folder for " + WUtf8(job.imagePath) + ".";
        post();
        return;
    }
    if (!WriteFileAtomic(job.imagePath, png.data(), png.size(), false, err))
    {
        r.message = err;
        post();
        return;
    }
    if (!job.asset.glowImage.empty())
    {
        std::vector<uint8_t> gpng = EncodePng(glow.width, glow.height, glow.rgba.data());
        if (!WriteFileAtomic(job.glowPath, gpng.data(), gpng.size(), false, err))
        {
            r.message = err;
            post();
            return;
        }
    }
    std::string gdt;
    if (FileExists(job.gdtPath) && !ReadWholeFile(job.gdtPath, gdt))
    {
        r.message = "Could not read " + WUtf8(job.gdtPath) + ".";
        post();
        return;
    }
    std::string entries = BuildImageEntry(job.asset) + BuildGlowImageEntry(job.asset) + BuildMaterialEntry(job.asset);
    std::vector<std::pair<std::string, std::string>> names = {{job.asset.material, "material.gdf"}, {job.asset.image, "image.gdf"}};
    if (!job.asset.glowImage.empty()) names.push_back({job.asset.glowImage, "image.gdf"});
    if (!UpsertGdtEntries(gdt, names, entries, err))
    {
        r.message = err;
        post();
        return;
    }
    size_t gslash = job.gdtPath.find_last_of(L"\\/");
    if (gslash != std::wstring::npos) EnsureDirectory(job.gdtPath.substr(0, gslash));
    if (!WriteFileAtomic(job.gdtPath, gdt.data(), gdt.size(), true, err))
    {
        r.message = err;
        post();
        return;
    }
    rp::Log("wrote %s (%dx%d, font %d px) and GDT entries %s / %s in %s", WUtf8(job.imagePath).c_str(), img.width, img.height, used,
            job.asset.image.c_str(), job.asset.material.c_str(), WUtf8(job.gdtPath).c_str());
    if (!UpdateGdtDb(job.root, [] { return g_unloading.load(); }, err))
    {
        r.message = "Texture and GDT written, but " + err + " Opening APE once also refreshes the asset database.";
        post();
        return;
    }
    r.ok = true;
    r.message = "Created material " + job.asset.material + " (" + std::to_string(img.width) + "x" + std::to_string(img.height) +
                (job.asset.glowImage.empty() ? ")." : ", glowing at brightness " + job.asset.glowScale + ").");
    post();
}

void StartCreate()
{
    if (g_busy) return;
    Settings s = ReadSettings();
    std::wstring root = GameRoot();
    if (root.empty())
    {
        Status(Tone::Error, "The BO3 folder is unknown. Set root= in RadiantPlugins.ini.");
        return;
    }
    if (s.style.text.find_first_not_of(L" \t\n") == std::wstring::npos)
    {
        Status(Tone::Error, "Type some text first.");
        return;
    }
    if (s.name.empty())
    {
        Status(Tone::Error, "Enter a material name (letters, digits and underscores).");
        return;
    }
    Ui()->SetEditText(w.name, s.name.c_str());
    std::wstring gdtPath = Utf8W(s.gdt.empty() ? WUtf8(DefaultGdt()) : s.gdt);
    if (gdtPath.size() < 5 || Lower(WUtf8(gdtPath.substr(gdtPath.size() - 4))) != ".gdt")
    {
        Status(Tone::Error, "The GDT path must end in .gdt.");
        return;
    }
    if (gdtPath.find(L':') == std::wstring::npos && gdtPath.rfind(L"\\\\", 0) != 0) gdtPath = Join(root, gdtPath);
    // The image lives under the BO3 root so the GDT can name it relative to it, like Treyarch's own.
    std::string folder = rp::ConfigGet("output", "image_folder", "texture_assets\\text_textures");
    for (char& c : folder)
        if (c == '/') c = '\\';
    while (!folder.empty() && (folder.back() == '\\')) folder.pop_back();
    if (folder.empty() || folder.find(':') != std::string::npos || folder.find("..") != std::string::npos)
    {
        Status(Tone::Error, "image_folder in TextTextures.ini must be a folder inside the BO3 folder, e.g. texture_assets\\text_textures.");
        return;
    }
    Job job;
    job.s = s;
    job.root = root;
    job.gdtPath = gdtPath;
    job.imagePath = Join(Join(root, Utf8W(folder)), Utf8W(s.name) + L".png");
    job.asset.material = s.name;
    job.asset.image = "i_" + s.name;
    job.asset.baseImageRelative = folder + "\\" + s.name + ".png";
    job.asset.kind = s.kind;
    if (s.glow != GlowMode::Off)
    {
        job.asset.glowImage = "i_" + s.name + "_glow";
        job.asset.glowImageRelative = folder + "\\" + s.name + "_glow.png";
        job.asset.glowScale = s.glowScale;
        job.glowPath = Join(Join(root, Utf8W(folder)), Utf8W(s.name) + L"_glow.png");
    }
    job.asset.surfaceType = s.surface;
    job.asset.usage = rp::ConfigGet("texture", "usage", "");
    job.applyAfter = Checked(w.applyAfter);
    if (s.kind != MaterialKind::Opaque && !Checked(w.transparent))
        rp::Log("note: %s is a see-through material kind but the background is opaque", s.name.c_str());
    SaveSettings();
    if (g_worker.joinable()) g_worker.join();
    SetBusy(true);
    Status(Tone::Busy, "Creating " + s.name + ": writing the texture and GDT, then updating BO3's asset database (gdtdb)...");
    g_worker = std::thread(RunJob, std::move(job));
}
void OnCreate(void*) { StartCreate(); }

// ---- 3D text model ----------------------------------------------------------------------------------------------------

struct Job3D
{
    Settings s;
    std::wstring root, gdtPath, exportDir;
    std::string imageFolder, modelFolder, name;
    double lineHeight = 32, depth = 4, tolerance = 0.6;
    bool backFace = true, collision = true, snap = true, place = true, cleanExport = false;
    ModelPivot pivot = ModelPivot::BackBottomCentre;
    std::string frontMaterial, sideMaterial; // empty = a solid-colour material made here
    uint32_t frontColor = 0xFFFFFFFF, sideColor = 0xFF6E6E6E;
};

// A 4x4 PNG of one colour and its image + material entries (no texture-browser category: they belong to the model).
bool WriteSolidMaterial(const Job3D& job, const std::string& material, uint32_t argb, std::string& entries, std::string& error, bool glow = false)
{
    uint8_t px[4 * 4 * 4];
    for (int i = 0; i < 16; ++i)
    {
        px[i * 4] = (argb >> 16) & 255;
        px[i * 4 + 1] = (argb >> 8) & 255;
        px[i * 4 + 2] = argb & 255;
        px[i * 4 + 3] = 255;
    }
    std::vector<uint8_t> png = EncodePng(4, 4, px);
    std::wstring dir = Join(job.root, Utf8W(job.imageFolder));
    if (!EnsureDirectory(dir))
    {
        error = "Could not create " + WUtf8(dir) + ".";
        return false;
    }
    if (!WriteFileAtomic(Join(dir, Utf8W(material) + L".png"), png.data(), png.size(), false, error)) return false;
    TextureAsset a;
    a.material = material;
    a.image = "i_" + material;
    a.baseImageRelative = job.imageFolder + "\\" + material + ".png";
    a.kind = MaterialKind::Opaque;
    a.surfaceType = job.s.surface;
    a.usage = "<not in editor>";
    if (glow)
    {
        a.glowImage = a.image; // the colour itself glows
        a.glowScale = job.s.glowScale;
    }
    entries += BuildImageEntry(a) + BuildMaterialEntry(a);
    return true;
}

void Run3DJob(Job3D job)
{
    JobResult r;
    r.isModel = true;
    r.model = job.name;
    r.placeModel = job.place;
    r.snap = job.snap;
    r.pivot = job.pivot;
    auto post = [&r] {
        JobResult copy = r;
        rp::PostToMainThread([copy] { Finish(copy); });
    };
    auto fail = [&](const std::string& m) {
        r.message = m;
        post();
    };
    // Names this build writes, checked against other GDTs.
    std::vector<std::pair<std::string, const char*>> names = {{job.name, "xmodel"}};
    std::string frontMat = job.frontMaterial.empty() ? job.name + "_front" : job.frontMaterial;
    std::string sideMat = job.sideMaterial.empty() ? job.name + "_side" : job.sideMaterial;
    if (job.frontMaterial.empty()) names.push_back({frontMat, "material"}), names.push_back({"i_" + frontMat, "image"});
    if (job.sideMaterial.empty()) names.push_back({sideMat, "material"}), names.push_back({"i_" + sideMat, "image"});
    std::string target = NormalizePath(job.gdtPath, job.root);
    for (auto& n : names)
    {
        std::string other;
        if (GdtDbFind(job.root, n.first, n.second, other) && !other.empty() && NormalizePath(Utf8W(other), job.root) != target)
            return fail(std::string(n.second) + " " + n.first + " already exists in " + other + ". Choose another name.");
    }
    for (const std::string& m : {job.frontMaterial, job.sideMaterial})
    {
        std::string where;
        if (!m.empty() && GdtDbFind(job.root, m, "material", where) && where.empty())
            return fail("Material " + m + " is not in BO3's asset database. Leave the field empty for a solid colour, or check the name.");
    }
    // Outlines -> polygons -> mesh.
    std::vector<Contour> contours;
    double lineHeightPx = 0;
    std::string err;
    if (!TextOutlines(job.s.style, contours, lineHeightPx, err)) return fail(err);
    std::vector<GlyphPolygon> polys = BuildPolygons(contours, job.tolerance);
    if (polys.empty() || lineHeightPx <= 0) return fail("The text has no visible letters.");
    ModelOptions opt;
    opt.scale = job.lineHeight / lineHeightPx;
    opt.depth = job.depth;
    opt.backFace = job.backFace;
    opt.pivot = job.pivot;
    opt.frontMaterial = frontMat;
    opt.sideMaterial = sideMat;
    opt.backMaterial = sideMat;
    std::vector<std::string> materials;
    ModelStats st;
    std::vector<MeshTri> mesh = BuildTextMesh(polys, opt, materials, st);
    if (mesh.empty()) return fail("The text produced no geometry.");
    r.modelHeight = st.height;
    // Solid-colour materials.
    std::string entries;
    if (job.frontMaterial.empty() && !WriteSolidMaterial(job, frontMat, job.frontColor, entries, err, job.s.glow != GlowMode::Off)) return fail(err);
    if (job.sideMaterial.empty() && sideMat != frontMat && !WriteSolidMaterial(job, sideMat, job.sideColor, entries, err)) return fail(err);
    // Export and compile.
    if (!CompileXModel(job.root, job.exportDir, job.name, WriteXModelExport(mesh, materials), job.cleanExport,
                       [] { return g_unloading.load(); }, err))
        return fail(err);
    // GDT: the xmodel plus any solid-colour materials, in one write.
    std::string filename = job.modelFolder.empty() ? job.name + ".xmodel_bin" : job.modelFolder + "\\" + job.name + ".xmodel_bin";
    entries += XModelGdtEntry(job.name, filename, job.collision);
    std::vector<std::pair<std::string, std::string>> replace = {{job.name, "xmodel.gdf"}};
    if (job.frontMaterial.empty()) replace.push_back({frontMat, "material.gdf"}), replace.push_back({"i_" + frontMat, "image.gdf"});
    if (job.sideMaterial.empty()) replace.push_back({sideMat, "material.gdf"}), replace.push_back({"i_" + sideMat, "image.gdf"});
    std::string gdt;
    if (FileExists(job.gdtPath) && !ReadWholeFile(job.gdtPath, gdt)) return fail("Could not read " + WUtf8(job.gdtPath) + ".");
    if (!UpsertGdtEntries(gdt, replace, entries, err)) return fail(err);
    size_t gslash = job.gdtPath.find_last_of(L"\\/");
    if (gslash != std::wstring::npos) EnsureDirectory(job.gdtPath.substr(0, gslash));
    if (!WriteFileAtomic(job.gdtPath, gdt.data(), gdt.size(), true, err)) return fail(err);
    rp::Log("3D text %s: %zu triangles, %.1f x %.1f x %.1f units, %zu letter shapes, %zu forced", job.name.c_str(), st.triangles, st.width,
            st.height, st.depth, st.polygons, st.forcedTriangles);
    if (!UpdateGdtDb(job.root, [] { return g_unloading.load(); }, err)) return fail("Model compiled and added to the GDT, but " + err);
    char size[160];
    snprintf(size, sizeof size, "%.1f x %.1f x %.1f units, %zu triangles", st.width, st.height, st.depth, st.triangles);
    r.ok = true;
    r.message = "Built 3D model " + job.name + " (" + size + ").";
    if (st.forcedTriangles)
        r.message += " " + std::to_string(st.forcedTriangles) +
                     " letter(s) have overlapping outlines (usually a font faked in bold or italic) and may show thin seams.";
    post();
}

double EditNumber(ModToolsWidget e, double def, double lo, double hi)
{
    std::string t = rp::EditText(e);
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (t.empty() || end == t.c_str() || !std::isfinite(v)) return def;
    return std::min(hi, std::max(lo, v));
}

void Start3D()
{
    if (g_busy) return;
    Settings s = ReadSettings();
    std::wstring root = GameRoot();
    if (root.empty()) return Status(Tone::Error, "The BO3 folder is unknown. Set root= in RadiantPlugins.ini.");
    if (s.style.text.find_first_not_of(L" \t\n") == std::wstring::npos) return Status(Tone::Error, "Type some text first.");
    if (s.name.empty()) return Status(Tone::Error, "Enter a name (letters, digits and underscores); the model gets that name.");
    Ui()->SetEditText(w.name, s.name.c_str());
    std::wstring gdtPath = Utf8W(s.gdt.empty() ? WUtf8(DefaultGdt()) : s.gdt);
    if (gdtPath.size() < 5 || Lower(WUtf8(gdtPath.substr(gdtPath.size() - 4))) != ".gdt") return Status(Tone::Error, "The GDT path must end in .gdt.");
    if (gdtPath.find(L':') == std::wstring::npos && gdtPath.rfind(L"\\\\", 0) != 0) gdtPath = Join(root, gdtPath);
    auto folderSetting = [](const char* section, const char* key, const char* def) {
        std::string f = rp::ConfigGet(section, key, def);
        for (char& c : f)
            if (c == '/') c = '\\';
        while (!f.empty() && f.back() == '\\') f.pop_back();
        return f;
    };
    Job3D job;
    job.s = s;
    job.root = root;
    job.gdtPath = gdtPath;
    job.name = s.name;
    job.imageFolder = folderSetting("output", "image_folder", "texture_assets\\text_textures");
    job.modelFolder = folderSetting("model", "folder", "text_models");
    for (const std::string& f : {job.imageFolder, job.modelFolder})
        if (f.find(':') != std::string::npos || f.find("..") != std::string::npos)
            return Status(Tone::Error, "image_folder and the [model] folder in TextTextures.ini must be folders inside the BO3 folder.");
    job.exportDir = Join(Join(root, L"model_export"), Utf8W(job.modelFolder));
    job.lineHeight = EditNumber(w.lineHeight, 32, 0.5, 4096);
    job.depth = EditNumber(w.depth, 4, 0.05, 1024);
    job.backFace = Checked(w.backFace);
    job.collision = Checked(w.collision);
    job.snap = Checked(w.snap);
    job.place = Checked(w.place3d);
    job.pivot = ComboIndex(w.pivot, 0) == 1 ? ModelPivot::Centre : ModelPivot::BackBottomCentre;
    job.frontMaterial = SanitizeAssetName(rp::EditText(w.frontMat));
    job.sideMaterial = SanitizeAssetName(rp::EditText(w.sideMat));
    job.frontColor = g_textColor;
    job.sideColor = g_sideColor;
    job.tolerance = std::max(0.05, rp::ConfigGetInt("model", "detail_tolerance_x10", 6) / 10.0);
    job.cleanExport = rp::ConfigGetBool("model", "clean_exports", false);
    SaveSettings();
    if (g_worker.joinable()) g_worker.join();
    SetBusy(true);
    Status(Tone::Busy, "Building 3D model " + s.name + ": outlines, mesh, export2bin, GDT and gdtdb...");
    g_worker = std::thread(Run3DJob, std::move(job));
}
void OnBuild3D(void*) { Start3D(); }

bool PlaceModel(const JobResult& r, std::string& message)
{
    Vec3 cam, fwd;
    if (!CameraView(cam, fwd))
    {
        message = "Could not read the 3D camera; add it with Entity > misc_model, model " + r.model + ".";
        return false;
    }
    double hx = fwd.x, hy = fwd.y, hl = std::sqrt(hx * hx + hy * hy);
    if (hl < 1e-6) hx = 0, hy = 1, hl = 1;
    hx /= hl;
    hy /= hl;
    // The text's front (model +X) faces back toward the camera.
    double fx = -hx, fy = -hy;
    if (r.snap)
    {
        if (std::fabs(fx) >= std::fabs(fy)) fx = fx >= 0 ? 1 : -1, fy = 0;
        else fy = fy >= 0 ? 1 : -1, fx = 0;
        hx = -fx;
        hy = -fy;
    }
    double yaw = std::atan2(fy, fx) * 180 / 3.14159265358979;
    if (yaw < 0) yaw += 360;
    double distance = std::max(16, rp::ConfigGetInt("model", "distance", 160));
    double ox = std::round(cam.x + hx * distance), oy = std::round(cam.y + hy * distance);
    double oz = std::round(r.pivot == ModelPivot::Centre ? cam.z : cam.z - r.modelHeight / 2);
    double origin[3] = {ox, oy, oz};
    if (!PlaceMiscModel(r.model, origin, yaw, "Place 3D text"))
    {
        message = "Radiant refused the misc_model; add it with Entity > misc_model, model " + r.model + ".";
        return false;
    }
    message = "Placed it in front of the camera, facing you (selected; Ctrl+Z undoes it).";
    return true;
}

// ---- fonts ------------------------------------------------------------------------------------------------------------

int CALLBACK FontProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM user)
{
    auto* set = reinterpret_cast<std::set<std::wstring>*>(user);
    if (lf->lfFaceName[0] && lf->lfFaceName[0] != L'@') set->insert(lf->lfFaceName);
    return 1;
}
void LoadFonts()
{
    std::set<std::wstring> names;
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, FontProc, reinterpret_cast<LPARAM>(&names), 0);
    ReleaseDC(nullptr, dc);
    g_fonts.assign(names.begin(), names.end());
    std::sort(g_fonts.begin(), g_fonts.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    if (g_fonts.empty()) g_fonts.push_back(L"Arial");
}

// ---- UI ---------------------------------------------------------------------------------------------------------------

ModToolsWidget Row(ModToolsWidget box, const char* label, ModToolsWidget parent)
{
    ModToolsWidget h = Ui()->HBox();
    Ui()->LayoutSpacing(h, 6);
    ModToolsWidget l = Ui()->Label(label, parent);
    Ui()->SetMinimumSize(l, 78, 0);
    Ui()->BoxAddWidget(h, l, 0);
    Ui()->BoxAddLayout(box, h, 0);
    return h;
}
ModToolsWidget Combo(ModToolsWidget parent, const std::vector<std::string>& items, int current)
{
    ModToolsWidget c = Ui()->ComboBox(parent);
    for (auto& i : items) Ui()->ComboAdd(c, i.c_str());
    Ui()->ComboSetCurrent(c, current);
    return c;
}
ModToolsWidget Slider(ModToolsWidget parent, int lo, int hi, int value)
{
    ModToolsWidget s = Ui()->Slider(1, lo, hi, parent);
    Ui()->SliderSetValue(s, std::min(hi, std::max(lo, value)));
    return s;
}
ModToolsWidget Section(ModToolsWidget box, const char* title, ModToolsWidget parent)
{
    ModToolsWidget l = Ui()->Label((std::string("<b>") + title + "</b>").c_str(), parent);
    Ui()->BoxAddWidget(box, l, 0);
    return l;
}

bool BuildUi()
{
    const ModToolsUiApi* ui = Ui();
    if (!ui || !UiHas(ui->ComboBox) || !UiHas(ui->Slider) || !UiHas(ui->SliderValue) || !rp::Has(&rp::host->OnChanged) ||
        !rp::host->OnChanged)
    {
        rp::Log("this RadiantPlugins host lacks combo boxes, sliders or OnChanged; update MTPlugins");
        return false;
    }
    LoadFonts();
    g_loading = true;
    ModToolsWidget content = ui->Widget(nullptr);
    w.root = content;
    ModToolsWidget v = ui->VBox(content);
    ui->LayoutMargins(v, 8, 6, 8, 6);
    ui->LayoutSpacing(v, 5);
    ui->BoxAddWidget(v, ui->Label("<b>Text Textures</b> <small>type a sign, get a BO3 material</small>", content), 0);

    // Text
    std::string savedText = UnescapeIni(rp::ConfigGet("style", "text", "WELCOME"));
    g_plainText = PlainTextReadable() && rp::ConfigGetBool("ui", "multiline_text_box", true);
    if (g_plainText)
    {
        w.text = ui->PlainTextEdit(content);
        ui->SetMinimumHeight(w.text, 64);
        ui->SetMaximumHeight(w.text, 120);
        ui->PlainSetText(w.text, savedText.c_str());
        ui->SetToolTip(w.text, "The sign text. Press Enter for a new line; long lines wrap to fit.");
    }
    else
    {
        std::string line;
        for (char c : savedText) line += c == '\n' ? std::string("\\n") : std::string(1, c);
        w.text = ui->LineEdit(line.c_str(), content);
        ui->SetToolTip(w.text, "The sign text. Type \\n for a new line; long lines wrap to fit.");
    }
    ui->BoxAddWidget(v, w.text, 0);

    // Font
    Section(v, "Font", content);
    std::vector<std::string> fontNames;
    std::wstring savedFont = Utf8W(rp::ConfigGet("style", "font", "Arial"));
    int fontIndex = 0;
    for (size_t i = 0; i < g_fonts.size(); ++i)
    {
        fontNames.push_back(WUtf8(g_fonts[i]));
        if (_wcsicmp(g_fonts[i].c_str(), savedFont.c_str()) == 0) fontIndex = (int)i;
        else if (fontIndex == 0 && _wcsicmp(g_fonts[i].c_str(), L"Arial") == 0) fontIndex = (int)i;
    }
    ModToolsWidget r = Row(v, "Typeface", content);
    w.font = Combo(content, fontNames, fontIndex);
    ui->BoxAddWidget(r, w.font, 1);

    r = Row(v, "Size", content);
    w.autoFit = ui->CheckBox("Auto-fit", content);
    ui->SetChecked(w.autoFit, rp::ConfigGetBool("style", "auto_fit", true));
    ui->SetToolTip(w.autoFit, "Use the largest size that fits inside the padding.");
    w.size = Slider(content, 8, 512, rp::ConfigGetInt("style", "size", 96));
    w.sizeLabel = ui->Label("", content);
    ui->SetMinimumSize(w.sizeLabel, 86, 0);
    ui->BoxAddWidget(r, w.autoFit, 0);
    ui->BoxAddWidget(r, w.size, 1);
    ui->BoxAddWidget(r, w.sizeLabel, 0);

    r = Row(v, "Style", content);
    w.bold = ui->CheckBox("Bold", content);
    w.italic = ui->CheckBox("Italic", content);
    w.underline = ui->CheckBox("Underline", content);
    ui->SetChecked(w.bold, rp::ConfigGetBool("style", "bold", true));
    ui->SetChecked(w.italic, rp::ConfigGetBool("style", "italic", false));
    ui->SetChecked(w.underline, rp::ConfigGetBool("style", "underline", false));
    ui->BoxAddWidget(r, w.bold, 0);
    ui->BoxAddWidget(r, w.italic, 0);
    ui->BoxAddWidget(r, w.underline, 0);
    ui->BoxAddStretch(r, 1);

    r = Row(v, "Alignment", content);
    w.align = Combo(content, {"Left", "Centre", "Right"}, std::min(2, std::max(0, rp::ConfigGetInt("style", "align", 1))));
    w.valign = Combo(content, {"Top", "Middle", "Bottom"}, std::min(2, std::max(0, rp::ConfigGetInt("style", "valign", 1))));
    ui->BoxAddWidget(r, w.align, 1);
    ui->BoxAddWidget(r, w.valign, 1);

    r = Row(v, "Padding", content);
    w.padding = Slider(content, 0, 256, rp::ConfigGetInt("style", "padding", 32));
    w.paddingLabel = ui->Label("", content);
    ui->SetMinimumSize(w.paddingLabel, 86, 0);
    ui->SetToolTip(w.padding, "Space kept clear around the text, in texture pixels.");
    ui->BoxAddWidget(r, w.padding, 1);
    ui->BoxAddWidget(r, w.paddingLabel, 0);

    r = Row(v, "Outline", content);
    w.outline = Slider(content, 0, 32, rp::ConfigGetInt("style", "outline", 0));
    w.outlineLabel = ui->Label("", content);
    ui->SetMinimumSize(w.outlineLabel, 86, 0);
    ui->BoxAddWidget(r, w.outline, 1);
    ui->BoxAddWidget(r, w.outlineLabel, 0);

    // Colours
    Section(v, "Colours", content);
    g_textColor = ParseHex(rp::ConfigGet("style", "text_color", "#FFFFFF"), 0xFFFFFFFF);
    g_outlineColor = ParseHex(rp::ConfigGet("style", "outline_color", "#000000"), 0xFF000000);
    g_bgColor = ParseHex(rp::ConfigGet("style", "background_color", "#1E3A5F"), 0xFF1E3A5F);
    struct ColourRow
    {
        const char* label;
        ModToolsWidget* swatch;
        ModToolsCallback pick;
    };
    ModToolsWidget bgRow = nullptr;
    for (ColourRow c : {ColourRow{"Text", &w.textSwatch, OnPickText}, ColourRow{"Outline", &w.outlineSwatch, OnPickOutline},
                        ColourRow{"Background", &w.bgSwatch, OnPickBackground}})
    {
        r = Row(v, c.label, content);
        *c.swatch = ui->Label("", content);
        ModToolsWidget b = ui->PushButton("Pick...", content);
        rp::host->OnClicked(rp::self, b, c.pick, nullptr);
        ui->BoxAddWidget(r, *c.swatch, 1);
        ui->BoxAddWidget(r, b, 0);
        bgRow = r;
    }
    w.transparent = ui->CheckBox("Transparent", content);
    ui->SetChecked(w.transparent, rp::ConfigGetBool("style", "transparent", false));
    ui->SetToolTip(w.transparent, "No background: only the letters (and outline) are drawn. Use with Decal or Cut-out.");
    ui->BoxAddWidget(bgRow, w.transparent, 0);

    // Texture
    Section(v, "Texture", content);
    std::vector<std::string> sizes;
    for (int s : kSizes) sizes.push_back(std::to_string(s));
    r = Row(v, "Size (px)", content);
    w.width = Combo(content, sizes, SizeIndex(rp::ConfigGetInt("texture", "width", 1024), 4));
    w.height = Combo(content, sizes, SizeIndex(rp::ConfigGetInt("texture", "height", 512), 3));
    ui->BoxAddWidget(r, w.width, 1);
    ui->BoxAddWidget(r, ui->Label("x", content), 0);
    ui->BoxAddWidget(r, w.height, 1);
    r = Row(v, "Material", content);
    w.kind = Combo(content, {kKinds[0], kKinds[1], kKinds[2]}, std::min(2, std::max(0, rp::ConfigGetInt("texture", "kind", 0))));
    ui->BoxAddWidget(r, w.kind, 1);
    r = Row(v, "Glow", content);
    w.glow = Combo(content, {"Off", "Letters glow", "Letters and outline glow", "Whole sign glows (lightbox)"},
                   std::min(3, std::max(0, rp::ConfigGetInt("texture", "glow", 0))));
    ui->SetToolTip(w.glow, "Makes an emissive material: what glows lights up in its own colour, even in the dark. "
                           "The 3D model's front glows too when its front material is made here.");
    w.glowScale = ui->LineEdit(rp::ConfigGet("texture", "glow_brightness", "8").c_str(), content);
    ui->SetMaximumWidth(w.glowScale, 50);
    ui->SetToolTip(w.glowScale, "Glow brightness (the material's scaleRGB). Treyarch's signs use 4 to 32, mostly 8.");
    ui->BoxAddWidget(r, w.glow, 1);
    ui->BoxAddWidget(r, ui->Label("brightness", content), 0);
    ui->BoxAddWidget(r, w.glowScale, 0);
    r = Row(v, "Surface", content);
    std::vector<std::string> surfaces(kSurfaces, kSurfaces + kSurfaceCount);
    std::string savedSurface = rp::ConfigGet("texture", "surface", "<none>");
    int surfaceIndex = 0;
    for (int i = 0; i < kSurfaceCount; ++i)
        if (savedSurface == kSurfaces[i]) surfaceIndex = i;
    w.surface = Combo(content, surfaces, surfaceIndex);
    ui->SetToolTip(w.surface, "What bullets and footsteps sound and look like on it in game.");
    ui->BoxAddWidget(r, w.surface, 1);
    r = Row(v, "Name", content);
    w.name = ui->LineEdit(rp::ConfigGet("output", "name", "sign_welcome").c_str(), content);
    ui->SetToolTip(w.name, "Material asset name (the image is i_<name>). Lower case letters, digits and underscores.");
    ui->BoxAddWidget(r, w.name, 1);
    r = Row(v, "GDT", content);
    w.gdt = ui->LineEdit(rp::ConfigGet("output", "gdt", WUtf8(DefaultGdt()).c_str()).c_str(), content);
    ui->SetToolTip(w.gdt, "GDT that receives the image and material entries. Created if missing. Keep it under source_data.");
    ui->SetPlaceholder(w.gdt, WUtf8(DefaultGdt()).c_str());
    ModToolsWidget browse = ui->PushButton("...", content);
    rp::host->OnClicked(rp::self, browse, OnBrowseGdt, nullptr);
    ui->BoxAddWidget(r, w.gdt, 1);
    ui->BoxAddWidget(r, browse, 0);

    // Preview
    Section(v, "Preview", content);
    w.preview = ui->Label("", content);
    ui->SetMinimumHeight(w.preview, 60);
    ui->BoxAddWidget(v, w.preview, 0);
    w.previewInfo = ui->Label("", content);
    ui->BoxAddWidget(v, w.previewInfo, 0);

    // Actions
    r = ui->HBox();
    w.create = ui->PushButton("Create texture", content);
    ui->SetToolTip(w.create, "Write the PNG and GDT entries, then update BO3's asset database. Re-creating a name overwrites it.");
    w.applyAfter = ui->CheckBox("then apply to selection", content);
    ui->SetChecked(w.applyAfter, rp::ConfigGetBool("sign", "apply_after_create", false));
    ui->BoxAddWidget(r, w.create, 0);
    ui->BoxAddWidget(r, w.applyAfter, 0);
    ui->BoxAddStretch(r, 1);
    ui->BoxAddLayout(v, r, 0);

    Section(v, "Use it in the map", content);
    r = Row(v, "Apply to", content);
    w.applyMode = Combo(content, {"The face the camera looks at", "Every face"}, rp::ConfigGetInt("sign", "apply_mode", 0) == 1 ? 1 : 0);
    w.apply = ui->PushButton("Apply to selection", content);
    ui->SetToolTip(w.apply, "Puts the material on the selected brushes, stretched once across the face and readable from the front.");
    ui->BoxAddWidget(r, w.applyMode, 1);
    ui->BoxAddWidget(r, w.apply, 0);
    r = Row(v, "Sign width", content);
    w.signWidth = ui->LineEdit(rp::ConfigGet("sign", "width", "64").c_str(), content);
    ui->SetToolTip(w.signWidth, "Width of a placed sign in world units; its height follows the texture's shape.");
    ui->SetMaximumWidth(w.signWidth, 70);
    w.place = ui->PushButton("Place sign", content);
    ui->SetToolTip(w.place, "Adds a thin brush in front of the camera with the material on the side facing you.");
    ui->BoxAddWidget(r, w.signWidth, 0);
    ui->BoxAddWidget(r, ui->Label("units", content), 0);
    ui->BoxAddStretch(r, 1);
    ui->BoxAddWidget(r, w.place, 0);

    // 3D text model
    Section(v, "3D text model", content);
    ui->BoxAddWidget(v, ui->Label("<small>Extrudes the text above into an XModel, e.g. big lettering on a building. Front = Text colour.</small>", content), 0);
    r = Row(v, "Line height", content);
    w.lineHeight = ui->LineEdit(rp::ConfigGet("model", "line_height", "32").c_str(), content);
    ui->SetToolTip(w.lineHeight, "Height of one line of text in world units (capitals are about 70% of it).");
    ui->SetMaximumWidth(w.lineHeight, 70);
    w.depth = ui->LineEdit(rp::ConfigGet("model", "depth", "4").c_str(), content);
    ui->SetToolTip(w.depth, "How far the letters stick out, in world units.");
    ui->SetMaximumWidth(w.depth, 70);
    ui->BoxAddWidget(r, w.lineHeight, 0);
    ui->BoxAddWidget(r, ui->Label("depth", content), 0);
    ui->BoxAddWidget(r, w.depth, 0);
    ui->BoxAddWidget(r, ui->Label("units", content), 0);
    ui->BoxAddStretch(r, 1);
    r = Row(v, "Sides", content);
    g_sideColor = ParseHex(rp::ConfigGet("model", "side_color", "#6E6E6E"), 0xFF6E6E6E);
    w.sideSwatch = ui->Label("", content);
    ModToolsWidget sidePick = ui->PushButton("Pick...", content);
    rp::host->OnClicked(rp::self, sidePick, OnPickSide, nullptr);
    ui->BoxAddWidget(r, w.sideSwatch, 1);
    ui->BoxAddWidget(r, sidePick, 0);
    r = Row(v, "Materials", content);
    w.frontMat = ui->LineEdit(rp::ConfigGet("model", "front_material", "").c_str(), content);
    w.sideMat = ui->LineEdit(rp::ConfigGet("model", "side_material", "").c_str(), content);
    ui->SetPlaceholder(w.frontMat, "front: text colour");
    ui->SetPlaceholder(w.sideMat, "sides: side colour");
    ui->SetToolTip(w.frontMat, "Optional: an existing material for the letter faces (e.g. a metal). Empty = solid text colour.");
    ui->SetToolTip(w.sideMat, "Optional: an existing material for the sides and back. Empty = solid side colour.");
    ui->BoxAddWidget(r, w.frontMat, 1);
    ui->BoxAddWidget(r, w.sideMat, 1);
    r = Row(v, "Options", content);
    w.backFace = ui->CheckBox("Back", content);
    w.collision = ui->CheckBox("Collision", content);
    w.snap = ui->CheckBox("Snap 90\xC2\xB0", content);
    ui->SetChecked(w.backFace, rp::ConfigGetBool("model", "back_face", true));
    ui->SetChecked(w.collision, rp::ConfigGetBool("model", "collision", true));
    ui->SetChecked(w.snap, rp::ConfigGetBool("model", "snap", true));
    ui->SetToolTip(w.backFace, "Close the back of the letters. Untick for letters flush against a wall.");
    ui->SetToolTip(w.collision, "Bullet collision (BulletCollisionLOD High).");
    ui->SetToolTip(w.snap, "Face the placed model along the nearest world axis instead of exactly at the camera.");
    ui->BoxAddWidget(r, w.backFace, 0);
    ui->BoxAddWidget(r, w.collision, 0);
    ui->BoxAddWidget(r, w.snap, 0);
    ui->BoxAddStretch(r, 1);
    r = Row(v, "Origin", content);
    w.pivot = Combo(content, {"Bottom centre of the back (for walls)", "Centre"}, rp::ConfigGetInt("model", "pivot", 0) == 1 ? 1 : 0);
    ui->BoxAddWidget(r, w.pivot, 1);
    r = ui->HBox();
    w.build3d = ui->PushButton("Build 3D model", content);
    ui->SetToolTip(w.build3d, "Write the XModel export, compile it with export2bin, add it to the GDT and update gdtdb.");
    w.place3d = ui->CheckBox("then place it in front of the camera", content);
    ui->SetChecked(w.place3d, rp::ConfigGetBool("model", "place", true));
    ui->BoxAddWidget(r, w.build3d, 0);
    ui->BoxAddWidget(r, w.place3d, 0);
    ui->BoxAddStretch(r, 1);
    ui->BoxAddLayout(v, r, 0);
    rp::host->OnClicked(rp::self, w.build3d, OnBuild3D, nullptr);

    w.status = ui->Label("", content);
    ui->SetWordWrap(w.status, 1);
    ui->BoxAddWidget(v, w.status, 0);
    ui->BoxAddStretch(v, 1);

    // Events
    rp::host->OnClicked(rp::self, w.create, OnCreate, nullptr);
    rp::host->OnClicked(rp::self, w.apply, OnApply, nullptr);
    rp::host->OnClicked(rp::self, w.place, OnPlace, nullptr);
    for (ModToolsWidget x : {w.font, w.autoFit, w.size, w.bold, w.italic, w.underline, w.align, w.valign, w.padding, w.outline,
                             w.transparent, w.width, w.height, w.kind, w.glow})
        rp::host->OnChanged(rp::self, x, OnValueChanged, nullptr);
    for (ModToolsWidget x : {w.glowScale, w.surface, w.name, w.gdt, w.applyMode, w.applyAfter, w.signWidth, w.lineHeight, w.depth, w.frontMat,
                             w.sideMat, w.backFace, w.collision, w.snap, w.pivot, w.place3d})
        rp::host->OnChanged(rp::self, x, OnEditFinished, nullptr);

    ModToolsWidget paneContent = content;
    if (UiHas(ui->ScrollArea))
        if (ModToolsWidget scroll = ui->ScrollArea(content, nullptr)) paneContent = scroll;
    g_pane = rp::host->CreatePane(rp::self, "Text Textures", paneContent, 380, 640);
    if (g_pane && HostHas(rp::host->TabPaneWith) && !rp::ConfigGetBool("pane", "placed", false))
    {
        rp::host->TabPaneWith(rp::self, g_pane, "Entity Info");
        rp::ConfigSet("pane", "placed", "1");
    }
    if (ModToolsWidget menu = rp::host->AddMenu(rp::self, "Text Textures"))
        if (g_pane) rp::host->AddMenuAction(rp::self, menu, rp::host->PaneToggleAction(rp::self, g_pane));
    g_loading = false;
    g_uiReady = true;
    UpdateSwatches();
    UpdateLabels();
    g_lastText = ReadText();
    Status(Tone::Info, "Type your text, pick a style, then Create texture.");
    return g_pane != nullptr;
}

// ---- plugin callbacks ---------------------------------------------------------------------------------------------------

int OnLoad()
{
    rp::Log("TextTextures " TEXTTEXTURES_VERSION " loading");
    g_winding = rp::ConfigGetInt("map", "winding", 0);
    if (g_winding != 1 && g_winding != -1) g_winding = 0;
    g_faceSuffix = rp::ConfigGet("map", "face_suffix", "");
    if (!BuildUi())
    {
        rp::Log("could not build the pane");
        return 0;
    }
    return 1;
}

void OnTick()
{
    if (!g_uiReady) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastPoll >= 250)
    {
        g_lastPoll = now;
        std::string t = ReadText();
        if (t != g_lastText)
        {
            g_lastText = t;
            MarkDirty();
        }
    }
    if (g_dirty && now - g_lastChange >= 120) RenderPreview();
}

void OnUnload()
{
    g_unloading = true;
    if (g_worker.joinable()) g_worker.join();
    g_uiReady = false;
    if (!g_lastPreviewFile.empty()) DeleteFileW(g_lastPreviewFile.c_str());
    w = Widgets{};
    g_pane = nullptr;
}

void OnSettings()
{
    if (g_pane) rp::host->ShowPane(rp::self, g_pane, 1);
}

const char* OnCommand(const char* cmd)
{
    // Automation channel: "status" returns the last status line.
    static std::string reply;
    if (cmd && strcmp(cmd, "status") == 0)
    {
        reply = g_statusText;
        return reply.c_str();
    }
    return nullptr;
}
} // namespace

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    g_api.size = sizeof g_api;
    g_api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    g_api.name = "TextTextures";
    g_api.displayName = "Text Textures";
    g_api.version = TEXTTEXTURES_VERSION;
    g_api.author = "stattic";
    g_api.description = "Type text in a pane and get a BO3 material of it: signs, labels and decals, applied to brushes or placed as a sign.";
    g_api.OnLoad = OnLoad;
    g_api.OnUnload = OnUnload;
    g_api.OnTick = OnTick;
    g_api.OnSettings = OnSettings;
    g_api.OnCommand = OnCommand;
    g_api.apps = MODTOOLS_APP_RADIANT;
    return &g_api;
}
