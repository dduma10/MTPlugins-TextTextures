// Text3DTests.cpp: triangulation and extrusion checks on shapes like glyphs (holes, islands, curves, concave).
// Build: g++ -std=c++17 -O1 Tests/Text3DTests.cpp Text3D.cpp TextRender.cpp -o text3d_tests
#include "Text3D.h"
#include "TextRender.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>
#include <tuple>

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

static Contour Rect(double x0, double y0, double x1, double y1, bool cw = false)
{
    Contour c = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    if (cw) std::reverse(c.begin(), c.end());
    return c;
}
static Contour Circle(double cx, double cy, double r, int n, bool cw = false)
{
    Contour c;
    for (int i = 0; i < n; ++i)
    {
        double a = (cw ? -1 : 1) * 2 * 3.14159265358979 * i / n;
        c.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return c;
}

// Exact area of the even-odd fill of a set of polygons.
static double ShapeArea(const std::vector<GlyphPolygon>& polys)
{
    double a = 0;
    for (auto& p : polys)
    {
        a += std::fabs(SignedArea(p.outer));
        for (auto& h : p.holes) a -= std::fabs(SignedArea(h));
    }
    return a;
}

static void CheckShape(const char* name, const std::vector<Contour>& contours, size_t expectPolys, double expectArea)
{
    auto polys = BuildPolygons(contours, 0.0);
    CHECK(polys.size() == expectPolys);
    double area = ShapeArea(polys), triArea = 0;
    bool clean = true, ccw = true;
    for (auto& p : polys)
    {
        std::vector<Pt2> pts;
        std::vector<int> tri;
        clean = Triangulate(p, pts, tri) && clean;
        for (size_t k = 0; k + 2 < tri.size(); k += 3)
        {
            Contour t = {pts[tri[k]], pts[tri[k + 1]], pts[tri[k + 2]]};
            double a = SignedArea(t);
            if (a < -1e-9) ccw = false;
            triArea += a;
        }
    }
    bool areaOk = std::fabs(triArea - area) < 1e-6 * std::max(1.0, area) && (expectArea < 0 || std::fabs(area - expectArea) < 1e-3 * std::max(1.0, expectArea));
    CHECK(clean);
    CHECK(ccw);
    CHECK(areaOk);
    printf("%-22s polys=%zu area=%.4f triangles cover %.4f %s\n", name, polys.size(), area, triArea, clean && ccw && areaOk ? "ok" : "BAD");

    // Extrude and check the solid.
    ModelOptions opt;
    opt.scale = 0.5;
    opt.depth = 3;
    std::vector<std::string> mats;
    ModelStats st;
    auto mesh = BuildTextMesh(polys, opt, mats, st);
    CHECK(!mesh.empty());
    CHECK(mats.size() == 2); // front, side (back shares side)
    // Edge manifold check on welded positions.
    std::map<std::tuple<long long, long long, long long>, int> ids;
    auto id = [&](const double p[3]) {
        auto key = std::make_tuple(std::llround(p[0] * 1e5), std::llround(p[1] * 1e5), std::llround(p[2] * 1e5));
        auto it = ids.find(key);
        if (it == ids.end()) it = ids.emplace(key, (int)ids.size()).first;
        return it->second;
    };
    std::map<std::pair<int, int>, int> directed;
    double volume = 0;
    bool windingOk = true;
    for (auto& t : mesh)
    {
        int v[3] = {id(t.p[0]), id(t.p[1]), id(t.p[2])};
        for (int k = 0; k < 3; ++k) directed[{v[k], v[(k + 1) % 3]}]++;
        const double *a = t.p[0], *b = t.p[1], *c = t.p[2];
        double gx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
        double gy = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
        double gz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        double nx = t.n[0][0] + t.n[1][0] + t.n[2][0], ny = t.n[0][1] + t.n[1][1] + t.n[2][1], nz = t.n[0][2] + t.n[1][2] + t.n[2][2];
        if (gx * nx + gy * ny + gz * nz >= 0) windingOk = false;
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
    }
    int bad = 0;
    for (auto& e : directed)
    {
        auto rev = directed.find({e.first.second, e.first.first});
        if (e.second != 1 || rev == directed.end() || rev->second != 1) ++bad;
    }
    double expectVolume = -area * opt.scale * opt.scale * opt.depth; // export winding is inward, so negative
    CHECK(bad == 0);
    CHECK(windingOk);
    CHECK(std::fabs(volume - expectVolume) < 1e-4 * std::fabs(expectVolume) + 1e-6);
    if (bad || !windingOk) printf("   open edges=%d winding=%d volume=%.5f expect %.5f\n", bad, windingOk, volume, expectVolume);
}

// Glow maps: what glows keeps its colour, the rest is black, opaque.
static void TestGlowMap()
{
    TextStyle st;
    st.textArgb = 0xFFFF8000;
    st.outlineArgb = 0xFF0000FF;
    st.backgroundArgb = 0xFF202020;
    st.outlinePx = 1;
    // pixel 0: letter, 1: outline, 2: background, 3: half-covered letter over the outline
    std::vector<uint8_t> text = {255, 0, 0, 128}, outline = {255, 255, 0, 255};
    auto px = [](const Image& im, int i) { return std::vector<int>{im.rgba[i * 4], im.rgba[i * 4 + 1], im.rgba[i * 4 + 2], im.rgba[i * 4 + 3]}; };
    Image l = GlowMap(st, text, outline, 4, 1, GlowMode::Letters);
    CHECK((px(l, 0) == std::vector<int>{255, 128, 0, 255}) && (px(l, 1) == std::vector<int>{0, 0, 0, 255}) && (px(l, 2) == std::vector<int>{0, 0, 0, 255}));
    CHECK(px(l, 3)[0] == 128 && px(l, 3)[2] == 0);
    Image lo = GlowMap(st, text, outline, 4, 1, GlowMode::LettersOutline);
    CHECK((px(lo, 1) == std::vector<int>{0, 0, 255, 255}) && (px(lo, 2) == std::vector<int>{0, 0, 0, 255}) && px(lo, 3)[2] > 100);
    Image ws = GlowMap(st, text, outline, 4, 1, GlowMode::WholeSign);
    CHECK((px(ws, 2) == std::vector<int>{32, 32, 32, 255}) && (px(ws, 0) == std::vector<int>{255, 128, 0, 255}));
    st.backgroundArgb = 0x00202020; // transparent background: nothing behind the letters glows
    CHECK((px(GlowMap(st, text, outline, 4, 1, GlowMode::WholeSign), 2) == std::vector<int>{0, 0, 0, 255}));
}

int main()
{
    TestGlowMap();
    const double pi = 3.14159265358979;
    CheckShape("square", {Rect(0, 0, 10, 10)}, 1, 100);
    CheckShape("square, CW input", {Rect(0, 0, 10, 10, true)}, 1, 100);
    CheckShape("frame (hole)", {Rect(0, 0, 10, 10), Rect(2, 2, 8, 8)}, 1, 64);
    CheckShape("ring 'O'", {Circle(0, 0, 10, 96), Circle(0, 0, 6, 64)}, 1, -1);
    CheckShape("two holes 'B'", {Rect(0, 0, 10, 20), Rect(2, 2, 7, 9), Rect(2, 11, 7, 18)}, 1, 200 - 35 - 35);
    CheckShape("island in hole", {Rect(0, 0, 20, 20), Rect(3, 3, 17, 17), Rect(7, 7, 13, 13)}, 2, 400 - 196 + 36);
    CheckShape("two letters", {Rect(0, 0, 4, 10), Rect(6, 0, 10, 10), Circle(20, 5, 5, 40), Circle(20, 5, 2, 24)}, 3, -1);
    // A concave 'E' with a hole whose rightmost point lines up with an outer vertex.
    Contour e = {{0, 0}, {10, 0}, {10, 2}, {3, 2}, {3, 9}, {8, 9}, {8, 11}, {3, 11}, {3, 18}, {10, 18}, {10, 20}, {0, 20}};
    CheckShape("concave 'E'", {e}, 1, 200 - 7 * 7 - 2 * 2 - 7 * 7);
    CheckShape("hole touching ray", {Rect(0, 0, 10, 10), Contour{{4, 5}, {6, 4}, {6, 6}}}, 1, 98);
    // Several holes in a row at the same height (bridges cross earlier bridges' columns).
    std::vector<Contour> many = {Rect(0, 0, 40, 6)};
    for (int i = 0; i < 6; ++i) many.push_back(Rect(2 + i * 6, 2, 5 + i * 6, 4));
    CheckShape("six holes in a row", many, 1, 240 - 6 * 6);

    // Area expectations for the curved shapes (polygonal circles).
    auto circleArea = [&](double r, int n) { return 0.5 * n * r * r * std::sin(2 * pi / n); };
    {
        auto polys = BuildPolygons({Circle(0, 0, 10, 96), Circle(0, 0, 6, 64)}, 0);
        CHECK(std::fabs(ShapeArea(polys) - (circleArea(10, 96) - circleArea(6, 64))) < 1e-6);
    }
    // Simplification removes collinear points but keeps corners.
    {
        Contour c;
        for (int i = 0; i <= 10; ++i) c.push_back({double(i), 0});
        for (int i = 1; i <= 10; ++i) c.push_back({10, double(i)});
        c.push_back({0, 10});
        auto polys = BuildPolygons({c}, 0.01);
        CHECK(polys.size() == 1 && polys[0].outer.size() == 4);
    }
    // The export text: one bone, welded positions, faces referencing them.
    {
        auto polys = BuildPolygons({Rect(0, 0, 10, 10)}, 0);
        ModelOptions opt;
        std::vector<std::string> mats;
        ModelStats st;
        auto mesh = BuildTextMesh(polys, opt, mats, st);
        std::string x = WriteXModelExport(mesh, mats);
        CHECK(x.rfind("MODEL\nVERSION 6\n", 0) == 0);
        CHECK(x.find("NUMVERTS 8\n") != std::string::npos); // a box welds to 8 corners
        CHECK(x.find("NUMFACES 12\n") != std::string::npos);
        CHECK(x.find("MATERIAL 0 \"front\"") != std::string::npos && x.find("MATERIAL 1 \"side\"") != std::string::npos);
        CHECK(std::fabs(st.width - 10) < 1e-9 && std::fabs(st.height - 10) < 1e-9);
        // Back-bottom-centre pivot: X from 0 to depth, Y centred, Z from 0.
        CHECK(x.find("OFFSET 4 -5 0\n") != std::string::npos && x.find("OFFSET 0 5 10\n") != std::string::npos);
    }
    printf(g_failures ? "%d check(s) failed\n" : "all 3D text tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
