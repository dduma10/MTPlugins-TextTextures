// MapEdit.cpp: brush face parsing, polygon reconstruction, texture fitting and sign brushes.
#include "MapEdit.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tt
{
namespace
{
Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Norm(Vec3 a)
{
    double l = Len(a);
    return l > 1e-12 ? Mul(a, 1 / l) : Vec3{};
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Reads "( x y z )" at s[pos], leaves pos after the ')'.
bool ReadPoint(const std::string& s, size_t& pos, Vec3& out)
{
    while (pos < s.size() && isspace((unsigned char)s[pos])) ++pos;
    if (pos >= s.size() || s[pos] != '(') return false;
    ++pos;
    double v[3];
    for (double& x : v)
    {
        while (pos < s.size() && isspace((unsigned char)s[pos])) ++pos;
        const char* b = s.c_str() + pos;
        char* e = nullptr;
        x = strtod(b, &e);
        if (e == b) return false;
        pos += size_t(e - b);
    }
    while (pos < s.size() && isspace((unsigned char)s[pos])) ++pos;
    if (pos >= s.size() || s[pos] != ')') return false;
    ++pos;
    out = {v[0], v[1], v[2]};
    return true;
}
// Next whitespace-separated token at s[pos], within [pos, limit).
bool ReadToken(const std::string& s, size_t& pos, size_t limit, std::string& tok, size_t* start = nullptr)
{
    while (pos < limit && isspace((unsigned char)s[pos])) ++pos;
    if (pos >= limit) return false;
    size_t b = pos;
    while (pos < limit && !isspace((unsigned char)s[pos])) ++pos;
    tok = s.substr(b, pos - b);
    if (start) *start = b;
    return true;
}

bool ParseFaceLine(const std::string& text, size_t lineBegin, size_t lineEnd, FaceLine& f)
{
    size_t pos = lineBegin;
    for (Vec3& p : f.p)
        if (!ReadPoint(text, pos, p) || pos > lineEnd) return false;
    std::string tok;
    size_t start = 0;
    if (!ReadToken(text, pos, lineEnd, f.material, &start)) return false;
    f.paramsBegin = start;
    for (int i = 0; i < 5; ++i)
    {
        if (!ReadToken(text, pos, lineEnd, tok)) return false;
        char* e = nullptr;
        strtod(tok.c_str(), &e);
        if (!e || *e) return false;
    }
    f.paramsEnd = pos;
    f.rest = Trim(text.substr(pos, lineEnd - pos));
    f.lineBegin = lineBegin;
    f.lineEnd = lineEnd;
    return true;
}

struct Plane
{
    Vec3 n;
    double d = 0;
};
Plane PlaneOf(const FaceLine& f)
{
    // Quake's winding: (p3 - p1) x (p2 - p1) points out of the brush.
    Vec3 n = Norm(Cross(Sub(f.p[2], f.p[0]), Sub(f.p[1], f.p[0])));
    return {n, Dot(n, f.p[0])};
}

// The axial projection table Radiant uses (floor, ceiling, +-X walls, +-Y walls): normal, U, V.
const double kAxes[6][3][3] = {
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},  {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
};
Vec3 V(const double a[3]) { return {a[0], a[1], a[2]}; }
} // namespace

std::string BoxBrushText(const Vec3& mins, const Vec3& maxs, const std::string& material, const std::string& faceSuffix,
                         int winding, const std::string& brushGuid)
{
    const double x0 = mins.x, y0 = mins.y, z0 = mins.z, x1 = maxs.x, y1 = maxs.y, z1 = maxs.z;
    // Quake order, (c - a) x (b - a) = outward normal; BO3's order swaps the last two points.
    const Vec3 f[6][3] = {
        {{x0, y0, z0}, {x0, y0 + 1, z0}, {x0, y0, z0 + 1}}, {{x0, y0, z0}, {x0, y0, z0 + 1}, {x0 + 1, y0, z0}},
        {{x0, y0, z0}, {x0 + 1, y0, z0}, {x0, y0 + 1, z0}}, {{x1, y1, z1}, {x1, y1 + 1, z1}, {x1 + 1, y1, z1}},
        {{x1, y1, z1}, {x1 + 1, y1, z1}, {x1, y1, z1 + 1}}, {{x1, y1, z1}, {x1, y1, z1 + 1}, {x1, y1 + 1, z1}},
    };
    auto pt = [](const Vec3& p) { return "( " + FormatNumber(p.x) + " " + FormatNumber(p.y) + " " + FormatNumber(p.z) + " )"; };
    std::string suffix = faceSuffix.empty() ? DefaultFaceSuffix() : faceSuffix;
    std::string out = "{\n";
    if (!brushGuid.empty()) out += "guid \"" + brushGuid + "\"\n";
    for (const auto& face : f)
        out += " " + pt(face[0]) + " " + (winding > 0 ? pt(face[1]) + " " + pt(face[2]) : pt(face[2]) + " " + pt(face[1])) + " " + material +
               " 64 64 0 0 0 " + suffix + "\n";
    out += "}\n";
    return out;
}

bool BrushAllMaterials(const BrushBlock& b, const std::string& prefix)
{
    if (b.faces.empty()) return false;
    for (const FaceLine& f : b.faces)
        if (Lower(f.material).compare(0, prefix.size(), Lower(prefix)) != 0) return false;
    return true;
}

std::string FormatNumber(double v)
{
    if (std::fabs(v) < 5e-7) return "0";
    char buf[64];
    snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::vector<BrushBlock> ParseBrushBlocks(const std::string& text)
{
    std::vector<BrushBlock> blocks;
    int depth = 0;
    BrushBlock cur;
    bool in = false;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        size_t lineEnd = nl == std::string::npos ? text.size() : nl;
        size_t next = nl == std::string::npos ? text.size() : nl + 1;
        size_t contentEnd = lineEnd;
        if (contentEnd > pos && text[contentEnd - 1] == '\r') --contentEnd;
        std::string line = Trim(text.substr(pos, contentEnd - pos));
        if (line == "{")
        {
            ++depth;
            if (depth == 2)
            {
                cur = BrushBlock{};
                cur.begin = pos;
                in = true;
            }
            else if (in && depth > 2)
                cur.isPatch = true;
        }
        else if (line == "}")
        {
            if (depth == 2 && in)
            {
                cur.end = next;
                blocks.push_back(std::move(cur));
                in = false;
            }
            --depth;
        }
        else if (in && depth == 2)
        {
            if (line.rfind("guid", 0) == 0 && cur.guid.empty())
            {
                size_t q = line.find('"');
                size_t q2 = q == std::string::npos ? q : line.find('"', q + 1);
                if (q2 != std::string::npos) cur.guid = Lower(line.substr(q + 1, q2 - q - 1));
            }
            else if (line == "mesh" || line == "curve" || line.rfind("mesh", 0) == 0 || line.rfind("curve", 0) == 0)
                cur.isPatch = true;
            else if (!line.empty() && line[0] == '(')
            {
                FaceLine f;
                size_t lb = pos;
                while (lb < contentEnd && isspace((unsigned char)text[lb])) ++lb;
                if (ParseFaceLine(text, lb, contentEnd, f)) cur.faces.push_back(f);
            }
        }
        pos = next;
    }
    return blocks;
}

bool BrushPolygons(const BrushBlock& b, std::vector<FacePoly>& out, int* winding)
{
    out.clear();
    if (winding) *winding = 0;
    if (b.isPatch || b.faces.size() < 4) return false;
    std::vector<Plane> base;
    for (const FaceLine& f : b.faces) base.push_back(PlaneOf(f));
    // Try the Quake winding first, then the opposite one, keeping whichever closes a solid.
    for (int flip = 0; flip < 2; ++flip)
    {
        std::vector<Plane> ps = base;
        if (flip)
            for (Plane& p : ps)
            {
                p.n = Mul(p.n, -1);
                p.d = -p.d;
            }
        std::vector<Vec3> verts;
        for (size_t i = 0; i < ps.size(); ++i)
            for (size_t j = i + 1; j < ps.size(); ++j)
                for (size_t k = j + 1; k < ps.size(); ++k)
                {
                    const Plane &a = ps[i], &c = ps[j], &e = ps[k];
                    if (Len(a.n) < .5 || Len(c.n) < .5 || Len(e.n) < .5) continue;
                    Vec3 ce = Cross(c.n, e.n);
                    double den = Dot(a.n, ce);
                    if (std::fabs(den) < 1e-9) continue;
                    Vec3 x = Mul(Add(Add(Mul(ce, a.d), Mul(Cross(e.n, a.n), c.d)), Mul(Cross(a.n, c.n), e.d)), 1 / den);
                    bool inside = true;
                    for (const Plane& p : ps)
                        if (Len(p.n) > .5 && Dot(p.n, x) > p.d + 0.01)
                        {
                            inside = false;
                            break;
                        }
                    if (!inside) continue;
                    bool dup = false;
                    for (const Vec3& v : verts)
                        if (Len(Sub(v, x)) < 0.001) dup = true;
                    if (!dup) verts.push_back(x);
                }
        if (verts.size() < 4) continue;
        out.resize(ps.size());
        size_t withArea = 0;
        for (size_t i = 0; i < ps.size(); ++i)
        {
            FacePoly& fp = out[i];
            fp.normal = ps[i].n;
            for (const Vec3& v : verts)
                if (std::fabs(Dot(ps[i].n, v) - ps[i].d) < 0.01) fp.corners.push_back(v);
            if (fp.corners.size() < 3)
            {
                fp.corners.clear();
                continue;
            }
            Vec3 c{};
            for (const Vec3& v : fp.corners) c = Add(c, v);
            fp.center = Mul(c, 1.0 / fp.corners.size());
            ++withArea;
        }
        if (withArea >= 4)
        {
            if (winding) *winding = flip ? -1 : 1;
            return true;
        }
    }
    out.clear();
    return false;
}

int ChooseCameraFace(const std::vector<FacePoly>& polys, const Vec3& camOrigin, const Vec3& camForward)
{
    int best = -1, bestAny = -1;
    double bestScore = -1e30, bestAnyScore = -1e30;
    for (size_t i = 0; i < polys.size(); ++i)
    {
        const FacePoly& p = polys[i];
        if (p.corners.empty()) continue;
        double score = -Dot(p.normal, camForward);
        if (score > bestAnyScore)
        {
            bestAnyScore = score;
            bestAny = (int)i;
        }
        bool cameraSide = Dot(Sub(camOrigin, p.center), p.normal) > 0;
        if (cameraSide && score > bestScore)
        {
            bestScore = score;
            best = (int)i;
        }
    }
    return best >= 0 ? best : bestAny;
}

namespace
{
// Radiant's texture rotation: both axes turn by `deg` within the plane of their two non-zero components.
void RotateAxes(Vec3& u, Vec3& v, double deg)
{
    if (deg == 0) return;
    double r = deg * 3.14159265358979323846 / 180, si = std::sin(r), co = std::cos(r);
    double* x = &u.x;
    double* y = &v.x;
    int sx = u.x != 0 ? 0 : u.y != 0 ? 1 : 2, sy = v.x != 0 ? 0 : v.y != 0 ? 1 : 2;
    double nx = co * x[sx] - si * x[sy], ny = si * x[sx] + co * x[sy];
    x[sx] = nx;
    x[sy] = ny;
    nx = co * y[sx] - si * y[sy];
    ny = si * y[sx] + co * y[sy];
    y[sx] = nx;
    y[sy] = ny;
}
} // namespace

std::string FitFaceParams(const FacePoly& face, const std::string& material, const Vec3& camForward)
{
    int axis = 0;
    double bd = -1e30;
    for (int i = 0; i < 6; ++i)
    {
        double d = Dot(face.normal, V(kAxes[i][0]));
        if (d > bd)
        {
            bd = d;
            axis = i;
        }
    }
    // Where "right" and "down" are for someone looking at the face from outside.
    Vec3 up{0, 0, 1}, right, down;
    if (axis >= 2)
    {
        right = Norm(Cross(Mul(face.normal, -1), up));
        down = {0, 0, -1};
    }
    else
    {
        Vec3 h = Norm(Vec3{camForward.x, camForward.y, 0});
        if (Len(h) < .5) h = {0, 1, 0};
        right = Norm(Cross(h, up));
        // Looking down at a floor the top of the screen is ahead; looking up at a ceiling it is behind.
        down = axis == 0 ? Mul(h, -1) : h;
    }
    // The quarter turn whose axes line up best with right/down (walls: always 0), then mirror what is left.
    int rotation = 0;
    Vec3 u = V(kAxes[axis][1]), v = V(kAxes[axis][2]);
    double bestScore = -1e30;
    for (int r = 0; r < 360; r += 90)
    {
        Vec3 ru = V(kAxes[axis][1]), rv = V(kAxes[axis][2]);
        RotateAxes(ru, rv, r);
        double score = std::fabs(Dot(ru, right)) + std::fabs(Dot(rv, down)) + 1e-3 * (Dot(ru, right) + Dot(rv, down));
        if (score > bestScore + 1e-9)
        {
            bestScore = score;
            rotation = r;
            u = ru;
            v = rv;
        }
    }
    double smin = 1e30, smax = -1e30, tmin = 1e30, tmax = -1e30;
    for (const Vec3& c : face.corners)
    {
        smin = std::min(smin, Dot(c, u));
        smax = std::max(smax, Dot(c, u));
        tmin = std::min(tmin, Dot(c, v));
        tmax = std::max(tmax, Dot(c, v));
    }
    double w = std::max(smax - smin, 0.01), h = std::max(tmax - tmin, 0.01);
    double width = w, xshift = -smin, height = h, yshift = -tmin;
    if (Dot(u, right) < 0)
    {
        width = -w;
        xshift = -smax;
    }
    if (Dot(v, down) < 0)
    {
        height = -h;
        yshift = -tmax;
    }
    return material + " " + FormatNumber(width) + " " + FormatNumber(height) + " " + FormatNumber(xshift) + " " +
           FormatNumber(yshift) + " " + std::to_string(rotation);
}

int ApplyMaterialInText(std::string& text, const std::string& material, ApplyMode mode, const Vec3& camOrigin,
                        const Vec3& camForward, const std::set<std::string>* onlyGuids, std::vector<std::string>* changedGuids)
{
    if (changedGuids) changedGuids->clear();
    struct Edit
    {
        size_t begin, end;
        std::string with;
    };
    std::vector<Edit> edits;
    for (const BrushBlock& b : ParseBrushBlocks(text))
    {
        if (onlyGuids && !onlyGuids->count(b.guid)) continue;
        std::vector<FacePoly> polys;
        if (!BrushPolygons(b, polys)) continue;
        int only = mode == ApplyMode::CameraFace ? ChooseCameraFace(polys, camOrigin, camForward) : -1;
        size_t before = edits.size();
        for (size_t i = 0; i < b.faces.size() && i < polys.size(); ++i)
        {
            if (polys[i].corners.empty()) continue;
            if (mode == ApplyMode::CameraFace && (int)i != only) continue;
            edits.push_back({b.faces[i].paramsBegin, b.faces[i].paramsEnd, FitFaceParams(polys[i], material, camForward)});
        }
        if (changedGuids && edits.size() > before) changedGuids->push_back(b.guid);
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin > b.begin; });
    for (const Edit& e : edits) text.replace(e.begin, e.end - e.begin, e.with);
    return (int)edits.size();
}

int DetectWinding(const std::string& text)
{
    for (const BrushBlock& b : ParseBrushBlocks(text))
    {
        std::vector<FacePoly> polys;
        int w = 0;
        if (BrushPolygons(b, polys, &w) && w != 0) return w;
    }
    return 0;
}

std::string WorldBrushImportText(const std::string& text, const std::set<std::string>* onlyGuids)
{
    std::string out = "{\n\"classname\" \"worldspawn\"\n";
    for (const BrushBlock& b : ParseBrushBlocks(text))
    {
        if (onlyGuids && !onlyGuids->count(b.guid)) continue;
        std::string block = text.substr(b.begin, b.end - b.begin);
        if (!block.empty() && block.back() != '\n') block += '\n';
        out += block;
    }
    out += "}\n";
    return out;
}

std::string DefaultFaceSuffix() { return "0 lightmap_gray 16384 16384 0 0 0 0"; }

std::string FaceSuffixFrom(const std::string& text)
{
    for (const BrushBlock& b : ParseBrushBlocks(text))
        for (const FaceLine& f : b.faces)
            if (!f.rest.empty()) return f.rest;
    return std::string();
}

Vec3 ForwardFromAngles(double pitchDeg, double yawDeg)
{
    const double r = 3.14159265358979323846 / 180;
    double p = pitchDeg * r, y = yawDeg * r;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

SignBrush BuildSignBrush(const Vec3& camOrigin, const Vec3& camForward, double distance, double width, double height,
                         double thickness, const std::string& material, const std::string& backMaterial,
                         const std::string& faceSuffix, const std::string& brushGuid, int winding)
{
    // The horizontal axis nearest the view direction; the sign's front faces back along it.
    Vec3 h{camForward.x, camForward.y, 0};
    if (Len(h) < 1e-6) h = {0, 1, 0};
    bool alongX = std::fabs(h.x) >= std::fabs(h.y);
    int sgn = alongX ? (h.x >= 0 ? 1 : -1) : (h.y >= 0 ? 1 : -1);
    Vec3 axis = alongX ? Vec3{double(sgn), 0, 0} : Vec3{0, double(sgn), 0};
    Vec3 at = Add(camOrigin, Mul(axis, distance));
    auto snap = [](double v) { return std::round(v); };
    double hw = width / 2, hh = height / 2;
    Vec3 mins, maxs;
    double cz = snap(at.z);
    mins.z = cz - hh;
    maxs.z = cz + hh;
    if (alongX)
    {
        double cy = snap(at.y), front = snap(at.x);
        mins.y = cy - hw;
        maxs.y = cy + hw;
        if (sgn > 0)
        {
            mins.x = front;
            maxs.x = front + thickness;
        }
        else
        {
            maxs.x = front;
            mins.x = front - thickness;
        }
    }
    else
    {
        double cx = snap(at.x), front = snap(at.y);
        mins.x = cx - hw;
        maxs.x = cx + hw;
        if (sgn > 0)
        {
            mins.y = front;
            maxs.y = front + thickness;
        }
        else
        {
            maxs.y = front;
            mins.y = front - thickness;
        }
    }
    Vec3 frontNormal = Mul(axis, -1);
    const double x0 = mins.x, y0 = mins.y, z0 = mins.z, x1 = maxs.x, y1 = maxs.y, z1 = maxs.z;
    struct F
    {
        Vec3 a, b, c, n;
    };
    // Quake winding, (c - a) x (b - a) = outward normal.
    const F faces[6] = {
        {{x0, y0, z0}, {x0, y0 + 1, z0}, {x0, y0, z0 + 1}, {-1, 0, 0}}, {{x0, y0, z0}, {x0, y0, z0 + 1}, {x0 + 1, y0, z0}, {0, -1, 0}},
        {{x0, y0, z0}, {x0 + 1, y0, z0}, {x0, y0 + 1, z0}, {0, 0, -1}}, {{x1, y1, z1}, {x1, y1 + 1, z1}, {x1 + 1, y1, z1}, {0, 0, 1}},
        {{x1, y1, z1}, {x1 + 1, y1, z1}, {x1, y1, z1 + 1}, {0, 1, 0}},  {{x1, y1, z1}, {x1, y1, z1 + 1}, {x1, y1 + 1, z1}, {1, 0, 0}},
    };
    std::string suffix = faceSuffix.empty() ? DefaultFaceSuffix() : faceSuffix;
    std::string brush = "{\n";
    if (!brushGuid.empty()) brush += "guid \"" + brushGuid + "\"\n";
    for (const F& f : faces)
    {
        auto pt = [](Vec3 p) { return "( " + FormatNumber(p.x) + " " + FormatNumber(p.y) + " " + FormatNumber(p.z) + " )"; };
        std::string params;
        if (Dot(f.n, frontNormal) > .5)
        {
            FacePoly poly;
            poly.normal = f.n;
            for (int i = 0; i < 8; ++i)
            {
                Vec3 c{i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0};
                if (std::fabs(Dot(Sub(c, f.a), f.n)) < 1e-6) poly.corners.push_back(c);
            }
            params = FitFaceParams(poly, material, camForward);
        }
        else
            params = backMaterial + " 64 64 0 0 0";
        // The table is in Quake's order; BO3 Radiant's swaps the last two points.
        brush += " " + pt(f.a) + " " + (winding > 0 ? pt(f.b) + " " + pt(f.c) : pt(f.c) + " " + pt(f.b)) + " " + params + " " + suffix + "\n";
    }
    brush += "}\n";
    SignBrush s;
    s.mins = mins;
    s.maxs = maxs;
    s.mapText = "{\n\"classname\" \"worldspawn\"\n" + brush + "}\n";
    return s;
}
} // namespace tt
