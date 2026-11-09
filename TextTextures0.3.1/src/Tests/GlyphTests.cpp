// GlyphTests.cpp (Windows/Wine): real font outlines through BuildPolygons -> Triangulate -> BuildTextMesh, checking
// area coverage and that each solid is closed. Writes the last mesh as OBJ for a visual check.
#include "Text3D.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
using namespace tt;
static int CALLBACK Proc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD type, LPARAM u)
{
    if ((type & TRUETYPE_FONTTYPE) && lf->lfFaceName[0] != L'@') ((std::set<std::wstring>*)u)->insert(lf->lfFaceName);
    return 1;
}
int wmain(int argc, wchar_t** argv)
{
    std::set<std::wstring> fonts;
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, Proc, (LPARAM)&fonts, 0);
    ReleaseDC(nullptr, dc);
    int failures = 0;
    std::vector<MeshTri> lastMesh;
    for (const auto& f : fonts)
        for (int bold = 0; bold < 2; ++bold)
        {
            TextStyle s;
            s.font = f;
            s.bold = bold;
            s.italic = bold;
            s.text = L"BO3 Zombies! @#&%8$\nQRgB aeiou 0123456789";
            std::vector<Contour> cs;
            double lh;
            std::string err;
            if (!TextOutlines(s, cs, lh, err)) { printf("%ls: %s\n", f.c_str(), err.c_str()); continue; }
            auto polys = BuildPolygons(cs, 0.6);
            double area = 0, tri = 0;
            int forced = 0;
            for (auto& p : polys)
            {
                area += std::fabs(SignedArea(p.outer));
                for (auto& h : p.holes) area -= std::fabs(SignedArea(h));
                std::vector<Pt2> pts; std::vector<int> t;
                if (!Triangulate(p, pts, t)) ++forced;
                for (size_t k = 0; k + 2 < t.size(); k += 3) tri += SignedArea(Contour{pts[t[k]], pts[t[k + 1]], pts[t[k + 2]]});
            }
            ModelOptions o; o.scale = 32.0 / lh; o.depth = 4;
            std::vector<std::string> mats; ModelStats st;
            auto mesh = BuildTextMesh(polys, o, mats, st);
            std::map<std::tuple<long long,long long,long long>, int> ids;
            auto id = [&](const double* p) { auto k = std::make_tuple(std::llround(p[0]*1e4), std::llround(p[1]*1e4), std::llround(p[2]*1e4)); auto it = ids.find(k); if (it == ids.end()) it = ids.emplace(k, (int)ids.size()).first; return it->second; };
            std::map<std::pair<int,int>, int> dir;
            for (auto& m : mesh) { int v[3] = {id(m.p[0]), id(m.p[1]), id(m.p[2])}; for (int k = 0; k < 3; ++k) dir[{v[k], v[(k+1)%3]}]++; }
            int open = 0;
            for (auto& e : dir) { auto r = dir.find({e.first.second, e.first.first}); if (e.second != 1 || r == dir.end() || r->second != 1) ++open; }
            double cover = area > 0 ? tri / area : 0;
            bool ok = std::fabs(cover - 1) < 1e-6 && forced == 0;
            // Open edges can only come from overlapping contours in the font itself; report them separately.
            printf("%-28ls %s contours=%3zu polys=%3zu tris=%6zu size=%.1fx%.1f cover=%.8f forced=%d open_edges=%d %s\n", f.c_str(), bold ? "BI" : "  ",
                   cs.size(), polys.size(), mesh.size(), st.width, st.height, cover, forced, open, ok ? "ok" : "BAD");
            if (!ok) ++failures;
            lastMesh = mesh;
            if (argc > 1 && f == argv[1] && !bold) lastMesh = mesh, s.text = s.text;
            if (argc > 1 && f == argv[1] && !bold)
            {
                FILE* obj = _wfopen(L"glyphs.obj", L"wb");
                for (auto& m : mesh) for (int k = 0; k < 3; ++k) fprintf(obj, "v %f %f %f\n", m.p[k][0], m.p[k][1], m.p[k][2]);
                for (size_t i = 0; i < mesh.size(); ++i) fprintf(obj, "f %zu %zu %zu\n", i*3+1, i*3+2, i*3+3);
                fclose(obj);
            }
        }
    printf(failures ? "%d font run(s) failed\n" : "all fonts ok\n", failures);
    return failures ? 1 : 0;
}
