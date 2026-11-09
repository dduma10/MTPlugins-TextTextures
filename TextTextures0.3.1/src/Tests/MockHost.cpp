// MockHost.cpp: loads TextTextures.dll the way RadiantPlugins does and drives it through a fake host (Wine-runnable).
// Widgets are plain records; the "map" is one world brush whose text the plugin reads and replaces.
#include "modtools_plugin.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct W
{
    std::string kind, text;
    int checked = 0, value = 0, current = 0, enabled = 1;
    std::vector<std::string> items;
};
static std::vector<W*> g_widgets;
static W* Make(const char* kind, const char* text = "")
{
    W* w = new W;
    w->kind = kind;
    w->text = text ? text : "";
    g_widgets.push_back(w);
    return w;
}
static W* AsW(ModToolsWidget p) { return static_cast<W*>(p); }
static std::map<std::string, std::string> g_ini;
static std::vector<std::pair<ModToolsCallback, void*>> g_posted;
static std::map<W*, std::pair<ModToolsCallback, void*>> g_clicks;
static std::map<W*, std::pair<ModToolsValueFn, void*>> g_changes;
static std::wstring g_root, g_pluginDir;
static std::string g_imported, g_lastLog;
static int g_deleted = 0, g_failures = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                               \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static const char* kWorldText = "// entity 0\n{\nguid \"{11111111-2222-3333-4444-555555555555}\"\n\"classname\" \"worldspawn\"\n"
                                "// brush 0\n{\n guid \"{AAAAAAAA-0000-0000-0000-000000000001}\"\n"
                                " ( 0 0 64 ) ( 64 0 64 ) ( 0 64 64 ) wall 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
                                " ( 0 0 0 ) ( 0 64 0 ) ( 64 0 0 ) wall 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
                                " ( 0 0 0 ) ( 0 0 64 ) ( 0 64 0 ) wall 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
                                " ( 64 0 0 ) ( 64 64 0 ) ( 64 0 64 ) wall 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
                                " ( 0 0 0 ) ( 64 0 0 ) ( 0 0 64 ) wall 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
                                " ( 0 64 0 ) ( 0 64 64 ) ( 64 64 0 ) wall 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n}\n}\n";
static int kWorld, kBrush, kInst;

// ---- ui ----
static ModToolsWidget uWidget(ModToolsWidget) { return Make("widget"); }
static ModToolsWidget uLabel(const char* t, ModToolsWidget) { return Make("label", t); }
static ModToolsWidget uLineEdit(const char* t, ModToolsWidget) { return Make("lineedit", t); }
static ModToolsWidget uButton(const char* t, ModToolsWidget) { return Make("button", t); }
static ModToolsWidget uCheck(const char* t, ModToolsWidget) { return Make("check", t); }
static ModToolsWidget uPlain(ModToolsWidget) { return Make("plain"); }
static ModToolsWidget uBox(ModToolsWidget) { return Make("layout"); }
static ModToolsWidget uHBox() { return Make("layout"); }
static void uAdd(ModToolsWidget, ModToolsWidget, int) {}
static void uStretch(ModToolsWidget, int) {}
static void uMargins(ModToolsWidget, int, int, int, int) {}
static void uInt(ModToolsWidget, int) {}
static void uSetText(ModToolsWidget w, const char* t) { AsW(w)->text = t ? t : ""; }
static void uPlaceholder(ModToolsWidget, const char*) {}
static size_t uEditText(ModToolsWidget w, char* out, size_t cap)
{
    if (AsW(w)->kind != "lineedit")
    {
        printf("FAIL: EditText called on a %s widget (the real host would read it as a QLineEdit)\n", AsW(w)->kind.c_str());
        ++g_failures;
    }
    const std::string& t = AsW(w)->text;
    if (cap) snprintf(out, cap, "%s", t.c_str());
    return t.size();
}
static void uSetChecked(ModToolsWidget w, int on) { AsW(w)->checked = on; }
static int uIsChecked(ModToolsWidget w) { return AsW(w)->checked; }
static void uSetEnabled(ModToolsWidget w, int on) { AsW(w)->enabled = on; }
static void uMinSize(ModToolsWidget, int, int) {}
static void uTip(ModToolsWidget, const char*) {}
static ModToolsWidget uSlider(int, int lo, int hi, ModToolsWidget) { W* w = Make("slider"); w->value = lo; return w; }
static void uSliderRange(ModToolsWidget, int, int) {}
static void uSliderSet(ModToolsWidget w, int v) { AsW(w)->value = v; }
static int uSliderValue(ModToolsWidget w) { return AsW(w)->value; }
static ModToolsWidget uCombo(ModToolsWidget) { return Make("combo"); }
static void uComboAdd(ModToolsWidget w, const char* t) { AsW(w)->items.push_back(t); }
static void uComboClear(ModToolsWidget w) { AsW(w)->items.clear(); }
static int uComboCurrent(ModToolsWidget w) { return AsW(w)->items.empty() ? -1 : AsW(w)->current; }
static void uComboSet(ModToolsWidget w, int i) { AsW(w)->current = i; }
static ModToolsWidget uScroll(ModToolsWidget, ModToolsWidget) { return Make("scroll"); }
static int uPickColor(ModToolsWidget, uint32_t, const char*, uint32_t* out) { *out = 0xFFFF8800; return 1; }

// ---- host ----
static const wchar_t* hPluginDir(ModToolsPlugin*) { return g_pluginDir.c_str(); }
static const wchar_t* hRoot(ModToolsPlugin*) { return g_root.c_str(); }
static void hLog(ModToolsPlugin*, const char* fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_lastLog = buf;
    printf("  [plugin] %s\n", buf);
}
static void hLogW(ModToolsPlugin*, const wchar_t*, ...) {}
static size_t hConfigGet(ModToolsPlugin*, const char* s, const char* k, const char* def, char* out, size_t cap)
{
    auto it = g_ini.find(std::string(s) + "/" + k);
    std::string v = it != g_ini.end() ? it->second : (def ? def : "");
    if (cap) snprintf(out, cap, "%s", v.c_str());
    return v.size();
}
static int hConfigGetInt(ModToolsPlugin*, const char* s, const char* k, int def)
{
    auto it = g_ini.find(std::string(s) + "/" + k);
    return it != g_ini.end() ? atoi(it->second.c_str()) : def;
}
static void hConfigSet(ModToolsPlugin*, const char* s, const char* k, const char* v) { g_ini[std::string(s) + "/" + k] = v ? v : ""; }
static void hPost(ModToolsPlugin*, ModToolsCallback fn, void* u) { g_posted.push_back({fn, u}); }
static void* hMainWindow(ModToolsPlugin*) { return nullptr; }
static int hPrefabDepth(ModToolsPlugin*) { return 0; }
static int hGetCamera(ModToolsPlugin*, float o[3], float a[3])
{
    o[0] = 32; o[1] = -200; o[2] = 32; // south of the cube, looking north
    a[0] = 0; a[1] = 90; a[2] = 0;
    return 1;
}
static void hForEachEntity(ModToolsPlugin*, ModToolsEntityFn fn, void* u) { fn(&kWorld, u); }
static void hForEachEntityBrush(ModToolsPlugin*, ModToolsEntity, ModToolsBrushFn, void*) {}
static int hEntityGuid(ModToolsPlugin*, ModToolsEntity, char out[40]) { strcpy(out, "{11111111-2222-3333-4444-555555555555}"); return 1; }
static int hBrushGuid(ModToolsPlugin*, ModToolsBrush, char out[40]) { strcpy(out, "{AAAAAAAA-0000-0000-0000-000000000001}"); return 1; }
static ModToolsEntity hBrushOwner(ModToolsPlugin*, ModToolsBrush) { return &kWorld; }
static ModToolsEntity hWorld(ModToolsPlugin*) { return &kWorld; }
static size_t hGetSelection(ModToolsPlugin*, ModToolsSelectedBrush* out, size_t cap)
{
    if (cap)
    {
        memset(out, 0, sizeof *out);
        strcpy(out->brushGuid, "{AAAAAAAA-0000-0000-0000-000000000001}");
        strcpy(out->entityGuid, "{11111111-2222-3333-4444-555555555555}");
    }
    return 1;
}
static char* hWriteText(ModToolsPlugin*, ModToolsEntity) { return _strdup(kWorldText); }
static void hFree(ModToolsPlugin*, char* s) { free(s); }
static int hImport(ModToolsPlugin*, const char* text, const char*) { g_imported = text; return 1; }
static int hDelete(ModToolsPlugin*, const ModToolsBrush* b, size_t n, const char*) { g_deleted += (int)n; return n == 1 && b[0] == &kBrush; }
static size_t hSelInsts(ModToolsPlugin*, ModToolsBrushInst* out, size_t cap) { if (cap) out[0] = &kInst; return 1; }
static void hDeselect(ModToolsPlugin*) {}
static ModToolsPane hCreatePane(ModToolsPlugin*, const char*, ModToolsWidget, int, int) { return Make("pane"); }
static void hShowPane(ModToolsPlugin*, ModToolsPane, int) {}
static ModToolsWidget hToggle(ModToolsPlugin*, ModToolsPane) { return Make("action"); }
static ModToolsWidget hAddMenu(ModToolsPlugin*, const char*) { return Make("menu"); }
static void hAddMenuAction(ModToolsPlugin*, ModToolsWidget, ModToolsWidget) {}
static int hOnClicked(ModToolsPlugin*, ModToolsWidget b, ModToolsCallback fn, void* u) { g_clicks[AsW(b)] = {fn, u}; return 1; }
static int hOnChanged(ModToolsPlugin*, ModToolsWidget wd, ModToolsValueFn fn, void* u) { g_changes[AsW(wd)] = {fn, u}; return 1; }
static int hApp(ModToolsPlugin*) { return MODTOOLS_APP_RADIANT; }
static void hUndo(ModToolsPlugin*, const char*) {}
static void hEndUndo(ModToolsPlugin*) {}
static void hRedraw(ModToolsPlugin*, unsigned) {}
static int hFixed(ModToolsPlugin*, ModToolsEntity) { return 0; }
static ModToolsBrush hBrushOfInst(ModToolsPlugin*, ModToolsBrushInst i) { return i == &kInst ? &kBrush : nullptr; }
static char* hWriteWith(ModToolsPlugin*, ModToolsEntity, const ModToolsBrush* b, size_t n) { return n == 1 && b[0] == &kBrush ? _strdup(kWorldText) : nullptr; }
static void hMarkModified(ModToolsPlugin*) {}
static int hTabPane(ModToolsPlugin*, ModToolsPane, const char*) { return 1; }

static void Pump(int ms)
{
    for (int t = 0; t < ms; t += 20)
    {
        auto posted = g_posted;
        g_posted.clear();
        for (auto& p : posted) p.first(p.second);
        Sleep(20);
    }
}
static W* FindButton(const char* text)
{
    for (W* w : g_widgets)
        if (w->kind == "button" && w->text == text) return w;
    return nullptr;
}
static void Click(const char* text)
{
    W* b = FindButton(text);
    CHECK(b && g_clicks.count(b));
    if (b && g_clicks.count(b)) g_clicks[b].first(g_clicks[b].second);
}
static std::string ReadAll(const std::wstring& p)
{
    std::string s;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        printf("usage: MockHost <TextTextures.dll> <fake BO3 root>\n");
        return 2;
    }
    g_root = argv[2];
    g_pluginDir = g_root + L"\\bin\\plugins\\TextTextures";
    ModToolsUiApi ui{};
    ui.size = sizeof ui;
    ui.Widget = uWidget; ui.Label = uLabel; ui.LineEdit = uLineEdit; ui.PushButton = uButton; ui.CheckBox = uCheck;
    ui.PlainTextEdit = uPlain; ui.VBox = uBox; ui.HBox = uHBox; ui.BoxAddWidget = uAdd; ui.BoxAddLayout = uAdd;
    ui.BoxAddStretch = uStretch; ui.LayoutMargins = uMargins; ui.LayoutSpacing = uInt; ui.SetLabelText = uSetText;
    ui.SetWordWrap = uInt; ui.SetEditText = uSetText; ui.EditText = uEditText; ui.SetChecked = uSetChecked;
    ui.IsChecked = uIsChecked; ui.SetEnabled = uSetEnabled; ui.PlainSetText = uSetText; ui.SetMinimumHeight = uInt;
    ui.SetMaximumHeight = uInt; ui.SetMaximumWidth = uInt; ui.SetMinimumSize = uMinSize; ui.SetToolTip = uTip;
    ui.PickColor = uPickColor; ui.Slider = uSlider; ui.SliderSetRange = uSliderRange; ui.SliderSetValue = uSliderSet;
    ui.SliderValue = uSliderValue; ui.ComboBox = uCombo; ui.ComboAdd = uComboAdd; ui.ComboClear = uComboClear;
    ui.ComboCurrent = uComboCurrent; ui.ComboSetCurrent = uComboSet; ui.ScrollArea = uScroll; ui.SetPlaceholder = uPlaceholder;
    ModToolsHostApi h{};
    h.size = sizeof h;
    h.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    h.hostVersion = 0x010103;
    h.PluginDir = hPluginDir; h.PluginsRoot = hPluginDir; h.RadiantRoot = hRoot; h.Log = hLog; h.LogW = hLogW;
    h.ConfigGet = hConfigGet; h.ConfigGetInt = hConfigGetInt; h.ConfigSet = hConfigSet; h.PostToMainThread = hPost;
    h.MainWindow = hMainWindow; h.PrefabDepth = hPrefabDepth; h.GetCamera = hGetCamera; h.ForEachEntity = hForEachEntity;
    h.ForEachEntityBrush = hForEachEntityBrush; h.EntityGuid = hEntityGuid; h.BrushGuid = hBrushGuid; h.BrushOwner = hBrushOwner;
    h.WorldEntity = hWorld; h.GetSelection = hGetSelection; h.WriteEntityText = hWriteText; h.FreeString = hFree;
    h.ImportMapText = hImport; h.DeleteBrushes = hDelete; h.SelectedBrushInsts = hSelInsts; h.DeselectAll = hDeselect;
    h.ui = &ui; h.CreatePane = hCreatePane; h.ShowPane = hShowPane; h.PaneToggleAction = hToggle; h.AddMenu = hAddMenu;
    h.AddMenuAction = hAddMenuAction; h.OnClicked = hOnClicked; h.App = hApp; h.BeginUndo = hUndo; h.EndUndo = hEndUndo;
    h.RequestRedraw = hRedraw; h.EntityFixedSize = hFixed; h.BrushOfInst = hBrushOfInst; h.WriteEntityTextWith = hWriteWith;
    h.OnChanged = hOnChanged; h.MarkMapModified = hMarkModified; h.TabPaneWith = hTabPane;

    g_ini["output/gdt"] = "";
    g_ini["output/name"] = "sign_welcome";
    HMODULE dll = LoadLibraryW(argv[1]);
    CHECK(dll != nullptr);
    if (!dll) return 1;
    auto entry = reinterpret_cast<ModToolsPluginEntryFn>(GetProcAddress(dll, "ModToolsPluginEntry"));
    CHECK(entry != nullptr);
    const ModToolsPluginApi* api = entry(reinterpret_cast<ModToolsPlugin*>(0x1234), &h);
    CHECK(api && api->size == sizeof(ModToolsPluginApi) && strcmp(api->name, "TextTextures") == 0);
    printf("loaded %s %s\n", api->displayName, api->version);
    CHECK(api->OnLoad() == 1);

    // The text box: the plugin must have used a line edit here (no Qt in this process).
    W* text = nullptr;
    for (W* w : g_widgets)
        if (w->kind == "lineedit" && w->text == "WELCOME") text = w;
    CHECK(text != nullptr);
    if (text) text->text = "AUTHORISED\\nPERSONNEL ONLY";
    for (int i = 0; i < 20; ++i)
    {
        api->OnTick();
        Sleep(20);
    }
    W* preview = nullptr;
    for (W* w : g_widgets)
        if (w->kind == "label" && w->text.find("<img src=\"file:///") == 0) preview = w;
    CHECK(preview != nullptr);
    if (preview) printf("preview label: %s\n", preview->text.c_str());

    // Colour picking goes through ui->PickColor.
    Click("Pick...");
    // Create the texture.
    Click("Create texture");
    Pump(3000);
    std::wstring png = g_root + L"\\texture_assets\\text_textures\\sign_welcome.png";
    std::wstring gdt = g_root + L"\\source_data\\text_textures.gdt";
    std::string pngData = ReadAll(png), gdtData = ReadAll(gdt);
    CHECK(pngData.size() > 100 && pngData.compare(1, 3, "PNG") == 0);
    CHECK(gdtData.find("\"sign_welcome\" ( \"material.gdf\" )") != std::string::npos);
    CHECK(gdtData.find("\"i_sign_welcome\" ( \"image.gdf\" )") != std::string::npos);
    CHECK(gdtData.find("\"baseImage\" \"texture_assets\\\\text_textures\\\\sign_welcome.png\"") != std::string::npos);
    printf("png %zu bytes, gdt %zu bytes, ran gdtdb: %s\n", pngData.size(), gdtData.size(),
           ReadAll(g_root + L"\\gdtdb\\ran.txt").c_str());
    CHECK(ReadAll(g_root + L"\\gdtdb\\ran.txt").find("/update") != std::string::npos);

    // Apply to the selected cube: the -Y face (toward the camera) gets the material, fitted.
    Click("Apply to selection");
    CHECK(g_deleted == 1);
    CHECK(g_imported.find("sign_welcome 64 64 0 64 0 0 lightmap_gray") != std::string::npos);
    printf("imported after apply:\n%s\n", g_imported.c_str());

    // Place a sign.
    g_imported.clear();
    Click("Place sign");
    CHECK(g_imported.find("sign_welcome") != std::string::npos && g_imported.find("caulk") != std::string::npos);
    printf("imported sign:\n%s\n", g_imported.c_str());

    // Build the 3D model and place it.
    g_imported.clear();
    Click("Build 3D model");
    Pump(8000);
    std::wstring exportFile = g_root + L"\\model_export\\text_models\\sign_welcome.xmodel_export";
    std::string exportText = ReadAll(exportFile), bin = ReadAll(g_root + L"\\model_export\\text_models\\sign_welcome.xmodel_bin");
    gdtData = ReadAll(gdt);
    CHECK(exportText.rfind("MODEL\nVERSION 6\n", 0) == 0);
    CHECK(exportText.find("MATERIAL 0 \"sign_welcome_front\"") != std::string::npos);
    CHECK(exportText.find("MATERIAL 1 \"sign_welcome_side\"") != std::string::npos);
    CHECK(bin.rfind("fake bin", 0) == 0);
    CHECK(gdtData.find("\"sign_welcome\" ( \"xmodel.gdf\" )") != std::string::npos);
    CHECK(gdtData.find("\"filename\" \"text_models\\\\sign_welcome.xmodel_bin\"") != std::string::npos);
    CHECK(gdtData.find("\"sign_welcome_front\" ( \"material.gdf\" )") != std::string::npos);
    CHECK(gdtData.find("\"baseImage\" \"texture_assets\\\\text_textures\\\\sign_welcome_front.png\"") != std::string::npos);
    CHECK(gdtData.find("\"sign_welcome\" ( \"material.gdf\" )") != std::string::npos); // the 2D texture is still there
    CHECK(g_imported.find("\"classname\" \"misc_model\"") != std::string::npos && g_imported.find("\"model\" \"sign_welcome\"") != std::string::npos);
    // Camera at (32,-200,32) looking north: the model 160 units ahead, facing south (yaw 270).
    CHECK(g_imported.find("\"angles\" \"0 270 0\"") != std::string::npos);
    CHECK(g_imported.find("\"origin\" \"32 -40 ") != std::string::npos);
    printf("export %zu bytes, bin: %s\nimported model:\n%s\n", exportText.size(), bin.c_str(), g_imported.c_str());
    FILE* xf = fopen("Z:\\tmp\\model.xmodel_export", "wb");
    if (xf) { fwrite(exportText.data(), 1, exportText.size(), xf); fclose(xf); }

    // Glow: letters glow, brightness 16 -> a glow PNG, its image entry and an emissive material; the 3D front glows too.
    W* glow = nullptr;
    W* nameEdit = nullptr;
    for (W* w : g_widgets)
    {
        if (w->kind == "combo" && w->items.size() == 4 && w->items[1] == "Letters glow") glow = w;
        if (w->kind == "lineedit" && w->text == "sign_welcome") nameEdit = w;
    }
    CHECK(glow && nameEdit);
    if (glow && nameEdit)
    {
        glow->current = 1;
        g_changes[glow].first(g_changes[glow].second, 1);
        for (W* w : g_widgets)
            if (w->kind == "lineedit" && w->text == "8") w->text = "16"; // brightness
        nameEdit->text = "sign_glow";
        Click("Create texture");
        Pump(3000);
        std::string gp = ReadAll(g_root + L"\\texture_assets\\text_textures\\sign_glow_glow.png");
        gdtData = ReadAll(gdt);
        CHECK(gp.size() > 100 && gp.compare(1, 3, "PNG") == 0);
        size_t m = gdtData.find("\"sign_glow\" ( \"material.gdf\" )");
        CHECK(m != std::string::npos);
        std::string mat = m == std::string::npos ? "" : gdtData.substr(m, gdtData.find("\n\t}", m) - m);
        CHECK(mat.find("\"materialType\" \"lit_emissive\"") != std::string::npos && mat.find("\"colorMap00\" \"i_sign_glow_glow\"") != std::string::npos &&
              mat.find("\"scaleRGB\" \"16\"") != std::string::npos && mat.find("\"emissiveIncompetence\" \"1\"") != std::string::npos);
        CHECK(gdtData.find("\"i_sign_glow_glow\" ( \"image.gdf\" )") != std::string::npos &&
              gdtData.find("\"baseImage\" \"texture_assets\\\\text_textures\\\\sign_glow_glow.png\"") != std::string::npos);
        CHECK(g_ini["texture/glow"] == "1" && g_ini["texture/glow_brightness"] == "16");
        Click("Build 3D model");
        Pump(5000);
        gdtData = ReadAll(gdt);
        size_t f3 = gdtData.find("\"sign_glow_front\" ( \"material.gdf\" )");
        CHECK(f3 != std::string::npos);
        std::string front = f3 == std::string::npos ? "" : gdtData.substr(f3, gdtData.find("\n\t}", f3) - f3);
        CHECK(front.find("\"materialType\" \"lit_emissive\"") != std::string::npos && front.find("\"colorMap00\" \"i_sign_glow_front\"") != std::string::npos &&
              front.find("\"emissiveIncompetence\" \"1\"") != std::string::npos);
        size_t s3 = gdtData.find("\"sign_glow_side\" ( \"material.gdf\" )");
        CHECK(s3 != std::string::npos && gdtData.substr(s3, gdtData.find("\n\t}", s3) - s3).find("lit_emissive") == std::string::npos);
    }

    std::string status = api->OnCommand ? api->OnCommand("status") : "";
    printf("status: %s\n", status.c_str());
    api->OnUnload();
    printf(g_failures ? "%d mock-host check(s) failed\n" : "mock host: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
