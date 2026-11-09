// Text3D.cpp: see Text3D.h.
#include "Text3D.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tt
{
namespace
{
double Cross(const Pt2& o, const Pt2& a, const Pt2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
double Cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }
double Dist2(const Pt2& a, const Pt2& b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }

double SegmentDistance(const Pt2& p, const Pt2& a, const Pt2& b)
{
    double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = std::max(0.0, std::min(1.0, t));
    double qx = a.x + t * dx - p.x, qy = a.y + t * dy - p.y;
    return std::sqrt(qx * qx + qy * qy);
}

Contour Simplify(const Contour& in, double tol)
{
    Contour c;
    double dup = std::max(1e-9, tol * 0.05);
    for (const Pt2& p : in)
        if (c.empty() || Dist2(p, c.back()) > dup * dup) c.push_back(p);
    while (c.size() > 1 && Dist2(c.front(), c.back()) <= dup * dup) c.pop_back();
    if (tol <= 0) return c;
    // Drop points that lie (almost) on the segment between their neighbours. Never two neighbours in one pass.
    for (bool changed = true; changed && c.size() > 3;)
    {
        changed = false;
        std::vector<bool> drop(c.size(), false);
        for (size_t i = 0; i < c.size(); ++i)
        {
            size_t p = (i + c.size() - 1) % c.size(), n = (i + 1) % c.size();
            if (drop[p]) continue;
            if (SegmentDistance(c[i], c[p], c[n]) < tol)
            {
                drop[i] = true;
                changed = true;
            }
        }
        Contour k;
        for (size_t i = 0; i < c.size(); ++i)
            if (!drop[i]) k.push_back(c[i]);
        if (k.size() < 3) break;
        c.swap(k);
    }
    return c;
}

bool PointInContour(const Pt2& p, const Contour& c)
{
    bool in = false;
    for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++)
    {
        const Pt2 &a = c[i], &b = c[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// Majority vote of three points, so a contour that touches another at one vertex is still placed right.
bool ContourInside(const Contour& inner, const Contour& outer)
{
    int votes = 0;
    for (int k = 0; k < 3; ++k)
    {
        const Pt2& a = inner[(inner.size() * k / 3) % inner.size()];
        const Pt2& b = inner[(inner.size() * k / 3 + 1) % inner.size()];
        Pt2 mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
        if (PointInContour(mid, outer)) ++votes;
    }
    return votes >= 2;
}

// Interior wedge test at ring vertex v (counter-clockwise ring): does direction v->m point into the polygon?
bool LocallyInside(const Pt2& prev, const Pt2& v, const Pt2& next, const Pt2& m)
{
    double sx = next.x - v.x, sy = next.y - v.y, ex = prev.x - v.x, ey = prev.y - v.y, dx = m.x - v.x, dy = m.y - v.y;
    if (Cross(sx, sy, ex, ey) > 0) return Cross(sx, sy, dx, dy) >= 0 && Cross(dx, dy, ex, ey) >= 0;
    return !(Cross(ex, ey, dx, dy) > 0 && Cross(dx, dy, sx, sy) > 0);
}

bool InTriangleStrict(const Pt2& p, const Pt2& a, const Pt2& b, const Pt2& c, double eps)
{
    return Cross(a, b, p) > eps && Cross(b, c, p) > eps && Cross(c, a, p) > eps;
}
bool InTriangleOrOn(const Pt2& p, const Pt2& a, const Pt2& b, const Pt2& c, double eps)
{
    return Cross(a, b, p) >= -eps && Cross(b, c, p) >= -eps && Cross(c, a, p) >= -eps;
}

// Joins a clockwise hole into the counter-clockwise ring with a two-way bridge from the hole's rightmost point.
void Bridge(Contour& ring, const Contour& hole, double eps)
{
    size_t mi = 0;
    for (size_t i = 1; i < hole.size(); ++i)
        if (hole[i].x > hole[mi].x || (hole[i].x == hole[mi].x && hole[i].y < hole[mi].y)) mi = i;
    const Pt2 m = hole[mi];
    // Nearest ring edge crossed by a ray from m toward +x.
    double bestX = 1e300;
    size_t edge = (size_t)-1;
    const size_t n = ring.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Pt2 &a = ring[k], &b = ring[(k + 1) % n];
        if ((a.y <= m.y && b.y >= m.y) || (b.y <= m.y && a.y >= m.y))
        {
            double x = a.y == b.y ? std::min(a.x, b.x) : a.x + (m.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x >= m.x - eps && x < bestX)
            {
                bestX = x;
                edge = k;
            }
        }
    }
    size_t v = 0;
    if (edge == (size_t)-1)
    {
        // No crossing (degenerate input): take the nearest ring vertex.
        double bd = 1e300;
        for (size_t k = 0; k < n; ++k)
            if (Dist2(ring[k], m) < bd) bd = Dist2(ring[k], m), v = k;
    }
    else
    {
        Pt2 i{bestX, m.y};
        size_t a = edge, b = (edge + 1) % n;
        v = ring[a].x > ring[b].x ? a : b;
        if (Dist2(ring[a], i) <= eps * eps) v = a;
        else if (Dist2(ring[b], i) <= eps * eps) v = b;
        else
        {
            // Another vertex inside triangle (m, i, P) may block the view: take the one at the smallest angle.
            const Pt2 p = ring[v];
            double bestTan = 1e300, bestD = 1e300;
            for (size_t k = 0; k < n; ++k)
            {
                const Pt2& r = ring[k];
                if (k == v || r.x < m.x - eps) continue;
                bool inside = p.y >= m.y ? InTriangleOrOn(r, m, i, p, eps) || InTriangleOrOn(r, m, p, i, eps)
                                         : InTriangleOrOn(r, m, p, i, eps) || InTriangleOrOn(r, m, i, p, eps);
                if (!inside) continue;
                double dx = r.x - m.x;
                double tanAngle = dx > eps ? std::fabs(r.y - m.y) / dx : 1e299;
                double d = Dist2(r, m);
                if (tanAngle < bestTan - 1e-12 || (std::fabs(tanAngle - bestTan) <= 1e-12 && d < bestD))
                {
                    const Pt2& prev = ring[(k + n - 1) % n];
                    const Pt2& next = ring[(k + 1) % n];
                    if (!LocallyInside(prev, r, next, m)) continue;
                    bestTan = tanAngle;
                    bestD = d;
                    v = k;
                }
            }
        }
    }
    // Among ring vertices at the same spot (earlier bridges), use one whose wedge contains the bridge.
    for (size_t k = 0; k < n; ++k)
        if (Dist2(ring[k], ring[v]) <= eps * eps && LocallyInside(ring[(k + n - 1) % n], ring[k], ring[(k + 1) % n], m))
        {
            v = k;
            break;
        }
    Contour out;
    out.reserve(n + hole.size() + 2);
    out.insert(out.end(), ring.begin(), ring.begin() + v + 1);
    for (size_t k = 0; k <= hole.size(); ++k) out.push_back(hole[(mi + k) % hole.size()]);
    out.push_back(ring[v]);
    out.insert(out.end(), ring.begin() + v + 1, ring.end());
    ring.swap(out);
}
} // namespace

double SignedArea(const Contour& c)
{
    double a = 0;
    for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) a += c[j].x * c[i].y - c[i].x * c[j].y;
    return a / 2;
}

std::vector<GlyphPolygon> BuildPolygons(const std::vector<Contour>& in, double tolerance)
{
    std::vector<Contour> cs;
    std::vector<double> areas;
    for (const Contour& c : in)
    {
        Contour s = Simplify(c, tolerance);
        double a = s.size() >= 3 ? SignedArea(s) : 0;
        if (std::fabs(a) <= std::max(1e-9, tolerance * tolerance)) continue;
        cs.push_back(std::move(s));
        areas.push_back(std::fabs(a));
    }
    const size_t n = cs.size();
    std::vector<int> depth(n, 0);
    std::vector<std::vector<size_t>> containers(n);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            if (i != j && areas[j] > areas[i] && ContourInside(cs[i], cs[j]))
            {
                ++depth[i];
                containers[i].push_back(j);
            }
    std::vector<GlyphPolygon> polys;
    std::vector<long> polyOf(n, -1);
    for (size_t i = 0; i < n; ++i)
    {
        if (depth[i] % 2) continue;
        GlyphPolygon p;
        p.outer = cs[i];
        if (SignedArea(p.outer) < 0) std::reverse(p.outer.begin(), p.outer.end());
        polyOf[i] = (long)polys.size();
        polys.push_back(std::move(p));
    }
    for (size_t i = 0; i < n; ++i)
    {
        if (depth[i] % 2 == 0) continue;
        // The smallest container one level up is the shape this hole is cut from.
        long best = -1;
        double bestArea = 1e300;
        for (size_t j : containers[i])
            if (depth[j] == depth[i] - 1 && polyOf[j] >= 0 && areas[j] < bestArea)
            {
                bestArea = areas[j];
                best = polyOf[j];
            }
        if (best < 0) continue;
        Contour h = cs[i];
        if (SignedArea(h) > 0) std::reverse(h.begin(), h.end());
        polys[best].holes.push_back(std::move(h));
    }
    return polys;
}

bool Triangulate(const GlyphPolygon& poly, std::vector<Pt2>& points, std::vector<int>& triangles)
{
    if (poly.outer.size() < 3) return true;
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const Pt2& p : poly.outer)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    double size = std::max(maxX - minX, maxY - minY);
    double eps = std::max(1e-12, size * 1e-9), areaEps = std::max(1e-18, size * size * 1e-12);

    Contour ring = poly.outer;
    std::vector<const Contour*> holes;
    for (const Contour& h : poly.holes)
        if (h.size() >= 3) holes.push_back(&h);
    auto maxXOf = [](const Contour* c) {
        double m = -1e300;
        for (const Pt2& p : *c) m = std::max(m, p.x);
        return m;
    };
    std::sort(holes.begin(), holes.end(), [&](const Contour* a, const Contour* b) { return maxXOf(a) > maxXOf(b); });
    for (const Contour* h : holes) Bridge(ring, *h, eps);

    const int base = (int)points.size();
    points.insert(points.end(), ring.begin(), ring.end());
    std::vector<int> idx(ring.size());
    for (size_t i = 0; i < ring.size(); ++i) idx[i] = (int)i;
    bool clean = true;
    auto P = [&](int i) -> const Pt2& { return ring[i]; };
    size_t t = 0, stall = 0;
    while (idx.size() > 3)
    {
        const size_t n = idx.size();
        t %= n;
        int i0 = idx[(t + n - 1) % n], i1 = idx[t], i2 = idx[(t + 1) % n];
        const Pt2 &a = P(i0), &b = P(i1), &c = P(i2);
        double cr = Cross(a, b, c);
        bool ear = cr > areaEps;
        if (ear)
            for (size_t k = 0; k < n && ear; ++k)
            {
                int j = idx[k];
                if (j == i0 || j == i1 || j == i2) continue;
                const Pt2& p = P(j);
                if (Dist2(p, a) <= eps * eps || Dist2(p, b) <= eps * eps || Dist2(p, c) <= eps * eps) continue;
                if (InTriangleStrict(p, a, b, c, -areaEps)) ear = false;
            }
        if (ear)
        {
            triangles.push_back(base + i0);
            triangles.push_back(base + i1);
            triangles.push_back(base + i2);
            idx.erase(idx.begin() + t);
            stall = 0;
            if (t > 0) --t;
            continue;
        }
        if (++stall < n)
        {
            ++t;
            continue;
        }
        // A whole lap without an ear: drop a flat vertex if there is one, else force the most convex corner.
        size_t flat = (size_t)-1, convex = (size_t)-1;
        double best = -1e300;
        for (size_t k = 0; k < n; ++k)
        {
            double c2 = Cross(P(idx[(k + n - 1) % n]), P(idx[k]), P(idx[(k + 1) % n]));
            if (std::fabs(c2) <= areaEps && flat == (size_t)-1) flat = k;
            if (c2 > best)
            {
                best = c2;
                convex = k;
            }
        }
        if (flat != (size_t)-1) idx.erase(idx.begin() + flat);
        else
        {
            size_t k = convex;
            triangles.push_back(base + idx[(k + n - 1) % n]);
            triangles.push_back(base + idx[k]);
            triangles.push_back(base + idx[(k + 1) % n]);
            idx.erase(idx.begin() + k);
            clean = false;
        }
        stall = 0;
        t = 0;
    }
    if (idx.size() == 3 && Cross(P(idx[0]), P(idx[1]), P(idx[2])) > areaEps)
    {
        triangles.push_back(base + idx[0]);
        triangles.push_back(base + idx[1]);
        triangles.push_back(base + idx[2]);
    }
    return clean;
}

// ---- mesh -------------------------------------------------------------------------------------------------------

namespace
{
struct V3
{
    double x, y, z;
};
V3 Sub3(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Cross3(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct MeshBuilder
{
    std::vector<MeshTri> tris;
    void Add(V3 a, V3 b, V3 c, V3 na, V3 nb, V3 nc, const double ua[2], const double ub[2], const double uc[2], int mat)
    {
        // Export winding: (b - a) x (c - a) points away from the surface normal.
        V3 g = Cross3(Sub3(b, a), Sub3(c, a));
        V3 n{na.x + nb.x + nc.x, na.y + nb.y + nc.y, na.z + nb.z + nc.z};
        if (Dot3(g, g) < 1e-24) return; // degenerate
        MeshTri t{};
        V3 ps[3] = {a, b, c}, ns[3] = {na, nb, nc};
        const double* us[3] = {ua, ub, uc};
        if (Dot3(g, n) > 0)
        {
            std::swap(ps[1], ps[2]);
            std::swap(ns[1], ns[2]);
            std::swap(us[1], us[2]);
        }
        for (int k = 0; k < 3; ++k)
        {
            t.p[k][0] = ps[k].x;
            t.p[k][1] = ps[k].y;
            t.p[k][2] = ps[k].z;
            t.n[k][0] = ns[k].x;
            t.n[k][1] = ns[k].y;
            t.n[k][2] = ns[k].z;
            t.uv[k][0] = us[k][0];
            t.uv[k][1] = us[k][1];
        }
        t.material = mat;
        tris.push_back(t);
    }
};

} // namespace

std::vector<MeshTri> BuildTextMesh(const std::vector<GlyphPolygon>& polys, const ModelOptions& opt, std::vector<std::string>& materials,
                                   ModelStats& stats)
{
    stats = ModelStats{};
    materials.clear();
    auto matIndex = [&](const std::string& m) {
        for (size_t i = 0; i < materials.size(); ++i)
            if (materials[i] == m) return (int)i;
        materials.push_back(m);
        return (int)materials.size() - 1;
    };
    int front = matIndex(opt.frontMaterial), side = matIndex(opt.sideMaterial);
    int back = opt.backFace ? matIndex(opt.backMaterial) : -1;

    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const auto& p : polys)
        for (const Pt2& q : p.outer)
        {
            minX = std::min(minX, q.x);
            maxX = std::max(maxX, q.x);
            minY = std::min(minY, q.y);
            maxY = std::max(maxY, q.y);
        }
    if (polys.empty() || maxX <= minX) return {};
    const double s = opt.scale, d = std::max(0.01, opt.depth);
    const double cx = (minX + maxX) / 2, baseY = opt.pivot == ModelPivot::Centre ? (minY + maxY) / 2 : minY;
    const double xFront = opt.pivot == ModelPivot::Centre ? d / 2 : d, xBack = xFront - d;
    const double W = (maxX - minX) * s, H = (maxY - minY) * s;
    auto pos = [&](const Pt2& p, double x) { return V3{x, (p.x - cx) * s, (p.y - baseY) * s}; };
    auto frontUv = [&](const Pt2& p, double uv[2]) {
        uv[0] = (p.x - minX) / (maxX - minX);
        uv[1] = 1 - (p.y - minY) / (maxY - minY);
    };
    MeshBuilder mb;
    const double cosSmooth = std::cos(opt.smoothAngleDeg * 3.14159265358979 / 180);
    for (const GlyphPolygon& poly : polys)
    {
        std::vector<Pt2> pts;
        std::vector<int> tri;
        if (!Triangulate(poly, pts, tri)) ++stats.forcedTriangles;
        V3 nf{1, 0, 0}, nb{-1, 0, 0};
        for (size_t k = 0; k + 2 < tri.size(); k += 3)
        {
            const Pt2 &a = pts[tri[k]], &b = pts[tri[k + 1]], &c = pts[tri[k + 2]];
            double ua[2], ub[2], uc[2];
            frontUv(a, ua);
            frontUv(b, ub);
            frontUv(c, uc);
            mb.Add(pos(a, xFront), pos(b, xFront), pos(c, xFront), nf, nf, nf, ua, ub, uc, front);
            if (back >= 0)
            {
                double ra[2] = {1 - ua[0], ua[1]}, rb[2] = {1 - ub[0], ub[1]}, rc[2] = {1 - uc[0], uc[1]};
                mb.Add(pos(a, xBack), pos(b, xBack), pos(c, xBack), nb, nb, nb, ra, rb, rc, back);
            }
        }
        // Side walls, one quad per contour edge. Outer rings run counter-clockwise and holes clockwise, so the
        // solid is always on the left and the outward normal on the right of each edge.
        std::vector<const Contour*> rings = {&poly.outer};
        for (const Contour& h : poly.holes) rings.push_back(&h);
        for (const Contour* ring : rings)
        {
            const Contour& r = *ring;
            const size_t n = r.size();
            if (n < 3) continue;
            std::vector<Pt2> en(n);
            for (size_t k = 0; k < n; ++k)
            {
                const Pt2 &p = r[k], &q = r[(k + 1) % n];
                double dx = q.x - p.x, dy = q.y - p.y, l = std::sqrt(dx * dx + dy * dy);
                en[k] = l > 0 ? Pt2{dy / l, -dx / l} : Pt2{0, 0};
            }
            // Normal at the start and end of each edge: shared with the neighbour edge when the corner is gentle.
            auto corner = [&](size_t edge, size_t other) {
                const Pt2 &a = en[edge], &b = en[other];
                if (a.x * b.x + a.y * b.y >= cosSmooth)
                {
                    double x = a.x + b.x, y = a.y + b.y, l = std::sqrt(x * x + y * y);
                    if (l > 1e-9) return Pt2{x / l, y / l};
                }
                return a;
            };
            double run = 0;
            for (size_t k = 0; k < n; ++k)
            {
                const Pt2 &p = r[k], &q = r[(k + 1) % n];
                Pt2 n0 = corner(k, (k + n - 1) % n), n1 = corner(k, (k + 1) % n);
                V3 N0{0, n0.x, n0.y}, N1{0, n1.x, n1.y};
                double len = std::sqrt(Dist2(p, q)) * s;
                double u0 = run / 64, u1 = (run + len) / 64;
                run += len;
                double uvPf[2] = {u0, 0}, uvQf[2] = {u1, 0}, uvQb[2] = {u1, d / 64}, uvPb[2] = {u0, d / 64};
                V3 pf = pos(p, xFront), qf = pos(q, xFront), qb = pos(q, xBack), pb = pos(p, xBack);
                mb.Add(pf, qf, qb, N0, N1, N1, uvPf, uvQf, uvQb, side);
                mb.Add(pf, qb, pb, N0, N1, N0, uvPf, uvQb, uvPb, side);
            }
        }
        ++stats.polygons;
    }
    stats.triangles = mb.tris.size();
    stats.width = W;
    stats.height = H;
    stats.depth = d;
    return mb.tris;
}

// ---- outlines (GDI) -------------------------------------------------------------------------------------------------

#ifdef _WIN32
bool TextOutlines(const TextStyle& style, std::vector<Contour>& contours, double& lineHeightPx, std::string& error)
{
    contours.clear();
    lineHeightPx = 0;
    const int px = 512; // outline resolution: GDI paths are integer pixels, so draw big
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
    {
        error = "Windows could not create a drawing context.";
        return false;
    }
    LOGFONTW lf{};
    lf.lfHeight = -px;
    lf.lfWeight = style.bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = style.italic ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy(lf.lfFaceName, style.font.c_str(), LF_FACESIZE - 1);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int advance = tm.tmHeight + tm.tmExternalLeading;
    lineHeightPx = tm.tmHeight;
    // Lines, measured for alignment.
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (wchar_t c : style.text)
    {
        if (c == L'\r') continue;
        if (c == L'\n')
        {
            lines.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    lines.push_back(cur);
    std::vector<int> widths;
    int maxW = 0;
    for (const auto& l : lines)
    {
        SIZE sz{};
        GetTextExtentPoint32W(dc, l.c_str(), (int)l.size(), &sz);
        widths.push_back(sz.cx);
        maxW = std::max(maxW, (int)sz.cx);
    }
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].empty()) continue;
        int x = style.align == 0 ? 0 : style.align == 2 ? maxW - widths[i] : (maxW - widths[i]) / 2;
        int y = (int)i * advance;
        BeginPath(dc);
        TextOutW(dc, x, y, lines[i].c_str(), (int)lines[i].size());
        EndPath(dc);
        FlattenPath(dc);
        int n = GetPath(dc, nullptr, nullptr, 0);
        if (n <= 0) continue;
        std::vector<POINT> pts(n);
        std::vector<BYTE> types(n);
        GetPath(dc, pts.data(), types.data(), n);
        Contour c;
        auto flush = [&] {
            if (c.size() >= 3) contours.push_back(c);
            c.clear();
        };
        for (int k = 0; k < n; ++k)
        {
            BYTE t = types[k] & ~PT_CLOSEFIGURE;
            if (t == PT_MOVETO) flush();
            c.push_back(Pt2{(double)pts[k].x, -(double)pts[k].y});
            if (types[k] & PT_CLOSEFIGURE) flush();
        }
        flush();
    }
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    if (contours.empty())
    {
        error = "No outlines came out of that text. The font may not be TrueType/OpenType, or the text is only spaces.";
        return false;
    }
    return true;
}
#else
bool TextOutlines(const TextStyle&, std::vector<Contour>& contours, double& lineHeightPx, std::string& error)
{
    contours.clear();
    lineHeightPx = 0;
    error = "Glyph outlines need Windows.";
    return false;
}
#endif
} // namespace tt
