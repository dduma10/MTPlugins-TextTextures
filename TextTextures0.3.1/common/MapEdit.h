// MapEdit.h: brush text editing for TextTextures. Radiant's .map brush faces are
//   ( x y z ) ( x y z ) ( x y z ) material width height xshift yshift rotation <rest: lightmap fields>
// and use Quake's axial projection: u = (p.U + xshift) / width, v = (p.V + yshift) / height.
// No Windows or Radiant dependencies, so the tests build it alone.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace tt
{
struct Vec3
{
    double x = 0, y = 0, z = 0;
};

struct FaceLine
{
    size_t lineBegin = 0, lineEnd = 0;     // the whole line in the entity text, without its line break
    size_t paramsBegin = 0, paramsEnd = 0; // "material w h xs ys rot", replaced when the face is retextured
    Vec3 p[3];
    std::string material;
    std::string rest;                      // what follows the rotation (lightmap material and its fields)
};

struct BrushBlock
{
    std::string guid;                      // as written, without quotes or braces changes, lower case
    size_t begin = 0, end = 0;             // from its "{" line to the end of its "}" line
    bool isPatch = false;                  // patches (mesh/curve) are never retextured
    std::vector<FaceLine> faces;
};

// Depth-2 blocks of entity text (as WriteEntityText writes it: braces on their own lines).
std::vector<BrushBlock> ParseBrushBlocks(const std::string& entityText);

struct FacePoly
{
    std::vector<Vec3> corners;
    Vec3 normal;                           // outward
    Vec3 center;
};
// One polygon per face line (empty corners when the plane does not touch the brush). false for a degenerate brush.
// *winding receives +1 when the face points follow Quake's order ((p3 - p1) x (p2 - p1) points out of the brush),
// -1 for the opposite order (what BO3 Radiant writes).
bool BrushPolygons(const BrushBlock& b, std::vector<FacePoly>& out, int* winding = nullptr);
// The winding the brushes in this text use: +1, -1, or 0 when no brush could tell.
int DetectWinding(const std::string& entityText);

// The face the camera looks at: facing it, on the camera's side of its plane, most head-on. -1 when none.
int ChooseCameraFace(const std::vector<FacePoly>& polys, const Vec3& camOrigin, const Vec3& camForward);

// "material width height xshift yshift rotation" (the fields before the face suffix) that stretches the texture once across the face, upright and readable
// from outside the brush (walls read with +Z up, floors and ceilings with the camera's heading up).
std::string FitFaceParams(const FacePoly& face, const std::string& material, const Vec3& camForward);

enum class ApplyMode
{
    CameraFace = 0, // only the face the camera looks at, per brush
    AllFaces = 1,
};

// Rewrites the faces of the brush blocks in entity text (only those whose lower-case guid is in onlyGuids, when
// given). Returns the number of faces changed; changedGuids receives the guids of the brushes that changed.
int ApplyMaterialInText(std::string& entityText, const std::string& material, ApplyMode mode, const Vec3& camOrigin,
                        const Vec3& camForward, const std::set<std::string>* onlyGuids = nullptr,
                        std::vector<std::string>* changedGuids = nullptr);

// Entity text reduced to an import of world brushes: an empty worldspawn block around the brush blocks (only
// those whose lower-case guid is in onlyGuids, when given).
std::string WorldBrushImportText(const std::string& entityText, const std::set<std::string>* onlyGuids = nullptr);

// What follows "material width height xshift yshift rotation" on the first brush face (BO3: "0 lightmap_gray 16384 16384 0 0 0 0").
std::string FaceSuffixFrom(const std::string& entityText);
std::string DefaultFaceSuffix();

// Camera angles (pitch up positive, yaw 90 = +Y, degrees) to a unit forward vector.
Vec3 ForwardFromAngles(double pitchDeg, double yawDeg);

struct SignBrush
{
    std::string mapText;  // import text: empty worldspawn block holding the sign brush
    Vec3 mins, maxs;
};
// A box `width` x `height` x `thickness` units centred `distance` units in front of the camera, standing upright,
// its front face (textured with `material`, fitted once) turned toward the camera along the nearest horizontal axis.
// The other faces get `backMaterial`. `brushGuid` is written as the brush's guid line when not empty.
// `winding` as DetectWinding returns it (0 = BO3's -1).
SignBrush BuildSignBrush(const Vec3& camOrigin, const Vec3& camForward, double distance, double width, double height,
                         double thickness, const std::string& material, const std::string& backMaterial,
                         const std::string& faceSuffix, const std::string& brushGuid, int winding = -1);

// One axis-aligned box brush block ("{" ... "}" with a guid line when brushGuid is not empty), every face in
// `material` at 64 x 64, written in `winding` order (as DetectWinding returns it; 0 = BO3's -1).
std::string BoxBrushText(const Vec3& mins, const Vec3& maxs, const std::string& material, const std::string& faceSuffix,
                         int winding, const std::string& brushGuid);

// Every face material of a brush block starts with `prefix` (e.g. "clip").
bool BrushAllMaterials(const BrushBlock& b, const std::string& prefix);

std::string FormatNumber(double v);
} // namespace tt
