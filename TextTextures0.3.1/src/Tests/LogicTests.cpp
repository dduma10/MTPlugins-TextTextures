// LogicTests.cpp: tests for the parts of TextTextures that need neither Windows nor Radiant.
// Build: g++ -std=c++17 -O1 Tests/LogicTests.cpp MapEdit.cpp GdtWriter.cpp Png.cpp -o logic_tests
#include "GdtWriter.h"
#include "MapEdit.h"
#include <cmath>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

using namespace tt;

static int g_failures = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                       \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

// A world entity as WriteEntityTextWith writes it: a 64 unit cube, a 64x16x32 slab and a patch, in BO3's winding.
static const char* kEntity = R"(// entity 0
{
guid "{11111111-2222-3333-4444-555555555555}"
"classname" "worldspawn"
// brush 0
{
 guid "{AAAAAAAA-0000-0000-0000-000000000001}"
 ( 0 0 64 ) ( 64 0 64 ) ( 0 64 64 ) wall_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 0 64 0 ) ( 64 0 0 ) wall_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 0 0 64 ) ( 0 64 0 ) wall_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 64 0 0 ) ( 64 64 0 ) ( 64 0 64 ) wall_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 64 0 0 ) ( 0 0 64 ) wall_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 64 0 ) ( 0 64 64 ) ( 64 64 0 ) wall_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
}
// brush 1
{
 guid "{BBBBBBBB-0000-0000-0000-000000000002}"
 ( 100 200 32 ) ( 164 200 32 ) ( 100 216 32 ) wall_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 100 200 0 ) ( 100 216 0 ) ( 164 200 0 ) wall_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 100 200 0 ) ( 100 200 32 ) ( 100 216 0 ) wall_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 164 200 0 ) ( 164 216 0 ) ( 164 200 32 ) wall_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 100 200 0 ) ( 164 200 0 ) ( 100 200 32 ) wall_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 100 216 0 ) ( 100 216 32 ) ( 164 216 0 ) wall_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
}
// brush 2
 {
  guid "{CCCCCCCC-0000-0000-0000-000000000003}"
  curve
  {
  toolFlags;
   some_patch_material
   lightmap_gray
   3 3 16 1
   (
	v 0 0 0 t 0 0 1 1
   )
  }
 }
}
)";

struct Params
{
    std::string material;
    double w, h, xs, ys, rot;
};
static Params ParseParams(const std::string& s)
{
    Params p{};
    std::istringstream in(s);
    in >> p.material >> p.w >> p.h >> p.xs >> p.ys >> p.rot;
    return p;
}

// UV of a world point under Radiant's axial projection for a face with this normal.
static void Uv(const Vec3& n, const Params& p, const Vec3& pt, double& u, double& v)
{
    const double axes[6][3][3] = {
        {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},  {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
        {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
    };
    int best = 0;
    double bd = -1e30;
    for (int i = 0; i < 6; ++i)
    {
        double d = n.x * axes[i][0][0] + n.y * axes[i][0][1] + n.z * axes[i][0][2];
        if (d > bd)
        {
            bd = d;
            best = i;
        }
    }
    // Rotation exactly as BrushModeler's FaceAxes applies it.
    double x[3] = {axes[best][1][0], axes[best][1][1], axes[best][1][2]}, y[3] = {axes[best][2][0], axes[best][2][1], axes[best][2][2]};
    if (p.rot != 0)
    {
        double r = p.rot * 3.14159265358979 / 180, si = std::sin(r), co = std::cos(r);
        int sx = x[0] ? 0 : x[1] ? 1 : 2, sy = y[0] ? 0 : y[1] ? 1 : 2;
        double nx = co * x[sx] - si * x[sy], ny = si * x[sx] + co * x[sy];
        x[sx] = nx;
        x[sy] = ny;
        nx = co * y[sx] - si * y[sy];
        ny = si * y[sx] + co * y[sy];
        y[sx] = nx;
        y[sy] = ny;
    }
    auto dot = [](const double a[3], const Vec3& b) { return a[0] * b.x + a[1] * b.y + a[2] * b.z; };
    u = (dot(x, pt) + p.xs) / p.w;
    v = (dot(y, pt) + p.ys) / p.h;
}
static bool Near(double a, double b) { return std::fabs(a - b) < 1e-4; }

// The viewer's top-left and bottom-right corners of a wall face seen from outside must map to (0,0) and (1,1).
static void CheckReadable(const FacePoly& f, const Params& p, const Vec3& camForward)
{
    Vec3 up{0, 0, 1}, right, down;
    bool wall = std::fabs(f.normal.z) < 0.5;
    if (wall)
    {
        Vec3 in{-f.normal.x, -f.normal.y, -f.normal.z};
        right = {in.y * up.z - in.z * up.y, in.z * up.x - in.x * up.z, in.x * up.y - in.y * up.x};
        down = {0, 0, -1};
    }
    else
    {
        double l = std::sqrt(camForward.x * camForward.x + camForward.y * camForward.y);
        Vec3 h{camForward.x / l, camForward.y / l, 0};
        right = {h.y, -h.x, 0};
        down = f.normal.z > 0 ? Vec3{-h.x, -h.y, 0} : Vec3{h.x, h.y, 0};
    }
    // top-left = least right, least down
    auto score = [&](const Vec3& c, double sr, double sd) { return sr * (c.x * right.x + c.y * right.y + c.z * right.z) + sd * (c.x * down.x + c.y * down.y + c.z * down.z); };
    Vec3 tl = f.corners[0], br = f.corners[0];
    for (const Vec3& c : f.corners)
    {
        if (score(c, -1, -1) > score(tl, -1, -1)) tl = c;
        if (score(c, 1, 1) > score(br, 1, 1)) br = c;
    }
    double u, v;
    Uv(f.normal, p, tl, u, v);
    CHECK(Near(u, 0) && Near(v, 0));
    if (!(Near(u, 0) && Near(v, 0))) printf("   top-left uv %.4f %.4f (normal %g %g %g)\n", u, v, f.normal.x, f.normal.y, f.normal.z);
    Uv(f.normal, p, br, u, v);
    CHECK(Near(u, 1) && Near(v, 1));
    if (!(Near(u, 1) && Near(v, 1))) printf("   bottom-right uv %.4f %.4f (normal %g %g %g)\n", u, v, f.normal.x, f.normal.y, f.normal.z);
}

static void TestParse()
{
    auto blocks = ParseBrushBlocks(kEntity);
    CHECK(blocks.size() == 3);
    CHECK(blocks[0].faces.size() == 6 && !blocks[0].isPatch);
    CHECK(blocks[0].guid == "{aaaaaaaa-0000-0000-0000-000000000001}");
    CHECK(blocks[2].isPatch);
    CHECK(blocks[0].faces[0].material == "wall_a");
    CHECK(blocks[0].faces[0].rest == "0 lightmap_gray 16384 16384 0 0 0 0");
    std::string params = std::string(kEntity).substr(blocks[0].faces[0].paramsBegin, blocks[0].faces[0].paramsEnd - blocks[0].faces[0].paramsBegin);
    CHECK(params == "wall_a 64 64 0 0 0");
    CHECK(FaceSuffixFrom(kEntity) == "0 lightmap_gray 16384 16384 0 0 0 0");
}

static void TestPolygonsAndWinding()
{
    auto blocks = ParseBrushBlocks(kEntity);
    std::vector<FacePoly> polys;
    int w = 0;
    CHECK(BrushPolygons(blocks[0], polys, &w));
    CHECK(w == -1);
    CHECK(polys.size() == 6);
    for (auto& p : polys) CHECK(p.corners.size() == 4);
    CHECK(DetectWinding(kEntity) == -1);
    CHECK(!BrushPolygons(blocks[2], polys));
}

static void TestEveryWallReadable()
{
    auto blocks = ParseBrushBlocks(kEntity);
    std::vector<FacePoly> polys;
    BrushPolygons(blocks[1], polys);
    // Stand outside each side of the slab and look at it.
    struct View
    {
        Vec3 origin;
        double pitch, yaw;
        Vec3 expectNormal;
    };
    const View views[] = {
        {{132, 100, 16}, 0, 90, {0, -1, 0}},  // south of it, looking north
        {{132, 400, 16}, 0, 270, {0, 1, 0}},  // north, looking south
        {{0, 208, 16}, 0, 0, {-1, 0, 0}},     // west, looking east
        {{300, 208, 16}, 0, 180, {1, 0, 0}},  // east, looking west
        {{132, 150, 200}, -80, 90, {0, 0, 1}},  // above, looking down, heading north
        {{132, 150, 200}, -80, 180, {0, 0, 1}}, // above, heading west
        {{132, 150, -200}, 80, 0, {0, 0, -1}},  // below, looking up, heading east
        {{132, 150, -200}, 80, 270, {0, 0, -1}}, // below, heading south
    };
    for (const View& view : views)
    {
        Vec3 fwd = ForwardFromAngles(view.pitch, view.yaw);
        int i = ChooseCameraFace(polys, view.origin, fwd);
        CHECK(i >= 0);
        if (i < 0) continue;
        const FacePoly& f = polys[i];
        CHECK(Near(f.normal.x, view.expectNormal.x) && Near(f.normal.y, view.expectNormal.y) && Near(f.normal.z, view.expectNormal.z));
        Params p = ParseParams(FitFaceParams(f, "sign_test", fwd));
        CHECK(p.material == "sign_test");
        if (std::fabs(f.normal.z) < 0.5) CHECK(p.rot == 0);
        CheckReadable(f, p, fwd);
    }
}

static void TestApply()
{
    std::string text = kEntity;
    Vec3 fwd = ForwardFromAngles(0, 90);
    int n = ApplyMaterialInText(text, "sign_hello", ApplyMode::CameraFace, Vec3{32, -300, 32}, fwd);
    CHECK(n == 2); // one face of each brush, the patch untouched
    auto blocks = ParseBrushBlocks(text);
    CHECK(blocks.size() == 3);
    int changed = 0;
    for (auto& b : blocks)
        for (auto& f : b.faces)
            if (f.material == "sign_hello")
            {
                ++changed;
                CHECK(f.rest == "0 lightmap_gray 16384 16384 0 0 0 0");
            }
    CHECK(changed == 2);
    CHECK(text.find("some_patch_material") != std::string::npos);
    std::string all = kEntity;
    CHECK(ApplyMaterialInText(all, "sign_all", ApplyMode::AllFaces, Vec3{}, fwd) == 12);

    std::string filtered = kEntity;
    std::set<std::string> only = {"{bbbbbbbb-0000-0000-0000-000000000002}"};
    std::vector<std::string> changedGuids;
    CHECK(ApplyMaterialInText(filtered, "sign_x", ApplyMode::CameraFace, Vec3{32, -300, 32}, fwd, &only, &changedGuids) == 1);
    CHECK(changedGuids.size() == 1 && changedGuids[0] == "{bbbbbbbb-0000-0000-0000-000000000002}");
    CHECK(ParseBrushBlocks(WorldBrushImportText(filtered, &only)).size() == 1);

    std::string import = WorldBrushImportText(text);
    CHECK(import.rfind("{\n\"classname\" \"worldspawn\"\n", 0) == 0);
    CHECK(import.find("{11111111") == std::string::npos); // the entity's own guid is dropped
    CHECK(ParseBrushBlocks(import).size() == 3);
}

static void TestSignBrush()
{
    for (double yaw : {0.0, 37.0, 90.0, 180.0, 250.0, 300.0})
        for (int winding : {-1, 1})
        {
            Vec3 cam{500, -40, 64}, fwd = ForwardFromAngles(-10, yaw);
            SignBrush s = BuildSignBrush(cam, fwd, 128, 128, 64, 2, "sign_test", "caulk", "", "{DDDDDDDD-0000-0000-0000-000000000004}", winding);
            auto blocks = ParseBrushBlocks(s.mapText);
            CHECK(blocks.size() == 1);
            if (blocks.empty()) continue;
            CHECK(blocks[0].faces.size() == 6);
            std::vector<FacePoly> polys;
            int w = 0;
            CHECK(BrushPolygons(blocks[0], polys, &w));
            CHECK(w == winding);
            int signFace = -1;
            for (size_t i = 0; i < blocks[0].faces.size(); ++i)
                if (blocks[0].faces[i].material == "sign_test") signFace = (int)i;
            CHECK(signFace >= 0);
            if (signFace < 0 || polys.size() != 6) continue;
            const FacePoly& f = polys[signFace];
            CHECK(f.normal.x * fwd.x + f.normal.y * fwd.y < 0); // faces the camera
            CHECK(Near(s.maxs.z - s.mins.z, 64));
            CHECK(Near(std::fabs(s.maxs.x - s.mins.x) + std::fabs(s.maxs.y - s.mins.y), 130));
            size_t a = blocks[0].faces[signFace].paramsBegin, b = blocks[0].faces[signFace].paramsEnd;
            Params p = ParseParams(s.mapText.substr(a, b - a));
            CheckReadable(f, p, fwd);
            CHECK(blocks[0].faces[signFace].rest == "0 lightmap_gray 16384 16384 0 0 0 0");
        }
}

static void TestGdt()
{
    CHECK(SanitizeAssetName("  Sign - Welcome.Home! ") == "sign___welcome_home");
    CHECK(SanitizeAssetName("___") == "");
    CHECK(GdtEscape("texture_assets\\a\\b.png") == "texture_assets\\\\a\\\\b.png");

    TextureAsset a;
    a.material = "sign_welcome";
    a.image = "i_sign_welcome";
    a.baseImageRelative = "texture_assets\\text_textures\\sign_welcome.png";
    a.kind = MaterialKind::Decal;
    std::string img = BuildImageEntry(a), mat = BuildMaterialEntry(a);
    CHECK(img.find("\"i_sign_welcome\" ( \"image.gdf\" )") != std::string::npos);
    CHECK(img.find("\"baseImage\" \"texture_assets\\\\text_textures\\\\sign_welcome.png\"") != std::string::npos);
    CHECK(img.find("\"compressionMethod\" \"compressed high color\"") != std::string::npos);
    CHECK(mat.find("\"colorMap\" \"i_sign_welcome\"") != std::string::npos);
    CHECK(mat.find("\"materialType\" \"lit_decal\"") != std::string::npos);
    CHECK(mat.find("\"usage\" \"decal\"") != std::string::npos);
    a.kind = MaterialKind::Opaque;
    mat = BuildMaterialEntry(a);
    CHECK(mat.find("\"materialType\" \"lit\"") != std::string::npos);
    CHECK(mat.find("\"materialCategory\" \"Geometry\"") != std::string::npos);
    size_t keys = 0;
    for (size_t p = 0; (p = mat.find("\n\t\t\"", p)) != std::string::npos; ++p) ++keys;
    CHECK(keys == 909);

    // Glow: emissive material types by kind, colorMap00 = the glow image, scaleRGB = brightness; key count unchanged.
    TextureAsset g = a;
    g.glowImage = "i_sign_welcome_glow";
    g.glowImageRelative = "texture_assets\\text_textures\\sign_welcome_glow.png";
    g.glowScale = "16";
    std::string gm = BuildMaterialEntry(g);
    CHECK(gm.find("\"materialType\" \"lit_emissive\"") != std::string::npos && gm.find("\"colorMap00\" \"i_sign_welcome_glow\"") != std::string::npos &&
          gm.find("\"scaleRGB\" \"16\"") != std::string::npos && gm.find("\"colorMap\" \"i_sign_welcome\"") != std::string::npos);
    CHECK(gm.find("\"emissiveIncompetence\" \"1\"") != std::string::npos);
    CHECK(BuildMaterialEntry(a).find("\"emissiveIncompetence\" \"0\"") != std::string::npos);
    size_t gkeys = 0;
    for (size_t p = 0; (p = gm.find("\n\t\t\"", p)) != std::string::npos; ++p) ++gkeys;
    CHECK(gkeys == 909);
    g.kind = MaterialKind::Decal;
    CHECK(BuildMaterialEntry(g).find("\"materialType\" \"decal_emissive\"") != std::string::npos);
    CHECK(BuildMaterialEntry(g).find("\"materialCategory\" \"Decal\"") != std::string::npos);
    g.kind = MaterialKind::Cutout;
    CHECK(BuildMaterialEntry(g).find("\"materialType\" \"lit_emissive_transparent\"") != std::string::npos);
    std::string gi = BuildGlowImageEntry(g);
    CHECK(gi.find("\"i_sign_welcome_glow\" ( \"image.gdf\" )") != std::string::npos && gi.find("sign_welcome_glow.png") != std::string::npos &&
          gi.find("\"mipMode\" \"Average\"") != std::string::npos);
    g.glowImage = g.image; // a solid colour that glows: no second image
    CHECK(BuildGlowImageEntry(g).empty());
    CHECK(BuildMaterialEntry(a).find("colorMap00\" \"i_") == std::string::npos);

    std::string err, gdt;
    CHECK(UpsertGdtEntries(gdt, {{"sign_welcome", "material.gdf"}, {"i_sign_welcome", "image.gdf"}}, img + mat, err));
    CHECK(ScanGdtEntries(gdt).size() == 2);
    std::string other = "{\n\t\"keep_me\" ( \"material.gdf\" )\n\t{\n\t\t\"note\" \"has } brace\"\n\t}\n\t\"sign_welcome\" ( \"material.gdf\" )\n\t{\n\t\t\"old\" \"1\"\n\t}\n}\n";
    CHECK(UpsertGdtEntries(other, {{"sign_welcome", "material.gdf"}, {"i_sign_welcome", "image.gdf"}}, img + mat, err));
    auto spans = ScanGdtEntries(other);
    CHECK(spans.size() == 3);
    CHECK(spans.size() == 3 && spans[0].name == "keep_me" && spans[1].name == "i_sign_welcome" && spans[2].name == "sign_welcome");
    CHECK(other.find("\"old\"") == std::string::npos);
    // Upserting again replaces rather than duplicates.
    CHECK(UpsertGdtEntries(other, {{"sign_welcome", "material.gdf"}, {"i_sign_welcome", "image.gdf"}}, img + mat, err));
    CHECK(ScanGdtEntries(other).size() == 3);
    std::string bad = "not a gdt";
    CHECK(!UpsertGdtEntries(bad, {}, img, err));
    FILE* f = fopen("/tmp/tt_test.gdt", "wb");
    if (f)
    {
        fwrite(other.data(), 1, other.size(), f);
        fclose(f);
    }
}

int main()
{
    TestParse();
    TestPolygonsAndWinding();
    TestEveryWallReadable();
    TestApply();
    TestSignBrush();
    TestGdt();
    if (g_failures) printf("%d check(s) failed\n", g_failures);
    else printf("all logic tests passed\n");
    return g_failures ? 1 : 0;
}
