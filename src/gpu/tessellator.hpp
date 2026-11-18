#pragma once

#include "nisaba/gpu/types.hpp"
#include "nisaba/gpu/renderer.hpp"
#include "nisaba/path/path.hpp"
#include <span>
#include <vector>

namespace nisaba::gpu {

// -----------------------------------------------------------------------------
// Nisaba GPU Contour Vertex Topology Flags
// -----------------------------------------------------------------------------
enum ContourPointFlags : uint8_t {
	PointCorner     = 1 << 0,
	PointLeft       = 1 << 1,
	PointBevel      = 1 << 2,
	PointInnerBevel = 1 << 3
};

// -----------------------------------------------------------------------------
// ContourPoint: Contoured vertex with analytical tangent and normal vectors
// -----------------------------------------------------------------------------
struct ContourPoint {
	float x{0.0f};
	float y{0.0f};
	float dx{0.0f};
	float dy{0.0f};
	float len{0.0f};
	float dmx{0.0f};
	float dmy{0.0f};
	uint8_t flags{0};
};

using GpuPathPoint = ContourPoint;

// -----------------------------------------------------------------------------
// PathContour: Segment slice in pre-allocated contour buffer
// -----------------------------------------------------------------------------
struct PathContour {
	int first{0};
	int count{0};
	bool closed{false};
	int nbevel{0};
	int fillOffset{0};
	int fillCount{0};
	int strokeOffset{0};
	int strokeCount{0};
	Winding winding{Winding::CounterClockwise};
	bool convex{false};
};

using GpuSubPath = PathContour;

// -----------------------------------------------------------------------------
// Tessellator: High-performance 2D vector geometry extrusion engine
// -----------------------------------------------------------------------------
class Tessellator {
public:
	Tessellator();

	void clear();
	void reset(float distTol, float tessTol, float fringeWidth);

	void addContour();
	void addPoint(float x, float y, uint8_t flags);
	void closeContour();
	void setContourWinding(Winding winding);

	// Convenience methods matching legacy calls
	void addPath() { addContour(); }
	void closePath() { closeContour(); }
	void setPathWinding(Winding winding) { setContourWinding(winding); }

	// Sovereign command stream & path flattening
	void flattenCommands(std::span<const PathCommand> commands);
	void flattenPath(const nisaba::Path& path, const Transform2D& xform = Transform2D::identity());

	bool expandStroke(float width, float fringe, LineCap cap, LineJoin join, float miterLimit);
	bool expandFill(float fringe, LineJoin join, float miterLimit);

	ContourPoint* lastPoint();
	PathContour* lastContour();
	PathContour* lastPath() { return lastContour(); }

	const std::vector<PathContour>& contours() const { return m_contours; }
	const std::vector<PathContour>& paths() const { return m_contours; }
	const std::vector<Vertex>& vertices() const { return m_vertices; }
	const float* bounds() const { return m_bounds; }

private:
	void tessellateBezier(float x1, float y1, float x2, float y2,
	                      float x3, float y3, float x4, float y4,
	                      int level, uint8_t type);
	void calculateJoins(float w, LineJoin join, float miterLimit);
	void finalizeContours();

	float m_distTol{0.01f};
	float m_tessTol{0.25f};
	float m_fringeWidth{1.0f};
	float m_bounds[4]{0.0f, 0.0f, 0.0f, 0.0f};

	std::vector<ContourPoint> m_points;
	std::vector<PathContour> m_contours;
	std::vector<Vertex> m_vertices;
};

} // namespace nisaba::gpu
