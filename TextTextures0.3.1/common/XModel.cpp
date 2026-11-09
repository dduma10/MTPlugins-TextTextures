// XModel.cpp: see XModel.h.
#include "XModel.h"
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace tt
{
std::string FormatFloat(double v)
{
    if (!std::isfinite(v) || std::fabs(v) < 5e-7) return "0";
    char b[48];
    snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

void AddMeshTri(std::vector<MeshTri>& mesh, const double a[3], const double b[3], const double c[3], const double na[3],
                const double nb[3], const double nc[3], const double ua[2], const double ub[2], const double uc[2], int material)
{
    double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double g[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    if (g[0] * g[0] + g[1] * g[1] + g[2] * g[2] < 1e-24) return;
    double n[3] = {na[0] + nb[0] + nc[0], na[1] + nb[1] + nc[1], na[2] + nb[2] + nc[2]};
    const double* ps[3] = {a, b, c};
    const double* ns[3] = {na, nb, nc};
    const double* us[3] = {ua, ub, uc};
    if (g[0] * n[0] + g[1] * n[1] + g[2] * n[2] > 0)
    {
        std::swap(ps[1], ps[2]);
        std::swap(ns[1], ns[2]);
        std::swap(us[1], us[2]);
    }
    MeshTri t{};
    for (int k = 0; k < 3; ++k)
    {
        for (int i = 0; i < 3; ++i)
        {
            t.p[k][i] = ps[k][i];
            t.n[k][i] = ns[k][i];
        }
        t.uv[k][0] = us[k][0];
        t.uv[k][1] = us[k][1];
    }
    t.material = material;
    mesh.push_back(t);
}

std::string WriteXModelExport(const std::vector<MeshTri>& mesh, const std::vector<std::string>& materials)
{
    // Weld positions: VERT records carry only position and weights; normals and UVs belong to face corners.
    std::unordered_map<std::string, size_t> index;
    std::vector<std::string> positions;
    std::vector<size_t> corner(mesh.size() * 3);
    for (size_t t = 0; t < mesh.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            std::string key = FormatFloat(mesh[t].p[k][0]) + " " + FormatFloat(mesh[t].p[k][1]) + " " + FormatFloat(mesh[t].p[k][2]);
            auto it = index.find(key);
            if (it == index.end())
            {
                it = index.emplace(key, positions.size()).first;
                positions.push_back(key);
            }
            corner[t * 3 + k] = it->second;
        }
    const bool wide = positions.size() > 65535;
    const char* vert = wide ? "VERT32 " : "VERT ";
    std::string o;
    o.reserve(positions.size() * 64 + mesh.size() * 300 + 4096);
    o += "MODEL\nVERSION 6\n\nNUMBONES 1\nBONE 0 -1 \"tag_origin\"\n\nBONE 0\nOFFSET 0 0 0\nSCALE 1 1 1\nX 1 0 0\nY 0 1 0\nZ 0 0 1\n\n";
    o += std::string(wide ? "NUMVERTS32 " : "NUMVERTS ") + std::to_string(positions.size()) + "\n\n";
    for (size_t i = 0; i < positions.size(); ++i) o += vert + std::to_string(i) + "\nOFFSET " + positions[i] + "\nBONES 1\nBONE 0 1\n\n";
    o += "NUMFACES " + std::to_string(mesh.size()) + "\n\n";
    for (size_t t = 0; t < mesh.size(); ++t)
    {
        const MeshTri& m = mesh[t];
        o += "TRI 0 " + std::to_string(m.material) + " 0 0\n";
        for (int k = 0; k < 3; ++k)
            o += vert + std::to_string(corner[t * 3 + k]) + "\nNORMAL " + FormatFloat(m.n[k][0]) + " " + FormatFloat(m.n[k][1]) + " " + FormatFloat(m.n[k][2]) +
                 "\nCOLOR 1 1 1 1\nUV 1 " + FormatFloat(m.uv[k][0]) + " " + FormatFloat(m.uv[k][1]) + "\n";
        o += "\n";
    }
    o += "NUMOBJECTS 1\nOBJECT 0 \"text\"\n\nNUMMATERIALS " + std::to_string(materials.size()) + "\n";
    for (size_t i = 0; i < materials.size(); ++i)
        o += "MATERIAL " + std::to_string(i) + " \"" + materials[i] +
             "\" \"Lambert\" \"\"\nCOLOR 0 0 0 1\nTRANSPARENCY 0 0 0 1\nAMBIENTCOLOR 1 1 1 1\nINCANDESCENCE 0 0 0 1\nCOEFFS 0.8 0\nGLOW 0 0\n"
             "REFRACTIVE 6 1\nSPECULARCOLOR 0.5 0.5 0.5 1\nREFLECTIVECOLOR 0 0 0 1\nREFLECTIVE 1 0.5\nBLINN -1 -1\nPHONG -1\n";
    return o;
}
} // namespace tt
