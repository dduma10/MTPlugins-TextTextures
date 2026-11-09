// Text3D.h: extruded 3D text as an XModel. Glyph outlines come from GDI paths (Windows); nesting, triangulation,
// extrusion and the XMODEL_EXPORT writer are plain C++.
//
// Model space: the text reads from +X (stand at +X looking toward -X): it runs along +Y, up is +Z, the front face
// is at X = depth and the back at X = 0. A misc_model with yaw = (direction it should face) shows it the right way.
#pragma once
#include "TextRender.h"
#include "XModel.h"
#include <string>
#include <vector>

namespace tt
{
struct Pt2
{
    double x = 0, y = 0;
};
using Contour = std::vector<Pt2>;

struct GlyphPolygon
{
    Contour outer;               // counter-clockwise (y up)
    std::vector<Contour> holes;  // clockwise
};

// Outlines of the style's text (font, bold, italic, alignment, lines separated by '\n'; no wrapping), y up, in
// pixels of a font whose line is `lineHeightPx` tall. Windows only.
bool TextOutlines(const TextStyle& style, std::vector<Contour>& contours, double& lineHeightPx, std::string& error);

// Drops repeated and nearly collinear points (deviation under `tolerance`), then sorts contours into outer shapes
// and their holes by nesting depth (even-odd, as TrueType outlines nest). Orientation is normalised.
std::vector<GlyphPolygon> BuildPolygons(const std::vector<Contour>& contours, double tolerance);

// Ear clipping with hole bridging. Appends counter-clockwise triangles (indices into `points`). Returns false when
// it had to force a triangle to finish (the result still covers the shape, possibly with a sliver).
bool Triangulate(const GlyphPolygon& poly, std::vector<Pt2>& points, std::vector<int>& triangles);

double SignedArea(const Contour& c);

enum class ModelPivot
{
    BackBottomCentre = 0, // on the wall, standing on its baseline: good for building signs
    Centre = 1,
};

struct ModelOptions
{
    double scale = 1;            // world units per outline unit
    double depth = 4;            // extrusion in world units
    bool backFace = true;
    double smoothAngleDeg = 35;  // side normals are averaged across corners flatter than this
    ModelPivot pivot = ModelPivot::BackBottomCentre;
    std::string frontMaterial = "front", sideMaterial = "side", backMaterial = "side";
};

struct ModelStats
{
    size_t triangles = 0, vertices = 0, polygons = 0, forcedTriangles = 0;
    double width = 0, height = 0, depth = 0; // world units
};

// The extruded mesh (export winding: each triangle's (b - a) x (c - a) points away from its normal, as CoD's
// XMODEL_EXPORT wants). materials receives the material names, indexed by MeshTri::material.
std::vector<MeshTri> BuildTextMesh(const std::vector<GlyphPolygon>& polys, const ModelOptions& opt, std::vector<std::string>& materials,
                                   ModelStats& stats);

} // namespace tt
