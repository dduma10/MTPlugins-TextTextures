// XModel.h: XMODEL_EXPORT writing (version 6, one tag_origin bone, as Brush Modeler writes for export2bin).
#pragma once
#include <string>
#include <vector>

namespace tt
{
struct MeshTri
{
    double p[3][3];
    double n[3][3];
    double uv[3][2];
    int material;
};

// The export text. Triangles use CoD's export winding: (b - a) x (c - a) points away from the surface normal.
// Positions are welded; normals and UVs stay per face corner.
std::string WriteXModelExport(const std::vector<MeshTri>& mesh, const std::vector<std::string>& materials);

// Adds a triangle in export winding, whatever order a, b, c come in (decided by the corner normals). Skips
// degenerate triangles.
void AddMeshTri(std::vector<MeshTri>& mesh, const double a[3], const double b[3], const double c[3], const double na[3],
                const double nb[3], const double nc[3], const double ua[2], const double ub[2], const double uc[2], int material);

// "%.6f" without trailing zeros, "0" for tiny values.
std::string FormatFloat(double v);
} // namespace tt
