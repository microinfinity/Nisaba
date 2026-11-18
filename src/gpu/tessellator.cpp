// -----------------------------------------------------------------------------
// Nisaba Sovereign 2D Graphics Engine - GPU Vector Tessellator
//
// Features:
// - Subpixel adaptive cubic Bézier curve subdivision
// - Analytical vector stroke extrusion (Butt/Round/Square caps, Miter/Round/Bevel joins)
// - Subpixel anti-aliasing feather fringe geometry generation
// - High-throughput retained memory topology buffers (Zero heap reallocations per frame)
// - Direct streaming from nisaba::Path and structured PathCommand sequence
// -----------------------------------------------------------------------------

#include "tessellator.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace nisaba::gpu {

namespace {

inline bool pointEquals(float x1, float y1, float x2, float y2, float tol) noexcept {
	float dx = x2 - x1;
	float dy = y2 - y1;
	return dx * dx + dy * dy < tol * tol;
}

inline float normalizeVector(float* x, float* y) noexcept {
	float d = std::sqrt((*x) * (*x) + (*y) * (*y));
	if (d > 1e-6f) {
		float invD = 1.0f / d;
		*x *= invD;
		*y *= invD;
	}
	return d;
}

inline float signedTriangleArea2(float ax, float ay, float bx, float by, float cx, float cy) noexcept {
	float abx = bx - ax;
	float aby = by - ay;
	float acx = cx - ax;
	float acy = cy - ay;
	return acx * aby - abx * acy;
}

inline float polygonSignedArea(const ContourPoint* pts, int npts) noexcept {
	float area = 0.0f;
	for (int i = 2; i < npts; ++i) {
		area += signedTriangleArea2(pts[0].x, pts[0].y, pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y);
	}
	return area * 0.5f;
}

inline void reverseContourPoints(ContourPoint* pts, int npts) noexcept {
	int i = 0, j = npts - 1;
	while (i < j) {
		std::swap(pts[i], pts[j]);
		++i;
		--j;
	}
}

inline int curveDivisions(float radius, float arcAngle, float tolerance) noexcept {
	float deltaAngle = std::acos(radius / (radius + tolerance)) * 2.0f;
	return std::max(2, static_cast<int>(std::ceil(arcAngle / deltaAngle)));
}

inline void computeBevelPoints(bool bevel, const ContourPoint* p0, const ContourPoint* p1, float width,
                               float* x0, float* y0, float* x1, float* y1) noexcept {
	if (bevel) {
		*x0 = p1->x + p0->dy * width;
		*y0 = p1->y - p0->dx * width;
		*x1 = p1->x + p1->dy * width;
		*y1 = p1->y - p1->dx * width;
	} else {
		*x0 = p1->x + p1->dmx * width;
		*y0 = p1->y + p1->dmy * width;
		*x1 = p1->x + p1->dmx * width;
		*y1 = p1->y + p1->dmy * width;
	}
}

inline Vertex* setVertex(Vertex* vtx, float x, float y, float u, float v) noexcept {
	vtx->x = x;
	vtx->y = y;
	vtx->u = u;
	vtx->v = v;
	vtx->color = 0xffffffff;
	return vtx + 1;
}

inline Vertex* buttCapStart(Vertex* dst, const ContourPoint* p,
                            float dx, float dy, float w, float d, float aa, float u0, float u1) noexcept {
	float px = p->x - dx * d;
	float py = p->y - dy * d;
	float dlx = dy;
	float dly = -dx;
	dst = setVertex(dst, px + dlx * w - dx * aa, py + dly * w - dy * aa, u0, 0.0f);
	dst = setVertex(dst, px - dlx * w - dx * aa, py - dly * w - dy * aa, u1, 0.0f);
	dst = setVertex(dst, px + dlx * w, py + dly * w, u0, 1.0f);
	dst = setVertex(dst, px - dlx * w, py - dly * w, u1, 1.0f);
	return dst;
}

inline Vertex* buttCapEnd(Vertex* dst, const ContourPoint* p,
                          float dx, float dy, float w, float d, float aa, float u0, float u1) noexcept {
	float px = p->x + dx * d;
	float py = p->y + dy * d;
	float dlx = dy;
	float dly = -dx;
	dst = setVertex(dst, px + dlx * w, py + dly * w, u0, 1.0f);
	dst = setVertex(dst, px - dlx * w, py - dly * w, u1, 1.0f);
	dst = setVertex(dst, px + dlx * w + dx * aa, py + dly * w + dy * aa, u0, 0.0f);
	dst = setVertex(dst, px - dlx * w + dx * aa, py - dly * w + dy * aa, u1, 0.0f);
	return dst;
}

inline Vertex* roundCapStart(Vertex* dst, const ContourPoint* p,
                             float dx, float dy, float w, int ncap, float u0, float u1) noexcept {
	float px = p->x;
	float py = p->y;
	float dlx = dy;
	float dly = -dx;
	for (int i = 0; i < ncap; ++i) {
		float a = i / static_cast<float>(ncap - 1) * PI;
		float ax = std::cos(a) * w, ay = std::sin(a) * w;
		dst = setVertex(dst, px - dlx * ax - dx * ay, py - dly * ax - dy * ay, u0, 1.0f);
		dst = setVertex(dst, px, py, 0.5f, 1.0f);
	}
	dst = setVertex(dst, px + dlx * w, py + dly * w, u0, 1.0f);
	dst = setVertex(dst, px - dlx * w, py - dly * w, u1, 1.0f);
	return dst;
}

inline Vertex* roundCapEnd(Vertex* dst, const ContourPoint* p,
                           float dx, float dy, float w, int ncap, float u0, float u1) noexcept {
	float px = p->x;
	float py = p->y;
	float dlx = dy;
	float dly = -dx;
	dst = setVertex(dst, px + dlx * w, py + dly * w, u0, 1.0f);
	dst = setVertex(dst, px - dlx * w, py - dly * w, u1, 1.0f);
	for (int i = 0; i < ncap; ++i) {
		float a = i / static_cast<float>(ncap - 1) * PI;
		float ax = std::cos(a) * w, ay = std::sin(a) * w;
		dst = setVertex(dst, px, py, 0.5f, 1.0f);
		dst = setVertex(dst, px - dlx * ax + dx * ay, py - dly * ax + dy * ay, u0, 1.0f);
	}
	return dst;
}

inline Vertex* roundJoin(Vertex* dst, const ContourPoint* p0, const ContourPoint* p1,
                         float lw, float rw, float lu, float ru, int ncap) noexcept {
	float dlx0 = p0->dy;
	float dly0 = -p0->dx;
	float dlx1 = p1->dy;
	float dly1 = -p1->dx;

	if (p1->flags & PointLeft) {
		float lx0, ly0, lx1, ly1;
		computeBevelPoints(p1->flags & PointInnerBevel, p0, p1, lw, &lx0, &ly0, &lx1, &ly1);
		float a0 = std::atan2(-dly0, -dlx0);
		float a1 = std::atan2(-dly1, -dlx1);
		if (a1 > a0) a1 -= PI * 2.0f;

		dst = setVertex(dst, lx0, ly0, lu, 1.0f);
		dst = setVertex(dst, p1->x - dlx0 * rw, p1->y - dly0 * rw, ru, 1.0f);

		int n = std::clamp(static_cast<int>(std::ceil(((a0 - a1) / PI) * ncap)), 2, ncap);
		for (int i = 0; i < n; ++i) {
			float u = i / static_cast<float>(n - 1);
			float a = a0 + u * (a1 - a0);
			float rx = p1->x + std::cos(a) * rw;
			float ry = p1->y + std::sin(a) * rw;
			dst = setVertex(dst, p1->x, p1->y, 0.5f, 1.0f);
			dst = setVertex(dst, rx, ry, ru, 1.0f);
		}

		dst = setVertex(dst, lx1, ly1, lu, 1.0f);
		dst = setVertex(dst, p1->x - dlx1 * rw, p1->y - dly1 * rw, ru, 1.0f);
	} else {
		float rx0, ry0, rx1, ry1;
		computeBevelPoints(p1->flags & PointInnerBevel, p0, p1, -rw, &rx0, &ry0, &rx1, &ry1);
		float a0 = std::atan2(dly0, dlx0);
		float a1 = std::atan2(dly1, dlx1);
		if (a1 < a0) a1 += PI * 2.0f;

		dst = setVertex(dst, p1->x + dlx0 * lw, p1->y + dly0 * lw, lu, 1.0f);
		dst = setVertex(dst, rx0, ry0, ru, 1.0f);

		int n = std::clamp(static_cast<int>(std::ceil(((a1 - a0) / PI) * ncap)), 2, ncap);
		for (int i = 0; i < n; ++i) {
			float u = i / static_cast<float>(n - 1);
			float a = a0 + u * (a1 - a0);
			float lx = p1->x + std::cos(a) * lw;
			float ly = p1->y + std::sin(a) * lw;
			dst = setVertex(dst, lx, ly, lu, 1.0f);
			dst = setVertex(dst, p1->x, p1->y, 0.5f, 1.0f);
		}

		dst = setVertex(dst, p1->x + dlx1 * lw, p1->y + dly1 * lw, lu, 1.0f);
		dst = setVertex(dst, rx1, ry1, ru, 1.0f);
	}
	return dst;
}

inline Vertex* bevelJoin(Vertex* dst, const ContourPoint* p0, const ContourPoint* p1,
                         float lw, float rw, float lu, float ru) noexcept {
	float dlx0 = p0->dy;
	float dly0 = -p0->dx;
	float dlx1 = p1->dy;
	float dly1 = -p1->dx;

	if (p1->flags & PointLeft) {
		float lx0, ly0, lx1, ly1;
		computeBevelPoints(p1->flags & PointInnerBevel, p0, p1, lw, &lx0, &ly0, &lx1, &ly1);
		dst = setVertex(dst, lx0, ly0, lu, 1.0f);
		dst = setVertex(dst, p1->x - dlx0 * rw, p1->y - dly0 * rw, ru, 1.0f);
		dst = setVertex(dst, lx1, ly1, lu, 1.0f);
		dst = setVertex(dst, p1->x - dlx1 * rw, p1->y - dly1 * rw, ru, 1.0f);
	} else {
		float rx0, ry0, rx1, ry1;
		computeBevelPoints(p1->flags & PointInnerBevel, p0, p1, -rw, &rx0, &ry0, &rx1, &ry1);
		dst = setVertex(dst, p1->x + dlx0 * lw, p1->y + dly0 * lw, lu, 1.0f);
		dst = setVertex(dst, rx0, ry0, ru, 1.0f);
		dst = setVertex(dst, p1->x + dlx1 * lw, p1->y + dly1 * lw, lu, 1.0f);
		dst = setVertex(dst, rx1, ry1, ru, 1.0f);
	}
	return dst;
}

} // namespace

// -------------------------------------------------------------
// Tessellator Implementation
// -------------------------------------------------------------
Tessellator::Tessellator() {
	m_points.reserve(256);
	m_contours.reserve(16);
	m_vertices.reserve(512);
}

void Tessellator::clear() {
	m_points.clear();
	m_contours.clear();
	m_vertices.clear();
}

void Tessellator::reset(float distTol, float tessTol, float fringeWidth) {
	m_distTol = distTol;
	m_tessTol = tessTol;
	m_fringeWidth = fringeWidth;
	clear();
}

PathContour* Tessellator::lastContour() {
	if (!m_contours.empty()) return &m_contours.back();
	return nullptr;
}

ContourPoint* Tessellator::lastPoint() {
	if (!m_points.empty()) return &m_points.back();
	return nullptr;
}

void Tessellator::addContour() {
	PathContour contour;
	contour.first = static_cast<int>(m_points.size());
	contour.winding = Winding::CounterClockwise;
	contour.closed = false;
	m_contours.push_back(contour);
}

void Tessellator::addPoint(float x, float y, uint8_t flags) {
	PathContour* contour = lastContour();
	if (!contour) return;

	if (contour->count > 0 && !m_points.empty()) {
		ContourPoint* pt = lastPoint();
		if (pointEquals(pt->x, pt->y, x, y, m_distTol)) {
			pt->flags |= flags;
			return;
		}
	}

	ContourPoint pt;
	pt.x = x;
	pt.y = y;
	pt.flags = flags;
	m_points.push_back(pt);
	contour->count++;
}

void Tessellator::closeContour() {
	PathContour* contour = lastContour();
	if (contour) contour->closed = true;
}

void Tessellator::setContourWinding(Winding winding) {
	PathContour* contour = lastContour();
	if (contour) contour->winding = winding;
}

void Tessellator::tessellateBezier(float x1, float y1, float x2, float y2,
                                   float x3, float y3, float x4, float y4,
                                   int level, uint8_t type) {
	if (level > 10) return;

	float x12 = (x1 + x2) * 0.5f;
	float y12 = (y1 + y2) * 0.5f;
	float x23 = (x2 + x3) * 0.5f;
	float y23 = (y2 + y3) * 0.5f;
	float x34 = (x3 + x4) * 0.5f;
	float y34 = (y3 + y4) * 0.5f;
	float x123 = (x12 + x23) * 0.5f;
	float y123 = (y12 + y23) * 0.5f;

	float dx = x4 - x1;
	float dy = y4 - y1;
	float d2 = std::abs((x2 - x4) * dy - (y2 - y4) * dx);
	float d3 = std::abs((x3 - x4) * dy - (y3 - y4) * dx);

	if ((d2 + d3) * (d2 + d3) < m_tessTol * (dx * dx + dy * dy)) {
		addPoint(x4, y4, type);
		return;
	}

	float x234 = (x23 + x34) * 0.5f;
	float y234 = (y23 + y34) * 0.5f;
	float x1234 = (x123 + x234) * 0.5f;
	float y1234 = (y123 + y234) * 0.5f;

	tessellateBezier(x1, y1, x12, y12, x123, y123, x1234, y1234, level + 1, 0);
	tessellateBezier(x1234, y1234, x234, y234, x34, y34, x4, y4, level + 1, type);
}

void Tessellator::flattenCommands(std::span<const PathCommand> commands) {
	if (!m_contours.empty()) return;

	for (const auto& cmd : commands) {
		switch (cmd.type) {
			case PathCommandType::MoveTo:
				addContour();
				addPoint(cmd.p0.x, cmd.p0.y, PointCorner);
				break;
			case PathCommandType::LineTo:
				addPoint(cmd.p0.x, cmd.p0.y, PointCorner);
				break;
			case PathCommandType::BezierTo: {
				ContourPoint* last = lastPoint();
				if (last) {
					tessellateBezier(last->x, last->y,
					                 cmd.p0.x, cmd.p0.y,
					                 cmd.p1.x, cmd.p1.y,
					                 cmd.p2.x, cmd.p2.y,
					                 0, PointCorner);
				}
				break;
			}
			case PathCommandType::Close:
				closeContour();
				break;
			case PathCommandType::Winding:
				setContourWinding(cmd.winding);
				break;
		}
	}

	finalizeContours();
}

void Tessellator::finalizeContours() {
	m_bounds[0] = m_bounds[1] = 1e6f;
	m_bounds[2] = m_bounds[3] = -1e6f;

	float maxPreserveAbsArea = -1.0f;
	float dominantPreserveArea = 0.0f;

	for (size_t j = 0; j < m_contours.size(); ++j) {
		PathContour& contour = m_contours[j];
		ContourPoint* pts = &m_points[contour.first];

		if (contour.count > 1) {
			ContourPoint* p0 = &pts[contour.count - 1];
			ContourPoint* p1 = &pts[0];
			if (pointEquals(p0->x, p0->y, p1->x, p1->y, m_distTol)) {
				contour.count--;
				contour.closed = true;
			}
		}

		if (contour.count > 2 && contour.winding == Winding::Preserve) {
			const float area = polygonSignedArea(pts, contour.count);
			const float absArea = std::abs(area);
			if (absArea > maxPreserveAbsArea) {
				maxPreserveAbsArea = absArea;
				dominantPreserveArea = area;
			}
		}
	}

	// Ensure dominant outer contour in Preserve paths is CCW so AA fringe points outward,
	// while flipping all Preserve contours uniformly to maintain outer-vs-hole contrast.
	const bool flipAllPreserve = (maxPreserveAbsArea > 0.0f && dominantPreserveArea < 0.0f);

	for (size_t j = 0; j < m_contours.size(); ++j) {
		PathContour& contour = m_contours[j];
		ContourPoint* pts = &m_points[contour.first];

		if (contour.count > 2) {
			if (contour.winding == Winding::Preserve) {
				if (flipAllPreserve) {
					reverseContourPoints(pts, contour.count);
				}
			} else {
				float area = polygonSignedArea(pts, contour.count);
				if ((contour.winding == Winding::CounterClockwise || contour.winding == Winding::EvenOdd) && area < 0.0f)
					reverseContourPoints(pts, contour.count);
				if (contour.winding == Winding::Clockwise && area > 0.0f)
					reverseContourPoints(pts, contour.count);
			}
		}

		for (int k = 0; k < contour.count; ++k) {
			ContourPoint* p0 = &pts[k];
			ContourPoint* p1 = (k + 1 < contour.count) ? &pts[k + 1] : &pts[0];

			p0->dx = p1->x - p0->x;
			p0->dy = p1->y - p0->y;
			p0->len = normalizeVector(&p0->dx, &p0->dy);

			m_bounds[0] = std::min(m_bounds[0], p0->x);
			m_bounds[1] = std::min(m_bounds[1], p0->y);
			m_bounds[2] = std::max(m_bounds[2], p0->x);
			m_bounds[3] = std::max(m_bounds[3], p0->y);
		}
	}
}

void Tessellator::flattenPath(const nisaba::Path& path, const Transform2D& xform) {
	if (!m_contours.empty()) return;

	PathSegmentsIter iter(path);
	while (auto segOpt = iter.next()) {
		const auto& seg = *segOpt;
		switch (seg.type) {
			case PathSegment::Type::MoveTo: {
				Point p = xform.transformPoint(seg.p0.x, seg.p0.y);
				addContour();
				addPoint(p.x, p.y, PointCorner);
				break;
			}
			case PathSegment::Type::LineTo: {
				Point p = xform.transformPoint(seg.p0.x, seg.p0.y);
				addPoint(p.x, p.y, PointCorner);
				break;
			}
			case PathSegment::Type::QuadTo: {
				ContourPoint* last = lastPoint();
				if (last) {
					Point cp = xform.transformPoint(seg.p0.x, seg.p0.y);
					Point ep = xform.transformPoint(seg.p1.x, seg.p1.y);
					float x0 = last->x;
					float y0 = last->y;
					float cx1 = x0 + 2.0f / 3.0f * (cp.x - x0);
					float cy1 = y0 + 2.0f / 3.0f * (cp.y - y0);
					float cx2 = ep.x + 2.0f / 3.0f * (cp.x - ep.x);
					float cy2 = ep.y + 2.0f / 3.0f * (cp.y - ep.y);
					tessellateBezier(x0, y0, cx1, cy1, cx2, cy2, ep.x, ep.y, 0, PointCorner);
				}
				break;
			}
			case PathSegment::Type::CubicTo: {
				ContourPoint* last = lastPoint();
				if (last) {
					Point cp1 = xform.transformPoint(seg.p0.x, seg.p0.y);
					Point cp2 = xform.transformPoint(seg.p1.x, seg.p1.y);
					Point ep  = xform.transformPoint(seg.p2.x, seg.p2.y);
					tessellateBezier(last->x, last->y, cp1.x, cp1.y, cp2.x, cp2.y, ep.x, ep.y, 0, PointCorner);
				}
				break;
			}
			case PathSegment::Type::Close:
				closeContour();
				break;
		}
	}

	finalizeContours();
}

void Tessellator::calculateJoins(float w, LineJoin join, float miterLimit) {
	float iw = (w > 0.0f) ? (1.0f / w) : 0.0f;

	for (size_t i = 0; i < m_contours.size(); ++i) {
		PathContour& contour = m_contours[i];
		ContourPoint* pts = &m_points[contour.first];
		int nleft = 0;
		contour.nbevel = 0;

		for (int j = 0; j < contour.count; ++j) {
			ContourPoint* p0 = (j > 0) ? &pts[j - 1] : &pts[contour.count - 1];
			ContourPoint* p1 = &pts[j];

			float dlx0 = p0->dy;
			float dly0 = -p0->dx;
			float dlx1 = p1->dy;
			float dly1 = -p1->dx;

			p1->dmx = (dlx0 + dlx1) * 0.5f;
			p1->dmy = (dly0 + dly1) * 0.5f;
			float dmr2 = p1->dmx * p1->dmx + p1->dmy * p1->dmy;
			if (dmr2 > 0.000001f) {
				float scale = std::min(1.0f / dmr2, 600.0f);
				p1->dmx *= scale;
				p1->dmy *= scale;
			}

			p1->flags = (p1->flags & PointCorner) ? PointCorner : 0;

			float cross = p1->dx * p0->dy - p0->dx * p1->dy;
			if (cross > 0.0f) {
				nleft++;
				p1->flags |= PointLeft;
			}

			float limit = std::max(1.01f, std::min(p0->len, p1->len) * iw);
			if ((dmr2 * limit * limit) < 1.0f) {
				p1->flags |= PointInnerBevel;
			}

			if (p1->flags & PointCorner) {
				if ((dmr2 * miterLimit * miterLimit) < 1.0f || join == LineJoin::Bevel || join == LineJoin::Round) {
					p1->flags |= PointBevel;
				}
			}

			if ((p1->flags & (PointBevel | PointInnerBevel)) != 0) {
				contour.nbevel++;
			}
		}

		contour.convex = (nleft == contour.count || nleft == 0);
	}
}

bool Tessellator::expandStroke(float w, float fringe, LineCap cap, LineJoin join, float miterLimit) {
	float aa = fringe;
	float u0 = (aa == 0.0f) ? 0.5f : 0.0f;
	float u1 = (aa == 0.0f) ? 0.5f : 1.0f;
	int ncap = curveDivisions(w, PI, m_tessTol);

	w += aa * 0.5f;

	calculateJoins(w, join, miterLimit);

	int cverts = 0;
	for (size_t i = 0; i < m_contours.size(); ++i) {
		const PathContour& contour = m_contours[i];
		int loop = contour.closed ? 1 : 0;
		if (join == LineJoin::Round)
			cverts += (contour.count + contour.nbevel * (ncap + 2) + 1) * 2;
		else
			cverts += (contour.count + contour.nbevel * 5 + 1) * 2;
		if (!loop) {
			if (cap == LineCap::Round)
				cverts += (ncap * 2 + 2) * 2;
			else
				cverts += (3 + 3) * 2;
		}
	}

	m_vertices.resize(cverts);
	Vertex* dst = m_vertices.data();

	for (size_t i = 0; i < m_contours.size(); ++i) {
		PathContour& contour = m_contours[i];
		ContourPoint* pts = &m_points[contour.first];
		contour.fillOffset = 0;
		contour.fillCount = 0;

		Vertex* startPathDst = dst;
		contour.strokeOffset = static_cast<int>(dst - m_vertices.data());
		bool loop = contour.closed;

		int s = loop ? 0 : 1;
		int e = loop ? contour.count : contour.count - 1;

		if (!loop) {
			ContourPoint* p0 = &pts[0];
			ContourPoint* p1 = &pts[1];
			float dx = p1->x - p0->x;
			float dy = p1->y - p0->y;
			normalizeVector(&dx, &dy);
			if (cap == LineCap::Butt)
				dst = buttCapStart(dst, p0, dx, dy, w, -aa * 0.5f, aa, u0, u1);
			else if (cap == LineCap::Square)
				dst = buttCapStart(dst, p0, dx, dy, w, w - aa, aa, u0, u1);
			else if (cap == LineCap::Round)
				dst = roundCapStart(dst, p0, dx, dy, w, ncap, u0, u1);
		}

		for (int j = s; j < e; ++j) {
			ContourPoint* p0 = (j > 0) ? &pts[j - 1] : &pts[contour.count - 1];
			ContourPoint* p1 = &pts[j];

			if ((p1->flags & (PointBevel | PointInnerBevel)) != 0) {
				if (join == LineJoin::Round)
					dst = roundJoin(dst, p0, p1, w, w, u0, u1, ncap);
				else
					dst = bevelJoin(dst, p0, p1, w, w, u0, u1);
			} else {
				dst = setVertex(dst, p1->x + (p1->dmx * w), p1->y + (p1->dmy * w), u0, 1.0f);
				dst = setVertex(dst, p1->x - (p1->dmx * w), p1->y - (p1->dmy * w), u1, 1.0f);
			}
		}

		if (loop) {
			dst = setVertex(dst, startPathDst[0].x, startPathDst[0].y, u0, 1.0f);
			dst = setVertex(dst, startPathDst[1].x, startPathDst[1].y, u1, 1.0f);
		} else {
			ContourPoint* p0 = &pts[contour.count - 2];
			ContourPoint* p1 = &pts[contour.count - 1];
			float dx = p1->x - p0->x;
			float dy = p1->y - p0->y;
			normalizeVector(&dx, &dy);
			if (cap == LineCap::Butt)
				dst = buttCapEnd(dst, p1, dx, dy, w, -aa * 0.5f, aa, u0, u1);
			else if (cap == LineCap::Square)
				dst = buttCapEnd(dst, p1, dx, dy, w, w - aa, aa, u0, u1);
			else if (cap == LineCap::Round)
				dst = roundCapEnd(dst, p1, dx, dy, w, ncap, u0, u1);
		}

		contour.strokeCount = static_cast<int>(dst - startPathDst);
	}

	m_vertices.resize(dst - m_vertices.data());
	return true;
}

bool Tessellator::expandFill(float fringe, LineJoin join, float miterLimit) {
	float aa = m_fringeWidth;
	bool hasFringe = fringe > 0.0f;

	calculateJoins(fringe, join, miterLimit);

	bool convex = (m_contours.size() == 1 && m_contours[0].convex);

	int cverts = 0;
	for (size_t i = 0; i < m_contours.size(); ++i) {
		const PathContour& contour = m_contours[i];
		cverts += contour.count + contour.nbevel + 1;
		if (hasFringe)
			cverts += (contour.count + contour.nbevel * 5 + 1) * 2;
	}

	m_vertices.resize(cverts);
	Vertex* dst = m_vertices.data();

	for (size_t i = 0; i < m_contours.size(); ++i) {
		PathContour& contour = m_contours[i];
		ContourPoint* pts = &m_points[contour.first];

		float woff = 0.5f * aa;
		Vertex* startFillDst = dst;
		contour.fillOffset = static_cast<int>(dst - m_vertices.data());

		if (hasFringe) {
			for (int j = 0; j < contour.count; ++j) {
				ContourPoint* p0 = (j > 0) ? &pts[j - 1] : &pts[contour.count - 1];
				ContourPoint* p1 = &pts[j];

				if (p1->flags & PointBevel) {
					float dlx0 = p0->dy;
					float dly0 = -p0->dx;
					float dlx1 = p1->dy;
					float dly1 = -p1->dx;
					if (p1->flags & PointLeft) {
						dst = setVertex(dst, p1->x + p1->dmx * woff, p1->y + p1->dmy * woff, 0.5f, 1.0f);
					} else {
						dst = setVertex(dst, p1->x + dlx0 * woff, p1->y + dly0 * woff, 0.5f, 1.0f);
						dst = setVertex(dst, p1->x + dlx1 * woff, p1->y + dly1 * woff, 0.5f, 1.0f);
					}
				} else {
					dst = setVertex(dst, p1->x + p1->dmx * woff, p1->y + p1->dmy * woff, 0.5f, 1.0f);
				}
			}
		} else {
			for (int j = 0; j < contour.count; ++j) {
				dst = setVertex(dst, pts[j].x, pts[j].y, 0.5f, 1.0f);
			}
		}

		contour.fillCount = static_cast<int>(dst - startFillDst);

		// Fringe generation
		if (hasFringe) {
			float lw = fringe + woff;
			float rw = fringe - woff;
			float lu = 0.0f;
			float ru = 1.0f;
			Vertex* startStrokeDst = dst;
			contour.strokeOffset = static_cast<int>(dst - m_vertices.data());

			if (convex) {
				lw = woff;
				lu = 0.5f;
			}

			for (int j = 0; j < contour.count; ++j) {
				ContourPoint* p0 = (j > 0) ? &pts[j - 1] : &pts[contour.count - 1];
				ContourPoint* p1 = &pts[j];

				if ((p1->flags & (PointBevel | PointInnerBevel)) != 0) {
					dst = bevelJoin(dst, p0, p1, lw, rw, lu, ru);
				} else {
					dst = setVertex(dst, p1->x + p1->dmx * lw, p1->y + p1->dmy * lw, lu, 1.0f);
					dst = setVertex(dst, p1->x - p1->dmx * rw, p1->y - p1->dmy * rw, ru, 1.0f);
				}
			}

			// Loop fringe
			dst = setVertex(dst, startStrokeDst[0].x, startStrokeDst[0].y, lu, 1.0f);
			dst = setVertex(dst, startStrokeDst[1].x, startStrokeDst[1].y, ru, 1.0f);

			contour.strokeCount = static_cast<int>(dst - startStrokeDst);
		} else {
			contour.strokeOffset = 0;
			contour.strokeCount = 0;
		}
	}

	m_vertices.resize(dst - m_vertices.data());
	return true;
}

} // namespace nisaba::gpu
