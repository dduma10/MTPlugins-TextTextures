// Radiant.h: Radiant-side helpers shared by the plugins: game folder, view refresh, the XModel/GDT pipeline and
// misc_model placement. Needs modtools_plugin.hpp bound (rp::Bind) first.
#pragma once
#include <functional>
#include <string>

namespace tt
{
std::wstring GameRoot();
// "c:\\full\\lower\\case" form of a path that may be relative to the game root, for comparing GDT locations.
std::string NormalizePath(std::wstring path, const std::wstring& root);

// MAIN THREAD. Redraws the views and, on the Steam Radiant build Brush Modeler knows, asks the 3D camera to reload
// its assets so new images and models show without a restart ([radiant] camera_asset_reload in the plugin's ini).
void RefreshRadiantView();

// Camera position and unit forward vector. MAIN THREAD.
bool CameraView(double origin[3], double forward[3]);

// Writes <exportDir>\<name>.xmodel_export and compiles it with <root>\bin\export2bin.exe. Any thread.
bool CompileXModel(const std::wstring& root, const std::wstring& exportDir, const std::string& name, const std::string& exportText,
                   bool deleteExport, const std::function<bool()>& stop, std::string& error);
// gdtdb.exe /update. Any thread. Returns false (and the output's end in `error`) on failure.
bool UpdateGdtDb(const std::wstring& root, const std::function<bool()>& stop, std::string& error);
// A rigid xmodel.gdf entry; filename is relative to model_export, single backslashes.
std::string XModelGdtEntry(const std::string& name, const std::string& filename, bool collision);

// Imports a misc_model and selects it, as one undo step. MAIN THREAD.
bool PlaceMiscModel(const std::string& model, const double origin[3], double yawDeg, const char* undoLabel);

// A folder setting from the plugin ini, inside the game folder: forward slashes turned back, no trailing slash.
// Empty when it is absolute or climbs out with "..".
std::string RelativeFolderSetting(const char* section, const char* key, const char* def);
} // namespace tt
