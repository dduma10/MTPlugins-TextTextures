// Radiant.cpp: see Radiant.h.
#include "Radiant.h"
#include "GdtWriter.h"
#include "Platform.h"
#include "XModel.h"
#include "modtools_plugin.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cmath>
#include <cstdio>

namespace tt
{
namespace
{
constexpr DWORD kKnownRadiantTimestamp = 0x5DF45023; // the Radiant_modtools.exe build the camera refresh addresses fit

std::string LowerA(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool RadiantBuildIsKnown()
{
    auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kKnownRadiantTimestamp;
}

// Asks the 3D camera to reload its assets (addresses in the one Radiant_modtools.exe build Steam ships; the same
// call Brush Modeler makes after a build).
using ReloadCameraAssetsFn = long long (*)(void*, unsigned char);
#ifdef _MSC_VER
bool ReloadCameraAssets(void** mainWindow, void* reload)
{
    __try
    {
        if (!mainWindow || !*mainWindow || !reload) return false;
        auto* main = static_cast<unsigned char*>(*mainWindow);
        auto* children = *reinterpret_cast<unsigned char**>(main + 64);
        if (!children) return false;
        auto* cam = *reinterpret_cast<unsigned char**>(children + 240);
        if (!cam) return false;
        reinterpret_cast<ReloadCameraAssetsFn>(reload)(cam, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#else
// Compilers without __try: check every pointer is readable memory before following it.
bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}
bool ReloadCameraAssets(void** mainWindow, void* reload)
{
    if (!Readable(mainWindow, sizeof(void*)) || !*mainWindow || !reload) return false;
    auto* main = static_cast<unsigned char*>(*mainWindow);
    if (!Readable(main + 64, sizeof(void*))) return false;
    auto* children = *reinterpret_cast<unsigned char**>(main + 64);
    if (!Readable(children + 240, sizeof(void*))) return false;
    auto* cam = *reinterpret_cast<unsigned char**>(children + 240);
    if (!Readable(cam, 16)) return false;
    reinterpret_cast<ReloadCameraAssetsFn>(reload)(cam, 1);
    return true;
}
#endif
} // namespace

std::wstring GameRoot()
{
    const wchar_t* r = rp::host && rp::host->RadiantRoot ? rp::host->RadiantRoot(rp::self) : nullptr;
    return r ? std::wstring(r) : std::wstring();
}

std::string NormalizePath(std::wstring p, const std::wstring& root)
{
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    bool absolute = p.size() > 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    if (!absolute) p = Join(root, p);
    wchar_t full[MAX_PATH * 2]{};
    if (GetFullPathNameW(p.c_str(), MAX_PATH * 2, full, nullptr)) p = full;
    return LowerA(WUtf8(p));
}

void RefreshRadiantView()
{
    bool reloaded = false;
    if (rp::Has(&rp::host->RadiantAddress) && rp::host->RadiantAddress && RadiantBuildIsKnown() &&
        rp::ConfigGetBool("radiant", "camera_asset_reload", true))
        reloaded = ReloadCameraAssets(static_cast<void**>(rp::host->RadiantAddress(rp::self, 0x1444C6560)),
                                      rp::host->RadiantAddress(rp::self, 0x140238300));
    rp::Log("camera asset reload %s", reloaded ? "requested" : "skipped");
    if (rp::Has(&rp::host->RequestRedraw) && rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D | MODTOOLS_VIEW_2D);
}

bool CameraView(double origin[3], double forward[3])
{
    float o[3]{}, a[3]{};
    if (!rp::host->GetCamera || !rp::host->GetCamera(rp::self, o, a)) return false;
    const double r = 3.14159265358979323846 / 180;
    for (int i = 0; i < 3; ++i) origin[i] = o[i];
    forward[0] = std::cos(a[0] * r) * std::cos(a[1] * r);
    forward[1] = std::cos(a[0] * r) * std::sin(a[1] * r);
    forward[2] = std::sin(a[0] * r);
    return true;
}

bool CompileXModel(const std::wstring& root, const std::wstring& exportDir, const std::string& name, const std::string& exportText,
                   bool deleteExport, const std::function<bool()>& stop, std::string& error)
{
    if (!EnsureDirectory(exportDir))
    {
        error = "Could not create " + WUtf8(exportDir) + ".";
        return false;
    }
    std::wstring exportFile = Join(exportDir, Utf8W(name) + L".xmodel_export");
    std::wstring binFile = Join(exportDir, Utf8W(name) + L".xmodel_bin");
    if (!WriteFileAtomic(exportFile, exportText.data(), exportText.size(), false, error)) return false;
    DeleteFileW(binFile.c_str());
    std::string tail;
    int code = RunProcess(Join(Join(root, L"bin"), L"export2bin.exe"), L"/v \"" + Utf8W(name) + L".xmodel_export\"", exportDir, stop, 300000, &tail);
    if (code != 0 || !FileExists(binFile))
    {
        rp::Log("export2bin exit=%d output tail:\n%s", code, tail.c_str());
        error = "export2bin could not compile the model (exit " + std::to_string(code) + "). See RadiantPlugins.log.";
        return false;
    }
    if (deleteExport) DeleteFileW(exportFile.c_str());
    return true;
}

bool UpdateGdtDb(const std::wstring& root, const std::function<bool()>& stop, std::string& error)
{
    std::string tail;
    int code = RunProcess(Join(Join(root, L"gdtdb"), L"gdtdb.exe"), L"/update", root, stop, 600000, &tail);
    if (code == 0) return true;
    rp::Log("gdtdb /update exit=%d output tail:\n%s", code, tail.c_str());
    error = "gdtdb /update failed (exit " + std::to_string(code) + "). See RadiantPlugins.log.";
    return false;
}

std::string XModelGdtEntry(const std::string& name, const std::string& filename, bool collision)
{
    return "\t\"" + name + "\" ( \"xmodel.gdf\" )\n\t{\n\t\t\"BulletCollisionLOD\" \"" + std::string(collision ? "High" : "None") +
           "\"\n\t\t\"filename\" \"" + GdtEscape(filename) + "\"\n\t\t\"type\" \"rigid\"\n\t}\n";
}

bool PlaceMiscModel(const std::string& model, const double origin[3], double yawDeg, const char* undoLabel)
{
    std::string text = "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"misc_model\"\n\"model\" \"" + model + "\"\n\"origin\" \"" +
                       FormatFloat(origin[0]) + " " + FormatFloat(origin[1]) + " " + FormatFloat(origin[2]) + "\"\n";
    if (std::fabs(yawDeg) > 1e-6) text += "\"angles\" \"0 " + FormatFloat(yawDeg) + " 0\"\n";
    text += "}\n";
    bool undo = rp::Has(&rp::host->BeginUndo) && rp::host->BeginUndo && rp::host->EndUndo;
    rp::host->DeselectAll(rp::self);
    if (undo) rp::host->BeginUndo(rp::self, undoLabel);
    int ok = rp::host->ImportMapText(rp::self, text.c_str(), undoLabel);
    if (undo) rp::host->EndUndo(rp::self);
    if (!ok)
    {
        rp::Log("misc_model import refused:\n%s", text.c_str());
        return false;
    }
    if (rp::Has(&rp::host->MarkMapModified) && rp::host->MarkMapModified) rp::host->MarkMapModified(rp::self);
    RefreshRadiantView();
    return true;
}

std::string RelativeFolderSetting(const char* section, const char* key, const char* def)
{
    std::string f = rp::ConfigGet(section, key, def);
    for (char& c : f)
        if (c == '/') c = '\\';
    while (!f.empty() && f.back() == '\\') f.pop_back();
    if (f.empty() || f.find(':') != std::string::npos || f.find("..") != std::string::npos || f[0] == '\\') return std::string();
    return f;
}
} // namespace tt
