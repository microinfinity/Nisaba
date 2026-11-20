#include "nisaba/gpu/context.hpp"
#include "tessellator.hpp"
#include "glyph_atlas.hpp"
#include "shader_uniforms.hpp"
#include "nisaba/text/bidi.hpp"
#include "nisaba/image/image_io.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace nisaba::gpu {

static inline float distPtSeg(float x, float y, float px, float py, float qx, float qy) {
	float pqx = qx - px;
	float pqy = qy - py;
	float dx = x - px;
	float dy = y - py;
	float d = pqx * pqx + pqy * pqy;
	float t = pqx * dx + pqy * dy;
	if (d > 0.0f) t /= d;
	if (t < 0.0f) t = 0.0f;
	else if (t > 1.0f) t = 1.0f;
	dx = px + t * pqx - x;
	dy = py + t * pqy - y;
	return dx * dx + dy * dy;
}

static inline float cross2D(float dx0, float dy0, float dx1, float dy1) {
	return dx1 * dy0 - dx0 * dy1;
}

static inline bool isTransformFlipped(const Transform2D& xform) {
	float det = xform.m[0] * xform.m[3] - xform.m[2] * xform.m[1];
	return det < 0.0f;
}

static inline bool ptEquals(float x1, float y1, float x2, float y2, float tol) {
	float dx = x2 - x1;
	float dy = y2 - y1;
	return dx * dx + dy * dy < tol * tol;
}

static inline float normalizeVec(float* x, float* y) {
	float d = std::sqrt((*x) * (*x) + (*y) * (*y));
	if (d > 1e-6f) {
		float id = 1.0f / d;
		*x *= id;
		*y *= id;
	}
	return d;
}

static inline float quantize(float a, float d) {
	return (static_cast<int>(a / d + 0.5f)) * d;
}

// -------------------------------------------------------------
// Context Lifecycle
// -------------------------------------------------------------
Context::Context(std::unique_ptr<Renderer> renderer, int flags)
	: m_renderer(std::move(renderer)), m_flags(flags) {
	if (!m_renderer) {
		throw std::runtime_error("Renderer cannot be null");
	}

	m_tessellator = new Tessellator();
	m_commands.reserve(256);

	// Setup Initial State
	m_states.reserve(32);
	m_states.emplace_back();
	reset();

	if (!m_renderer->init()) {
		throw std::runtime_error("Failed to initialize GPU renderer");
	}

	// Initialize Sovereign Glyph Atlas & Font Texture (2048x2048 for high throughput)
	m_atlas = std::make_unique<SovereignGlyphAtlas>(2048, 2048);
	m_fontTextureId = m_renderer->createTexture(TextureType::Alpha, m_atlas->width(), m_atlas->height(), 0, m_atlas->data());
	m_atlas->markClean();
}

Context::~Context() {
	if (m_fontTextureId != 0 && m_renderer) {
		m_renderer->deleteTexture(m_fontTextureId);
		m_fontTextureId = 0;
	}
	delete m_tessellator;
	if (m_renderer) {
		m_renderer->shutdown();
	}
}

// -------------------------------------------------------------
// Frame Lifecycle
// -------------------------------------------------------------
void Context::beginFrame(float windowWidth, float windowHeight, float devicePixelRatio) {
	if (m_renderer) {
		m_renderer->cancel();
	}
	m_states.clear();
	m_layerStack.clear();
	m_states.emplace_back();
	reset();

	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;
	m_devicePixelRatio = devicePixelRatio;
	m_tessTol = 0.25f / devicePixelRatio;
	m_distTol = 0.01f / devicePixelRatio;
	m_fringeWidth = 1.0f / devicePixelRatio;

	m_tessellator->reset(m_distTol, m_tessTol, m_fringeWidth);
	m_renderer->viewport(windowWidth, windowHeight, devicePixelRatio);
}

void Context::cancelFrame() {
	m_layerStack.clear();
	m_renderer->cancel();
}

void Context::endFrame() {
	m_renderer->flush();
}

void Context::resetState() {
	if (m_renderer) {
		m_renderer->resetState();
	}
}

// -------------------------------------------------------------
// State Handling
// -------------------------------------------------------------
void Context::save() {
	if (m_states.empty()) {
		m_states.emplace_back();
	}
	m_states.push_back(m_states.back());
	m_states.back().isLayer = false;
}

void Context::pushLayer(const LayerDesc& desc) {
	if (m_states.empty()) {
		m_states.emplace_back();
	}
	const State parentState = m_states.back();
	m_states.push_back(parentState);
	State& layerState = m_states.back();
	layerState.isLayer = true;
	layerState.clipDepth = 0;
	if (desc.hasBounds) {
		layerState.scissor.xform = Transform2D::identity();
		const float bw = std::max(0.0f, desc.bounds[2] - desc.bounds[0]);
		const float bh = std::max(0.0f, desc.bounds[3] - desc.bounds[1]);
		layerState.scissor.xform[4] = desc.bounds[0] + bw * 0.5f;
		layerState.scissor.xform[5] = desc.bounds[1] + bh * 0.5f;
		layerState.scissor.extent[0] = bw * 0.5f;
		layerState.scissor.extent[1] = bh * 0.5f;
	}
	m_layerStack.push_back(LayerEntry{desc, parentState.scissor, parentState.clipDepth});
	if (m_renderer) {
		m_renderer->pushLayer(desc, parentState.scissor, parentState.clipDepth);
	}
}

void Context::restore() {
	if (m_states.size() <= 1) return;
	if (m_states.back().isLayer && !m_layerStack.empty()) {
		LayerEntry entry = std::move(m_layerStack.back());
		m_layerStack.pop_back();
		m_states.pop_back();
		if (m_renderer) {
			m_renderer->popLayer(entry.desc, entry.parentScissor, entry.parentClipDepth);
		}
		return;
	}
	int curDepth = m_states.back().clipDepth;
	m_states.pop_back();
	int targetDepth = m_states.back().clipDepth;
	if (curDepth > targetDepth && m_renderer) {
		m_renderer->restoreClip(targetDepth);
	}
}

void Context::reset() {
	State& s = currentState();
	s = State();
	setPaintColor(s.fill, Color::rgba(255, 255, 255, 255));
	setPaintColor(s.stroke, Color::rgba(0, 0, 0, 255));
	s.compositeOperation = CompositeOperationState::fromCompositeOp(CompositeOperation::SourceOver);
	s.shapeAntiAlias = true;
	s.strokeWidth = 1.0f;
	s.miterLimit = 10.0f;
	s.lineCap = LineCap::Butt;
	s.lineJoin = LineJoin::Miter;
	s.alpha = 1.0f;
	s.xform = Transform2D::identity();
	s.scissor.extent[0] = -1.0f;
	s.scissor.extent[1] = -1.0f;
	s.fontSize = 16.0f;
	s.letterSpacing = 0.0f;
	s.lineHeight = 1.0f;
	s.fontBlur = 0.0f;
	s.textAlign = Align::Left | Align::Baseline;
	s.clipDepth = 0;
	int defFont = findFont("sans");
	if (defFont <= 0 && !m_fontNames.empty()) {
		defFont = m_fontNames.begin()->second;
	}
	s.fontId = (defFont > 0) ? defFont : 0;
}

void Context::setPaintColor(Paint& p, Color color) {
	p.xform = Transform2D::identity();
	p.gradUnitXform = Transform2D::identity();
	p.radius = 0.0f;
	p.feather = 1.0f;
	p.extent[0] = 0.0f;
	p.extent[1] = 0.0f;
	p.innerColor = color;
	p.outerColor = color;
	p.image = 0;
	p.hasColorMatrix = false;
	p.colorTransfer = 0;
	p.hasColorBlend = false;
	p.gradientKind = GradientKind::None;
	p.gradientSpread = SpreadKind::Pad;
	p.gradStopCount = 0;
	p.gradParams0 = {};
	p.gradParams1 = {};
	p.customShader = nullptr;
}

void Context::shapeAntiAlias(bool enabled) {
	currentState().shapeAntiAlias = enabled;
}

void Context::strokeColor(Color color) {
	setPaintColor(currentState().stroke, color);
}

void Context::strokePaint(const Paint& paint) {
	State& s = currentState();
	s.stroke = paint;
	s.stroke.xform = s.stroke.xform.multiply(s.xform);
}

void Context::strokeShader(std::shared_ptr<class Shader> shader) {
	State& s = currentState();
	s.stroke = Paint::custom(std::move(shader));
	s.stroke.xform = s.stroke.xform.multiply(s.xform);
}

void Context::fillColor(Color color) {
	setPaintColor(currentState().fill, color);
}

void Context::fillPaint(const Paint& paint) {
	State& s = currentState();
	s.fill = paint;
	s.fill.xform = s.fill.xform.multiply(s.xform);
}

void Context::fillShader(std::shared_ptr<class Shader> shader) {
	State& s = currentState();
	s.fill = Paint::custom(std::move(shader));
	s.fill.xform = s.fill.xform.multiply(s.xform);
}

void Context::miterLimit(float limit) {
	currentState().miterLimit = limit;
}

void Context::strokeWidth(float size) {
	currentState().strokeWidth = size;
}

void Context::lineCap(LineCap cap) {
	currentState().lineCap = cap;
}

void Context::lineCap(nisaba::LineCap cap) {
	switch (cap) {
		case nisaba::LineCap::Butt: currentState().lineCap = LineCap::Butt; break;
		case nisaba::LineCap::Round: currentState().lineCap = LineCap::Round; break;
		case nisaba::LineCap::Square: currentState().lineCap = LineCap::Square; break;
	}
}

void Context::lineJoin(LineJoin join) {
	currentState().lineJoin = join;
}

void Context::lineJoin(nisaba::LineJoin join) {
	switch (join) {
		case nisaba::LineJoin::Miter: currentState().lineJoin = LineJoin::Miter; break;
		case nisaba::LineJoin::Round: currentState().lineJoin = LineJoin::Round; break;
		case nisaba::LineJoin::Bevel: currentState().lineJoin = LineJoin::Bevel; break;
		default: currentState().lineJoin = LineJoin::Miter; break;
	}
}

void Context::globalAlpha(float alpha) {
	currentState().alpha = alpha;
}

// -------------------------------------------------------------
// Transforms
// -------------------------------------------------------------
void Context::resetTransform() {
	currentState().xform = Transform2D::identity();
}

void Context::transform(float a, float b, float c, float d, float e, float f) {
	State& s = currentState();
	Transform2D t(a, b, c, d, e, f);
	s.xform = s.xform.premultiply(t);
}

void Context::transform(const Transform2D& t) {
	State& s = currentState();
	s.xform = s.xform.premultiply(t);
}

void Context::transform(const nisaba::Transform& ts) {
	transform(Transform2D(ts));
}

void Context::translate(float x, float y) {
	State& s = currentState();
	s.xform = s.xform.premultiply(Transform2D::translate(x, y));
}

void Context::rotate(float angleRad) {
	State& s = currentState();
	s.xform = s.xform.premultiply(Transform2D::rotate(angleRad));
}

void Context::skewX(float angleRad) {
	State& s = currentState();
	s.xform = s.xform.premultiply(Transform2D::skewX(angleRad));
}

void Context::skewY(float angleRad) {
	State& s = currentState();
	s.xform = s.xform.premultiply(Transform2D::skewY(angleRad));
}

void Context::scale(float x, float y) {
	State& s = currentState();
	s.xform = s.xform.premultiply(Transform2D::scale(x, y));
}

Transform2D Context::currentTransform() const {
	return currentState().xform;
}

// -------------------------------------------------------------
// Scissoring
// -------------------------------------------------------------
void Context::scissor(float x, float y, float w, float h) {
	State& s = currentState();
	w = std::max(0.0f, w);
	h = std::max(0.0f, h);

	s.scissor.xform = Transform2D::identity();
	s.scissor.xform.m[4] = x + w * 0.5f;
	s.scissor.xform.m[5] = y + h * 0.5f;
	s.scissor.xform = s.scissor.xform.multiply(s.xform);

	s.scissor.extent[0] = w * 0.5f;
	s.scissor.extent[1] = h * 0.5f;
}

void Context::scissorScreen(float x, float y, float w, float h) {
	State& s = currentState();
	w = std::max(0.0f, w);
	h = std::max(0.0f, h);

	s.scissor.xform = Transform2D::identity();
	s.scissor.xform.m[4] = x + w * 0.5f;
	s.scissor.xform.m[5] = y + h * 0.5f;

	s.scissor.extent[0] = w * 0.5f;
	s.scissor.extent[1] = h * 0.5f;
}

static void isectRects(float* dst,
                       float ax, float ay, float aw, float ah,
                       float bx, float by, float bw, float bh) {
	float minx = std::max(ax, bx);
	float miny = std::max(ay, by);
	float maxx = std::min(ax + aw, bx + bw);
	float maxy = std::min(ay + ah, by + bh);
	dst[0] = minx;
	dst[1] = miny;
	dst[2] = std::max(0.0f, maxx - minx);
	dst[3] = std::max(0.0f, maxy - miny);
}

void Context::intersectScissor(float x, float y, float w, float h) {
	State& s = currentState();
	if (s.scissor.extent[0] < 0.0f) {
		scissor(x, y, w, h);
		return;
	}

	Transform2D invXform;
	s.xform.inverse(invXform);

	Transform2D pxform = s.scissor.xform.multiply(invXform);
	float ex = s.scissor.extent[0];
	float ey = s.scissor.extent[1];

	float tex = ex * std::abs(pxform.m[0]) + ey * std::abs(pxform.m[2]);
	float tey = ex * std::abs(pxform.m[1]) + ey * std::abs(pxform.m[3]);

	float rect[4];
	isectRects(rect, pxform.m[4] - tex, pxform.m[5] - tey, tex * 2.0f, tey * 2.0f, x, y, w, h);
	scissor(rect[0], rect[1], rect[2], rect[3]);
}

void Context::resetScissor() {
	currentState().scissor.extent[0] = -1.0f;
	currentState().scissor.extent[1] = -1.0f;
}

// -------------------------------------------------------------
// Stencil Vector Clipping
// -------------------------------------------------------------
void Context::clipPath(const nisaba::Path& p, Winding winding) {
	State& s = currentState();
	if (s.clipDepth >= 15) return;

	materializePending();
	m_commands.clear();
	m_tessellator->clear();
	path(p);
	materializePending();

	m_tessellator->flattenCommands(m_commands);
	m_tessellator->expandFill(0.0f, LineJoin::Miter, 2.4f);

	const auto& paths = m_tessellator->contours();
	const auto& allVerts = m_tessellator->vertices();
	m_renderPaths.resize(paths.size());

	for (size_t i = 0; i < paths.size(); ++i) {
		const auto& tp = paths[i];
		auto& rp = m_renderPaths[i];
		rp.first = tp.first;
		rp.count = tp.count;
		rp.closed = tp.closed;
		rp.nbevel = tp.nbevel;
		rp.fill = (tp.fillCount > 0) ? &allVerts[tp.fillOffset] : nullptr;
		rp.fillCount = tp.fillCount;
		rp.stroke = nullptr;
		rp.strokeCount = 0;
		rp.winding = tp.winding;
		rp.convex = tp.convex;
	}

	s.clipDepth++;
	if (m_renderer) {
		m_renderer->renderClip(m_renderPaths.data(), static_cast<int>(m_renderPaths.size()),
		                       winding, s.clipDepth, s.scissor, m_tessellator->bounds());
	}
	m_commands.clear();
	m_tessellator->clear();
}

void Context::restoreClip(int targetDepth) {
	State& s = currentState();
	s.clipDepth = targetDepth;
	if (m_renderer) {
		m_renderer->restoreClip(targetDepth);
	}
}

void Context::resetClip() {
	State& s = currentState();
	s.clipDepth = 0;
	if (m_renderer) {
		m_renderer->resetClip();
	}
}

int Context::clipDepth() const noexcept {
	return currentState().clipDepth;
}

// -------------------------------------------------------------
// Global Compositing
// -------------------------------------------------------------
void Context::globalCompositeOperation(CompositeOperation op) {
	currentState().compositeOperation = CompositeOperationState::fromCompositeOp(op);
}

void Context::globalCompositeBlendMode(BlendMode mode) {
	currentState().compositeOperation = CompositeOperationState::fromBlendMode(mode);
}

void Context::globalCompositeBlendFunc(BlendFactor sfactor, BlendFactor dfactor) {
	globalCompositeBlendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}

void Context::globalCompositeBlendFuncSeparate(BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha, BlendFactor dstAlpha) {
	State& s = currentState();
	s.compositeOperation.srcRGB = srcRGB;
	s.compositeOperation.dstRGB = dstRGB;
	s.compositeOperation.srcAlpha = srcAlpha;
	s.compositeOperation.dstAlpha = dstAlpha;
	s.compositeOperation.isProgrammableBlend = false;
}

void Context::setCompositeState(const CompositeOperationState& state) {
	currentState().compositeOperation = state;
}

const CompositeOperationState& Context::currentCompositeState() const {
	return currentState().compositeOperation;
}

// -------------------------------------------------------------
// Images
// -------------------------------------------------------------
int Context::createImage(const char* filename, int imageFlags) {
	if (!filename) return 0;
	auto res = nisaba::image::load_image_file(filename);
	if (!res) return 0;
	return createImageRGBA(static_cast<int>(res->width()),
	                       static_cast<int>(res->height()),
	                       imageFlags,
	                       res->data());
}

int Context::createImageMem(int imageFlags, unsigned char* data, int ndata) {
	if (!data || ndata <= 0) return 0;
	auto res = nisaba::image::load_image_from_memory(std::span<const uint8_t>(data, static_cast<size_t>(ndata)));
	if (!res) return 0;
	return createImageRGBA(static_cast<int>(res->width()),
	                       static_cast<int>(res->height()),
	                       imageFlags,
	                       res->data());
}

int Context::createImageRGBA(int w, int h, int imageFlags, const unsigned char* data) {
	return m_renderer->createTexture(TextureType::RGBA, w, h, imageFlags, data);
}

int Context::createImageWithFormat(int w, int h, int strideBytes, PixelFormat format, int imageFlags, const unsigned char* data) {
	if (!m_renderer || w <= 0 || h <= 0 || !data) return 0;
	switch (format) {
		case PixelFormat::RGBA8888:
			return m_renderer->createTextureWithStride(TextureType::RGBA, w, h, strideBytes, imageFlags, data);
		case PixelFormat::BGRA8888:
			return m_renderer->createTextureWithStride(TextureType::BGRA, w, h, strideBytes, imageFlags, data);
		case PixelFormat::Alpha8:
			return m_renderer->createTextureWithStride(TextureType::Alpha, w, h, strideBytes, imageFlags, data);
		case PixelFormat::RGB565: {
			size_t effStride = (strideBytes > 0) ? static_cast<size_t>(strideBytes) : static_cast<size_t>(w) * 2;
			auto ref = PixmapRef::from_raw_parts(data, static_cast<uint32_t>(w), static_cast<uint32_t>(h), effStride, PixelFormat::RGB565);
			if (!ref) return 0;
			Pixmap rgba = ref->to_rgba8888();
			return m_renderer->createTexture(TextureType::RGBA, w, h, imageFlags | ImageFlags::ImagePremultiplied, rgba.data());
		}
	}
	return 0;
}

void Context::updateImage(int image, const unsigned char* data) {
	int w, h;
	m_renderer->getTextureSize(image, w, h);
	m_renderer->updateTexture(image, 0, 0, w, h, data);
}

void Context::imageSize(int image, int& w, int& h) {
	m_renderer->getTextureSize(image, w, h);
}

void Context::deleteImage(int image) {
	m_renderer->deleteTexture(image);
}

// -------------------------------------------------------------
// Paints
// -------------------------------------------------------------
Paint Context::linearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol) {
	return Paint::linearGradient(sx, sy, ex, ey, icol, ocol);
}

Paint Context::boxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol) {
	return Paint::boxGradient(x, y, w, h, r, f, icol, ocol);
}

Paint Context::radialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol) {
	return Paint::radialGradient(cx, cy, inr, outr, icol, ocol);
}

Paint Context::imagePattern(float cx, float cy, float w, float h, float angleRad, int image, float alpha) {
	return Paint::imagePattern(cx, cy, w, h, angleRad, image, alpha);
}

// -------------------------------------------------------------
// Path Commands
// -------------------------------------------------------------
void Context::materializePending() {
	if (m_singleCircle) {
		m_singleCircle = false;
		ellipsePath(m_circleCx, m_circleCy, m_circleR, m_circleR);
	}
	if (m_singleEllipse) {
		m_singleEllipse = false;
		ellipsePath(m_ellipseCx, m_ellipseCy, m_ellipseRx, m_ellipseRy);
	}
	if (m_singleRect) {
		m_singleRect = false;
		rectPath(m_rectX, m_rectY, m_rectW, m_rectH);
	}
	if (m_singleRRect) {
		m_singleRRect = false;
		roundedRectVaryingPath(m_rrectX, m_rrectY, m_rrectW, m_rrectH, m_rrectR, m_rrectR, m_rrectR, m_rrectR);
	}
	if (m_singleVaryingRRect) {
		m_singleVaryingRRect = false;
		roundedRectVaryingPath(m_rrectX, m_rrectY, m_rrectW, m_rrectH, m_radTL, m_radTR, m_radBR, m_radBL);
	}
}

void Context::appendCommand(const PathCommand& cmd) {
	materializePending();
	State& s = currentState();
	PathCommand transformed = cmd;

	// Always record the current pen position in LOCAL coordinate space!
	switch (cmd.type) {
		case PathCommandType::MoveTo:
		case PathCommandType::LineTo:
			m_commandX = cmd.p0.x;
			m_commandY = cmd.p0.y;
			break;
		case PathCommandType::BezierTo:
			m_commandX = cmd.p2.x;
			m_commandY = cmd.p2.y;
			break;
		default:
			break;
	}

	if (s.xform.isIdentity()) {
		m_singleCircle = false;
		m_singleEllipse = false;
		m_singleRect = false;
		m_singleRRect = false;
		m_singleVaryingRRect = false;
		m_commands.push_back(transformed);
		return;
	}

	switch (cmd.type) {
		case PathCommandType::MoveTo:
		case PathCommandType::LineTo: {
			Point pt = s.xform.transformPoint(cmd.p0.x, cmd.p0.y);
			transformed.p0 = pt;
			break;
		}
		case PathCommandType::BezierTo: {
			Point p1 = s.xform.transformPoint(cmd.p0.x, cmd.p0.y);
			Point p2 = s.xform.transformPoint(cmd.p1.x, cmd.p1.y);
			Point p3 = s.xform.transformPoint(cmd.p2.x, cmd.p2.y);
			transformed.p0 = p1;
			transformed.p1 = p2;
			transformed.p2 = p3;
			break;
		}
		case PathCommandType::Close:
		case PathCommandType::Winding:
			break;
	}

	m_singleCircle = false;
	m_singleEllipse = false;
	m_singleRect = false;
	m_singleRRect = false;
	m_singleVaryingRRect = false;
	m_commands.push_back(transformed);
}

void Context::beginPath() {
	m_commands.clear();
	m_tessellator->clear();
	m_singleCircle = false;
	m_singleEllipse = false;
	m_singleRect = false;
	m_singleRRect = false;
	m_singleVaryingRRect = false;
}

void Context::path(const nisaba::Path& p, Winding defaultWinding) {
	PathSegmentsIter iter(p);
	while (auto seg = iter.next()) {
		switch (seg->type) {
			case PathSegment::Type::MoveTo:
				moveTo(seg->p0.x, seg->p0.y);
				if (defaultWinding != Winding::CounterClockwise) {
					pathWinding(defaultWinding);
				}
				break;
			case PathSegment::Type::LineTo:
				lineTo(seg->p0.x, seg->p0.y);
				break;
			case PathSegment::Type::QuadTo:
				quadTo(seg->p0.x, seg->p0.y, seg->p1.x, seg->p1.y);
				break;
			case PathSegment::Type::CubicTo:
				bezierTo(seg->p0.x, seg->p0.y, seg->p1.x, seg->p1.y, seg->p2.x, seg->p2.y);
				break;
			case PathSegment::Type::Close:
				closePath();
				break;
		}
	}
}

void Context::moveTo(float x, float y) {
	appendCommand(PathCommand::makeMoveTo(Point(x, y)));
}

void Context::lineTo(float x, float y) {
	appendCommand(PathCommand::makeLineTo(Point(x, y)));
}

void Context::bezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
	appendCommand(PathCommand::makeBezierTo(Point(c1x, c1y), Point(c2x, c2y), Point(x, y)));
}

void Context::quadTo(float cx, float cy, float x, float y) {
	float x0 = m_commandX;
	float y0 = m_commandY;
	Point cp1(x0 + 2.0f / 3.0f * (cx - x0), y0 + 2.0f / 3.0f * (cy - y0));
	Point cp2(x + 2.0f / 3.0f * (cx - x), y + 2.0f / 3.0f * (cy - y));
	appendCommand(PathCommand::makeBezierTo(cp1, cp2, Point(x, y)));
}

void Context::arcTo(float x1, float y1, float x2, float y2, float radius) {
	if (m_commands.empty()) return;

	float x0 = m_commandX;
	float y0 = m_commandY;

	if (ptEquals(x0, y0, x1, y1, m_distTol) ||
	    ptEquals(x1, y1, x2, y2, m_distTol) ||
	    distPtSeg(x1, y1, x0, y0, x2, y2) < m_distTol * m_distTol ||
	    radius < m_distTol) {
		lineTo(x1, y1);
		return;
	}

	float dx0 = x0 - x1;
	float dy0 = y0 - y1;
	float dx1 = x2 - x1;
	float dy1 = y2 - y1;
	normalizeVec(&dx0, &dy0);
	normalizeVec(&dx1, &dy1);

	float a = std::acos(dx0 * dx1 + dy0 * dy1);
	float d = radius / std::tan(a * 0.5f);

	if (d > 10000.0f) {
		lineTo(x1, y1);
		return;
	}

	float cx, cy, a0, a1;
	Winding dir;

	if (cross2D(dx0, dy0, dx1, dy1) > 0.0f) {
		cx = x1 + dx0 * d + dy0 * radius;
		cy = y1 + dy0 * d + -dx0 * radius;
		a0 = std::atan2(dx0, -dy0);
		a1 = std::atan2(-dx1, dy1);
		dir = Winding::CounterClockwise;
	} else {
		cx = x1 + dx0 * d + -dy0 * radius;
		cy = y1 + dy0 * d + dx0 * radius;
		a0 = std::atan2(-dx0, dy0);
		a1 = std::atan2(dx1, -dy1);
		dir = Winding::Clockwise;
	}

	arc(cx, cy, radius, a0, a1, dir);
}

void Context::closePath() {
	appendCommand(PathCommand::makeClose());
}

void Context::pathWinding(Winding dir) {
	appendCommand(PathCommand::makeWinding(dir));
}

void Context::arc(float cx, float cy, float r, float a0, float a1, Winding dir) {
	float da = a1 - a0;
	if (dir == Winding::CounterClockwise) {
		if (std::abs(da) >= PI * 2.0f) da = PI * 2.0f;
		else while (da < 0.0f) da += PI * 2.0f;
	} else {
		if (std::abs(da) >= PI * 2.0f) da = -PI * 2.0f;
		else while (da > 0.0f) da -= PI * 2.0f;
	}

	int ndivs = std::max(1, std::min(static_cast<int>(std::abs(da) / (PI * 0.5f) + 0.5f), 5));
	float hda = (da / static_cast<float>(ndivs)) * 0.5f;
	float kappa = std::abs(4.0f / 3.0f * (1.0f - std::cos(hda)) / std::sin(hda));
	if (da < 0.0f) kappa = -kappa;

	float px = 0.0f, py = 0.0f, ptanx = 0.0f, ptany = 0.0f;
	int move = m_commands.empty() ? 0 : 1;

	for (int i = 0; i <= ndivs; i++) {
		float a = a0 + da * (i / static_cast<float>(ndivs));
		float dx = std::cos(a);
		float dy = std::sin(a);
		float x = cx + dx * r;
		float y = cy + dy * r;
		float tanx = -dy * r * kappa;
		float tany = dx * r * kappa;

		if (i == 0) {
			if (move == 0) moveTo(x, y);
			else lineTo(x, y);
		} else {
			bezierTo(px + ptanx, py + ptany, x - tanx, y - tany, x, y);
		}
		px = x;
		py = y;
		ptanx = tanx;
		ptany = tany;
	}
}

void Context::rect(float x, float y, float w, float h) {
	if (m_commands.empty()) {
		m_singleRect = true;
		m_rectX = x;
		m_rectY = y;
		m_rectW = w;
		m_rectH = h;
		return;
	}
	materializePending();
	rectPath(x, y, w, h);
}

void Context::rectPath(float x, float y, float w, float h) {
	moveTo(x, y);
	lineTo(x, y + h);
	lineTo(x + w, y + h);
	lineTo(x + w, y);
	closePath();
}

void Context::roundedRect(float x, float y, float w, float h, float r) {
	if (r < 0.1f) {
		rect(x, y, w, h);
		return;
	}
	if (w < 0.0f) { x += w; w = -w; }
	if (h < 0.0f) { y += h; h = -h; }
	if (m_commands.empty()) {
		m_singleRRect = true;
		m_rrectX = x;
		m_rrectY = y;
		m_rrectW = w;
		m_rrectH = h;
		m_rrectR = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
		return;
	}
	materializePending();
	roundedRectVaryingPath(x, y, w, h, r, r, r, r);
}

void Context::roundedRectVarying(float x, float y, float w, float h,
                                float radTopLeft, float radTopRight,
                                float radBottomRight, float radBottomLeft) {
	if (radTopLeft < 0.1f && radTopRight < 0.1f && radBottomRight < 0.1f && radBottomLeft < 0.1f) {
		rect(x, y, w, h);
		return;
	}
	if (radTopLeft == radTopRight && radTopRight == radBottomRight && radBottomRight == radBottomLeft &&
	    m_commands.empty() && radTopLeft >= 0.1f) {
		roundedRect(x, y, w, h, radTopLeft);
		return;
	}
	if (w < 0.0f) {
		x += w;
		w = -w;
		std::swap(radTopLeft, radTopRight);
		std::swap(radBottomLeft, radBottomRight);
	}
	if (h < 0.0f) {
		y += h;
		h = -h;
		std::swap(radTopLeft, radBottomLeft);
		std::swap(radTopRight, radBottomRight);
	}
	if (m_commands.empty()) {
		m_singleVaryingRRect = true;
		m_rrectX = x;
		m_rrectY = y;
		m_rrectW = w;
		m_rrectH = h;
		float maxR = std::min(w, h) * 0.5f;
		m_radTL = std::clamp(radTopLeft, 0.0f, maxR);
		m_radTR = std::clamp(radTopRight, 0.0f, maxR);
		m_radBR = std::clamp(radBottomRight, 0.0f, maxR);
		m_radBL = std::clamp(radBottomLeft, 0.0f, maxR);
		return;
	}
	materializePending();
	roundedRectVaryingPath(x, y, w, h, radTopLeft, radTopRight, radBottomRight, radBottomLeft);
}

void Context::roundedRectVaryingPath(float x, float y, float w, float h,
                                    float radTopLeft, float radTopRight,
                                    float radBottomRight, float radBottomLeft) {

	float halfw = std::abs(w) * 0.5f;
	float halfh = std::abs(h) * 0.5f;
	float rxBL = std::min(radBottomLeft, halfw) * (w < 0.0f ? -1.0f : 1.0f);
	float ryBL = std::min(radBottomLeft, halfh) * (h < 0.0f ? -1.0f : 1.0f);
	float rxBR = std::min(radBottomRight, halfw) * (w < 0.0f ? -1.0f : 1.0f);
	float ryBR = std::min(radBottomRight, halfh) * (h < 0.0f ? -1.0f : 1.0f);
	float rxTR = std::min(radTopRight, halfw) * (w < 0.0f ? -1.0f : 1.0f);
	float ryTR = std::min(radTopRight, halfh) * (h < 0.0f ? -1.0f : 1.0f);
	float rxTL = std::min(radTopLeft, halfw) * (w < 0.0f ? -1.0f : 1.0f);
	float ryTL = std::min(radTopLeft, halfh) * (h < 0.0f ? -1.0f : 1.0f);

	auto addCorner = [this](float cx, float cy, float rx, float ry, float c0, float s0) {
		float maxR = std::max(std::abs(rx), std::abs(ry));
		if (maxR < 0.1f) return;
		float deltaAngle = std::acos(maxR / (maxR + m_tessTol)) * 2.0f;
		int ndivs = std::clamp(static_cast<int>(std::ceil((PI * 0.5f) / deltaAngle)), 2, 8);
		float step = (PI * 0.5f) / static_cast<float>(ndivs);
		float Cs = std::cos(step);
		float Ss = std::sin(step);
		float c = c0;
		float s = s0;
		for (int i = 1; i <= ndivs; ++i) {
			float cnext = c * Cs + s * Ss;
			float snext = s * Cs - c * Ss;
			c = cnext;
			s = snext;
			lineTo(cx + c * rx, cy + s * ry);
		}
	};

	moveTo(x, y + ryTL);
	lineTo(x, y + h - ryBL);
	if (std::abs(rxBL) >= 0.1f && std::abs(ryBL) >= 0.1f) {
		addCorner(x + rxBL, y + h - ryBL, rxBL, ryBL, -1.0f, 0.0f);
	} else {
		lineTo(x, y + h);
		lineTo(x + rxBL, y + h);
	}

	lineTo(x + w - rxBR, y + h);
	if (std::abs(rxBR) >= 0.1f && std::abs(ryBR) >= 0.1f) {
		addCorner(x + w - rxBR, y + h - ryBR, rxBR, ryBR, 0.0f, 1.0f);
	} else {
		lineTo(x + w, y + h);
		lineTo(x + w, y + h - ryBR);
	}

	lineTo(x + w, y + ryTR);
	if (std::abs(rxTR) >= 0.1f && std::abs(ryTR) >= 0.1f) {
		addCorner(x + w - rxTR, y + ryTR, rxTR, ryTR, 1.0f, 0.0f);
	} else {
		lineTo(x + w, y);
		lineTo(x + w - rxTR, y);
	}

	lineTo(x + rxTL, y);
	if (std::abs(rxTL) >= 0.1f && std::abs(ryTL) >= 0.1f) {
		addCorner(x + rxTL, y + ryTL, rxTL, ryTL, 0.0f, -1.0f);
	} else {
		lineTo(x, y);
		lineTo(x, y + ryTL);
	}

	closePath();
}

void Context::ellipse(float cx, float cy, float rx, float ry) {
	if (std::abs(rx - ry) < 1e-4f) {
		circle(cx, cy, rx);
		return;
	}
	if (m_commands.empty()) {
		m_singleEllipse = true;
		m_ellipseCx = cx;
		m_ellipseCy = cy;
		m_ellipseRx = std::abs(rx);
		m_ellipseRy = std::abs(ry);
		return;
	}
	materializePending();
	ellipsePath(cx, cy, rx, ry);
}

void Context::ellipsePath(float cx, float cy, float rx, float ry) {
	float maxR = std::max(std::abs(rx), std::abs(ry));
	if (maxR <= 0.0f) return;
	float deltaAngle = std::acos(maxR / (maxR + m_tessTol)) * 2.0f;
	int n = std::clamp(static_cast<int>(std::ceil(2.0f * PI / deltaAngle)), 12, 64);
	float step = (2.0f * PI) / static_cast<float>(n);

	float cs = std::cos(step);
	float ss = std::sin(step);
	float c = 1.0f;
	float s = 0.0f;

	moveTo(cx + rx, cy);
	for (int i = 1; i < n; ++i) {
		float cnext = c * cs - s * ss;
		float snext = s * cs + c * ss;
		c = cnext;
		s = snext;
		lineTo(cx + rx * c, cy + ry * s);
	}
	closePath();
}

void Context::circle(float cx, float cy, float r) {
	if (m_commands.empty()) {
		m_singleCircle = true;
		m_circleCx = cx;
		m_circleCy = cy;
		m_circleR = r;
		return;
	}
	materializePending();
	ellipsePath(cx, cy, r, r);
}

// -------------------------------------------------------------
// Draw Operations (Fill & Stroke)
// -------------------------------------------------------------
namespace {
struct UnitCircleLUT {
	float cos48[49];
	float sin48[49];
	float cos32[33];
	float sin32[33];
	UnitCircleLUT() {
		for (int i = 0; i <= 48; ++i) {
			float a = (static_cast<float>(i) / 48.0f) * (2.0f * 3.14159265358979323846f);
			cos48[i] = std::cos(a);
			sin48[i] = std::sin(a);
		}
		for (int i = 0; i <= 32; ++i) {
			float a = (static_cast<float>(i) / 32.0f) * (2.0f * 3.14159265358979323846f);
			cos32[i] = std::cos(a);
			sin32[i] = std::sin(a);
		}
	}
};
static const UnitCircleLUT g_circleLut;
} // namespace

void Context::fill() {
	State& s = currentState();
	Paint fillPaint = s.fill;

	bool isAxisAligned = (std::abs(s.xform[1]) < 1e-4f && std::abs(s.xform[2]) < 1e-4f &&
	                      s.xform[0] > 0.0f && s.xform[3] > 0.0f);
	float det = s.xform[0] * s.xform[3] - s.xform[1] * s.xform[2];
	bool isConformalRot = std::abs(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1] -
	                               (s.xform[2] * s.xform[2] + s.xform[3] * s.xform[3])) < 1e-4f &&
	                      std::abs(s.xform[0] * s.xform[2] + s.xform[1] * s.xform[3]) < 1e-4f &&
	                      det > 0.0f;

	bool isSolid = (fillPaint.customShader == nullptr && !fillPaint.hasAdvancedGradient() &&
	                fillPaint.image == 0 && fillPaint.extent[0] == 0.0f && fillPaint.extent[1] == 0.0f &&
	                fillPaint.radius == 0.0f &&
	                fillPaint.innerColor.r == fillPaint.outerColor.r &&
	                fillPaint.innerColor.g == fillPaint.outerColor.g &&
	                fillPaint.innerColor.b == fillPaint.outerColor.b &&
	                fillPaint.innerColor.a == fillPaint.outerColor.a);

	bool isRadialGrad = (fillPaint.customShader == nullptr && !fillPaint.hasAdvancedGradient() && fillPaint.image == 0 &&
	                     fillPaint.radius > 0.0f && fillPaint.extent[0] == fillPaint.radius && fillPaint.extent[1] == fillPaint.radius);
	bool isLinearGrad = (fillPaint.customShader == nullptr && !fillPaint.hasAdvancedGradient() && fillPaint.image == 0 &&
	                     fillPaint.radius == 0.0f && fillPaint.extent[0] == 0.0f && fillPaint.extent[1] > 0.0f);
	bool isImagePat   = (fillPaint.customShader == nullptr && !fillPaint.hasAdvancedGradient() && fillPaint.image != 0);

	Scissor noScissor{};
	bool hasAxisAlignedScissor = (s.scissor.extent[0] >= 0.0f &&
	                              std::abs(s.scissor.xform[1]) < 1e-4f && std::abs(s.scissor.xform[2]) < 1e-4f &&
	                              s.scissor.xform[0] > 0.0f && s.scissor.xform[3] > 0.0f);
	const Scissor& quadScissor = (s.scissor.extent[0] < 0.0f || hasAxisAlignedScissor) ? noScissor : s.scissor;

	auto clipAxisAlignedQuadToScissor = [&](float& x0, float& y0, float& x1, float& y1,
	                                        float& u0, float& v0, float& u1, float& v1) -> bool {
		float sMinX = 0.0f;
		float sMaxX = m_windowWidth;
		float sMinY = 0.0f;
		float sMaxY = m_windowHeight;
		if (hasAxisAlignedScissor) {
			float scx = s.scissor.xform[4];
			float scy = s.scissor.xform[5];
			float shw = s.scissor.extent[0] * s.scissor.xform[0];
			float shh = s.scissor.extent[1] * s.scissor.xform[3];
			sMinX = std::max(sMinX, scx - shw);
			sMaxX = std::min(sMaxX, scx + shw);
			sMinY = std::max(sMinY, scy - shh);
			sMaxY = std::min(sMaxY, scy + shh);
		}
		if (x1 <= sMinX || x0 >= sMaxX || y1 <= sMinY || y0 >= sMaxY) {
			return false;
		}
		float w = x1 - x0;
		float h = y1 - y0;
		if (w > 1e-4f && h > 1e-4f && (x0 < sMinX || x1 > sMaxX || y0 < sMinY || y1 > sMaxY)) {
			float nx0 = std::max(x0, sMinX);
			float nx1 = std::min(x1, sMaxX);
			float ny0 = std::max(y0, sMinY);
			float ny1 = std::min(y1, sMaxY);
			float du = u1 - u0;
			float dv = v1 - v0;
			u0 = u0 + ((nx0 - x0) / w) * du;
			u1 = u1 - ((x1 - nx1) / w) * du;
			v0 = v0 + ((ny0 - y0) / h) * dv;
			v1 = v1 - ((y1 - ny1) / h) * dv;
			x0 = nx0;
			x1 = nx1;
			y0 = ny0;
			y1 = ny1;
		}
		return true;
	};

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleCircle && isConformalRot && (isSolid || isRadialGrad || isLinearGrad || isImagePat) && m_circleR > 0.0f) {
		float sc = std::sqrt(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1]);
		Point center = s.xform.transformPoint(m_circleCx, m_circleCy);
		float cx = center.x;
		float cy = center.y;
		float r = m_circleR * sc;

		fillPaint.innerColor.a *= s.alpha;
		fillPaint.outerColor.a *= s.alpha;

		float Rq = r + 1.5f;
		float scale = Rq / r;
		uint32_t c = fillPaint.innerColor.premultiplied().toRGBA8();

		float x0 = cx - Rq, y0 = cy - Rq, x1 = cx + Rq, y1 = cy + Rq;
		Vertex quad[6];
		if (isImagePat || isLinearGrad) {
			float u0 = -Rq, v0 = -Rq, u1 = Rq, v1 = Rq;
			if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;
			quad[0] = Vertex(x0, y0, u0, v0, c);
			quad[1] = Vertex(x0, y1, u0, v1, c);
			quad[2] = Vertex(x1, y1, u1, v1, c);
			quad[3] = Vertex(x0, y0, u0, v0, c);
			quad[4] = Vertex(x1, y1, u1, v1, c);
			quad[5] = Vertex(x1, y0, u1, v0, c);
			fillPaint.xform.m[4] -= cx;
			fillPaint.xform.m[5] -= cy;
			if (isImagePat) {
				fillPaint.radius = r;
				fillPaint.feather = r;
				fillPaint.outerColor.r = r;
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 1 /* ShaderFillImg */);
			} else {
				fillPaint.extent[0] = r;
				fillPaint.radius = r;
				fillPaint.feather = r;
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 11 /* ShaderRRectGradient */);
			}
			return;
		}

		if (isRadialGrad &&
		    std::abs(fillPaint.xform[4] - cx) < 1e-3f && std::abs(fillPaint.xform[5] - cy) < 1e-3f &&
		    std::abs(fillPaint.xform[1]) < 1e-4f && std::abs(fillPaint.xform[2]) < 1e-4f &&
		    std::abs(fillPaint.xform[0] - 1.0f) < 1e-4f && std::abs(fillPaint.xform[3] - 1.0f) < 1e-4f) {
			float innerR = fillPaint.radius - fillPaint.feather * 0.5f;
			// Hollow radial wave ring (innerColor == outerColor and innerR > 16):
			// Skip the transparent interior disk [0, innerR] completely!
			if (innerR > 16.0f &&
			    fillPaint.innerColor.r == fillPaint.outerColor.r &&
			    fillPaint.innerColor.g == fillPaint.outerColor.g &&
			    fillPaint.innerColor.b == fillPaint.outerColor.b &&
			    std::abs(fillPaint.innerColor.a - fillPaint.outerColor.a) < 1e-4f) {
				m_textVertices.resize(32 * 6);
				Vertex* dst = m_textVertices.data();
				for (int i = 0; i < 32; ++i) {
					float c0 = g_circleLut.cos32[i],     s0 = g_circleLut.sin32[i];
					float c1 = g_circleLut.cos32[i + 1], s1 = g_circleLut.sin32[i + 1];
					float ox0 = cx + r      * c0, oy0 = cy + r      * s0;
					float ix0 = cx + innerR * c0, iy0 = cy + innerR * s0;
					float ox1 = cx + r      * c1, oy1 = cy + r      * s1;
					float ix1 = cx + innerR * c1, iy1 = cy + innerR * s1;
					dst[0] = Vertex(ox0, oy0, ox0 - cx, oy0 - cy, c);
					dst[1] = Vertex(ix0, iy0, ix0 - cx, iy0 - cy, c);
					dst[2] = Vertex(ox1, oy1, ox1 - cx, oy1 - cy, c);
					dst[3] = Vertex(ix0, iy0, ix0 - cx, iy0 - cy, c);
					dst[4] = Vertex(ix1, iy1, ix1 - cx, iy1 - cy, c);
					dst[5] = Vertex(ox1, oy1, ox1 - cx, oy1 - cy, c);
					dst += 6;
				}
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, m_textVertices.data(), 32 * 6, m_fringeWidth, 12 /* Vertex-interpolated RadialGrad */);
				return;
			}
			// Concentric radial gradient on circle: ftcoord = local position in pixels (u in [-Rq, Rq])
			// Encode r in paintMat[0] so all concentric circles with same radius & gradient coalesce into 1 draw call!
			float u0 = -Rq, v0 = -Rq, u1 = Rq, v1 = Rq;
			if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;
			quad[0] = Vertex(x0, y0, u0, v0, c);
			quad[1] = Vertex(x0, y1, u0, v1, c);
			quad[2] = Vertex(x1, y1, u1, v1, c);
			quad[3] = Vertex(x0, y0, u0, v0, c);
			quad[4] = Vertex(x1, y1, u1, v1, c);
			quad[5] = Vertex(x1, y0, u1, v0, c);
			fillPaint.xform = Transform2D::identity();
			fillPaint.xform.m[0] = r;
			m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 15 /* Concentric Circle RadialGrad */);
			return;
		}

		float u0 = -scale, v0 = -scale, u1 = scale, v1 = scale;
		if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;

		// Triangle 1 (p0 -> p3 -> p2: CCW in NDC)
		quad[0] = Vertex(x0, y0, u0, v0, c);
		quad[1] = Vertex(x0, y1, u0, v1, c);
		quad[2] = Vertex(x1, y1, u1, v1, c);
		// Triangle 2 (p0 -> p2 -> p1: CCW in NDC)
		quad[3] = Vertex(x0, y0, u0, v0, c);
		quad[4] = Vertex(x1, y1, u1, v1, c);
		quad[5] = Vertex(x1, y0, u1, v0, c);

		int sType = isSolid ? 7 /* ShaderCircle */ : 6 /* ShaderRadialGrad */;
		m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, sType);
		return;
	}

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleEllipse && det > 1e-6f && isSolid && m_ellipseRx > 0.0f && m_ellipseRy > 0.0f) {
		fillPaint.innerColor.a *= s.alpha;
		fillPaint.outerColor.a *= s.alpha;
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		uint32_t c = fillPaint.innerColor.premultiplied().toRGBA8();

		if (isAxisAligned) {
			float sx = s.xform[0];
			float sy = s.xform[3];
			float cx = m_ellipseCx * sx + s.xform[4];
			float cy = m_ellipseCy * sy + s.xform[5];
			float rx = m_ellipseRx * sx;
			float ry = m_ellipseRy * sy;
			float Rqx = rx + fringe;
			float Rqy = ry + fringe;
			float scaleX = Rqx / rx;
			float scaleY = Rqy / ry;

			float x0 = cx - Rqx, y0 = cy - Rqy, x1 = cx + Rqx, y1 = cy + Rqy;
			float u0 = -scaleX, v0 = -scaleY, u1 = scaleX, v1 = scaleY;
			if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;

			Vertex quad[6];
			quad[0] = Vertex(x0, y0, u0, v0, c);
			quad[1] = Vertex(x0, y1, u0, v1, c);
			quad[2] = Vertex(x1, y1, u1, v1, c);
			quad[3] = Vertex(x0, y0, u0, v0, c);
			quad[4] = Vertex(x1, y1, u1, v1, c);
			quad[5] = Vertex(x1, y0, u1, v0, c);

			m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 7 /* ShaderCircle */);
			return;
		} else {
			float col0 = std::sqrt(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1]);
			float col1 = std::sqrt(s.xform[2] * s.xform[2] + s.xform[3] * s.xform[3]);
			float fx = (fringe > 0.0f) ? (fringe * col1 / det) : 0.0f;
			float fy = (fringe > 0.0f) ? (fringe * col0 / det) : 0.0f;

			float Rqx = m_ellipseRx + fx;
			float Rqy = m_ellipseRy + fy;
			float scaleX = Rqx / m_ellipseRx;
			float scaleY = Rqy / m_ellipseRy;

			Point p0 = s.xform.transformPoint(m_ellipseCx - Rqx, m_ellipseCy - Rqy);
			Point p1 = s.xform.transformPoint(m_ellipseCx + Rqx, m_ellipseCy - Rqy);
			Point p2 = s.xform.transformPoint(m_ellipseCx + Rqx, m_ellipseCy + Rqy);
			Point p3 = s.xform.transformPoint(m_ellipseCx - Rqx, m_ellipseCy + Rqy);

			Vertex quad[6] = {
				Vertex(p0.x, p0.y, -scaleX, -scaleY, c),
				Vertex(p3.x, p3.y, -scaleX,  scaleY, c),
				Vertex(p2.x, p2.y,  scaleX,  scaleY, c),
				Vertex(p0.x, p0.y, -scaleX, -scaleY, c),
				Vertex(p2.x, p2.y,  scaleX,  scaleY, c),
				Vertex(p1.x, p1.y,  scaleX, -scaleY, c)
			};

			m_renderer->renderTriangles(fillPaint, s.compositeOperation, s.scissor, quad, 6, m_fringeWidth, 7 /* ShaderCircle */);
			return;
		}
	}

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleRect && det > 1e-6f && m_rectW > 0.0f && m_rectH > 0.0f) {
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		fillPaint.innerColor.a *= s.alpha;
		fillPaint.outerColor.a *= s.alpha;
		uint32_t c = fillPaint.innerColor.premultiplied().toRGBA8();

		if (isAxisAligned) {
			float sx = s.xform[0];
			float sy = s.xform[3];
			float fx = fringe / sx;
			float fy = fringe / sy;

			float xA = m_rectX - fx;
			float xB = m_rectX + m_rectW + fx;
			float yA = m_rectY - fy;
			float yB = m_rectY + m_rectH + fy;

			Point p0 = s.xform.transformPoint(xA, yA);
			Point p2 = s.xform.transformPoint(xB, yB);

			bool coversViewport = (s.scissor.extent[0] < 0.0f) &&
			                      (p0.x <= 0.0f && p0.y <= 0.0f && p2.x >= m_windowWidth && p2.y >= m_windowHeight);

			if (isSolid && (fringe <= 0.0f || coversViewport)) {
				float x0 = p0.x, y0 = p0.y, x1 = p2.x, y1 = p2.y;
				float u0 = 0.5f, v0 = 1.0f, u1 = 0.5f, v1 = 1.0f;
				if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;
				Vertex quad[6];
				quad[0] = Vertex(x0, y0, 0.5f, 1.0f, c);
				quad[1] = Vertex(x0, y1, 0.5f, 1.0f, c);
				quad[2] = Vertex(x1, y1, 0.5f, 1.0f, c);
				quad[3] = Vertex(x0, y0, 0.5f, 1.0f, c);
				quad[4] = Vertex(x1, y1, 0.5f, 1.0f, c);
				quad[5] = Vertex(x1, y0, 0.5f, 1.0f, c);
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 2 /* ShaderSimple (uses innerCol) */);
				return;
			}

			if (fillPaint.customShader != nullptr) {
				float x0 = m_rectX * sx + s.xform[4];
				float y0 = m_rectY * sy + s.xform[5];
				float x1 = (m_rectX + m_rectW) * sx + s.xform[4];
				float y1 = (m_rectY + m_rectH) * sy + s.xform[5];
				float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
				if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;

				Vertex quad[6];
				quad[0] = Vertex(x0, y0, 0.5f, 1.0f, c);
				quad[1] = Vertex(x0, y1, 0.5f, 1.0f, c);
				quad[2] = Vertex(x1, y1, 0.5f, 1.0f, c);
				quad[3] = Vertex(x0, y0, 0.5f, 1.0f, c);
				quad[4] = Vertex(x1, y1, 0.5f, 1.0f, c);
				quad[5] = Vertex(x1, y0, 0.5f, 1.0f, c);
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, -1 /* derive from paint */);
				return;
			}

			if (!isSolid && !fillPaint.hasAdvancedGradient()) {
				float x0 = m_rectX * sx + s.xform[4];
				float y0 = m_rectY * sy + s.xform[5];
				float x1 = (m_rectX + m_rectW) * sx + s.xform[4];
				float y1 = (m_rectY + m_rectH) * sy + s.xform[5];
				float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
				if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) return;

				Transform2D invxform;
				fillPaint.xform.inverse(invxform);

				if (isLinearGrad) {
					float invFeather = (fillPaint.feather > 1e-4f) ? (1.0f / fillPaint.feather) : 1.0f;
					auto evalY = [&](float px, float py) {
						return (invxform[1] * px + invxform[3] * py + invxform[5]) * invFeather;
					};
					float g00 = evalY(x0, y0);
					float g01 = evalY(x0, y1);
					float g11 = evalY(x1, y1);
					float g10 = evalY(x1, y0);
					Vertex quad[6] = {
						Vertex(x0, y0, 0.5f, g00, c),
						Vertex(x0, y1, 0.5f, g01, c),
						Vertex(x1, y1, 0.5f, g11, c),
						Vertex(x0, y0, 0.5f, g00, c),
						Vertex(x1, y1, 0.5f, g11, c),
						Vertex(x1, y0, 0.5f, g10, c)
					};
					m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 14 /* Vertex-interpolated LinearGrad */);
					return;
				}

				if (isRadialGrad) {
					if (fillPaint.outerColor.a == 0.0f &&
					    !s.compositeOperation.isProgrammableBlend &&
					    s.compositeOperation.dstRGB != BlendFactor::Zero &&
					    std::abs(fillPaint.xform[1]) < 1e-4f && std::abs(fillPaint.xform[2]) < 1e-4f &&
					    std::abs(fillPaint.xform[0] - 1.0f) < 1e-4f && std::abs(fillPaint.xform[3] - 1.0f) < 1e-4f) {
						float rcx = fillPaint.xform[4];
						float rcy = fillPaint.xform[5];
						float R = fillPaint.radius + fillPaint.feather * 0.5f;
						if (x0 <= rcx - R && x1 >= rcx + R && y0 <= rcy - R && y1 >= rcy + R) {
							// Tight circumscribed 12-gon (dodecagon) eliminates 98.2% of empty corner overdraw!
							// tan(15 deg) = 0.26794919f, sec(15 deg) = 1.03527618f
							float t1 = 0.26794919f * R;
							float t2 = 0.76732699f * R;
							float uvX[12] = { -t1,  t1,  t2,  R,  R,  t2,  t1, -t1, -t2, -R, -R, -t2 };
							float uvY[12] = { -R,  -R,  -t2, -t1, t1, t2,  R,   R,   t2,  t1, -t1, -t2 };
							Vertex tris[30];
							for (int k = 0; k < 10; ++k) {
								tris[k * 3 + 0] = Vertex(rcx + uvX[0],     rcy + uvY[0],     uvX[0],     uvY[0],     c);
								tris[k * 3 + 1] = Vertex(rcx + uvX[k + 2], rcy + uvY[k + 2], uvX[k + 2], uvY[k + 2], c);
								tris[k * 3 + 2] = Vertex(rcx + uvX[k + 1], rcy + uvY[k + 1], uvX[k + 1], uvY[k + 1], c);
							}
							m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, tris, 30, m_fringeWidth, 12 /* Vertex-interpolated RadialGrad */);
							return;
						}
						x0 = std::max(x0, rcx - R);
						x1 = std::min(x1, rcx + R);
						y0 = std::max(y0, rcy - R);
						y1 = std::min(y1, rcy + R);
						if (x0 >= x1 || y0 >= y1) return;
					}
					Point pt00 = invxform.transformPoint(x0, y0);
					Point pt01 = invxform.transformPoint(x0, y1);
					Point pt11 = invxform.transformPoint(x1, y1);
					Point pt10 = invxform.transformPoint(x1, y0);
					Vertex quad[6] = {
						Vertex(x0, y0, pt00.x, pt00.y, c),
						Vertex(x0, y1, pt01.x, pt01.y, c),
						Vertex(x1, y1, pt11.x, pt11.y, c),
						Vertex(x0, y0, pt00.x, pt00.y, c),
						Vertex(x1, y1, pt11.x, pt11.y, c),
						Vertex(x1, y0, pt10.x, pt10.y, c)
					};
					m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 12 /* Vertex-interpolated RadialGrad */);
					return;
				}

				if (isImagePat) {
					const bool flipY = (m_renderer->textureFlags(fillPaint.image) & ImageFlags::ImageFlipY) != 0;
					Transform2D uvXform = computeImageUvTransform(fillPaint.xform, fillPaint.extent[0], fillPaint.extent[1], flipY);
					Point uv00 = uvXform.transformPoint(x0, y0);
					Point uv01 = uvXform.transformPoint(x0, y1);
					Point uv11 = uvXform.transformPoint(x1, y1);
					Point uv10 = uvXform.transformPoint(x1, y0);
					Vertex quad[6] = {
						Vertex(x0, y0, uv00.x, uv00.y, c),
						Vertex(x0, y1, uv01.x, uv01.y, c),
						Vertex(x1, y1, uv11.x, uv11.y, c),
						Vertex(x0, y0, uv00.x, uv00.y, c),
						Vertex(x1, y1, uv11.x, uv11.y, c),
						Vertex(x1, y0, uv10.x, uv10.y, c)
					};
					fillPaint.radius = 1.0f;
					m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 3 /* ShaderImg */);
					return;
				}

				// Box Gradient
				Point pt00 = invxform.transformPoint(x0, y0);
				Point pt01 = invxform.transformPoint(x0, y1);
				Point pt11 = invxform.transformPoint(x1, y1);
				Point pt10 = invxform.transformPoint(x1, y0);
				Vertex quad[6] = {
					Vertex(x0, y0, pt00.x, pt00.y, c),
					Vertex(x0, y1, pt01.x, pt01.y, c),
					Vertex(x1, y1, pt11.x, pt11.y, c),
					Vertex(x0, y0, pt00.x, pt00.y, c),
					Vertex(x1, y1, pt11.x, pt11.y, c),
					Vertex(x1, y0, pt10.x, pt10.y, c)
				};
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 13 /* Vertex-interpolated BoxGrad */);
				return;
			}

			if (isSolid) {
				float uA = (fringe > 0.0f) ? (-1.0f - (2.0f * fx) / m_rectW) : -1.0f;
				float uB = (fringe > 0.0f) ? ( 1.0f + (2.0f * fx) / m_rectW) :  1.0f;
				float vA = (fringe > 0.0f) ? (-1.0f - (2.0f * fy) / m_rectH) : -1.0f;
				float vB = (fringe > 0.0f) ? ( 1.0f + (2.0f * fy) / m_rectH) :  1.0f;

				float x0 = p0.x, y0 = p0.y, x1 = p2.x, y1 = p2.y;
				if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, uA, vA, uB, vB)) return;

				Vertex quad[6];
				quad[0] = Vertex(x0, y0, uA, vA, c);
				quad[1] = Vertex(x0, y1, uA, vB, c);
				quad[2] = Vertex(x1, y1, uB, vB, c);
				quad[3] = Vertex(x0, y0, uA, vA, c);
				quad[4] = Vertex(x1, y1, uB, vB, c);
				quad[5] = Vertex(x1, y0, uB, vA, c);

				m_renderer->renderTriangles(fillPaint, s.compositeOperation, quadScissor, quad, 6, m_fringeWidth, 8 /* ShaderRect */);
				return;
			}
		} else if (isSolid) {
			// General affine-transformed solid rectangle (rotated, scaled, or skewed)
			float col0 = std::sqrt(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1]);
			float col1 = std::sqrt(s.xform[2] * s.xform[2] + s.xform[3] * s.xform[3]);
			float fx = (fringe > 0.0f) ? (fringe * col1 / det) : 0.0f;
			float fy = (fringe > 0.0f) ? (fringe * col0 / det) : 0.0f;

			float xA = m_rectX - fx;
			float xB = m_rectX + m_rectW + fx;
			float yA = m_rectY - fy;
			float yB = m_rectY + m_rectH + fy;

			float uA = (fringe > 0.0f) ? (-1.0f - (2.0f * fx) / m_rectW) : -1.0f;
			float uB = (fringe > 0.0f) ? ( 1.0f + (2.0f * fx) / m_rectW) :  1.0f;
			float vA = (fringe > 0.0f) ? (-1.0f - (2.0f * fy) / m_rectH) : -1.0f;
			float vB = (fringe > 0.0f) ? ( 1.0f + (2.0f * fy) / m_rectH) :  1.0f;

			Point p0 = s.xform.transformPoint(xA, yA);
			Point p1 = s.xform.transformPoint(xB, yA);
			Point p2 = s.xform.transformPoint(xB, yB);
			Point p3 = s.xform.transformPoint(xA, yB);

			Vertex quad[6] = {
				Vertex(p0.x, p0.y, uA, vA, c),
				Vertex(p3.x, p3.y, uA, vB, c),
				Vertex(p2.x, p2.y, uB, vB, c),
				Vertex(p0.x, p0.y, uA, vA, c),
				Vertex(p2.x, p2.y, uB, vB, c),
				Vertex(p1.x, p1.y, uB, vA, c)
			};

			m_renderer->renderTriangles(fillPaint, s.compositeOperation, s.scissor, quad, 6, m_fringeWidth, 8 /* ShaderRect */);
			return;
		}
	}

	if (m_renderer->backendType() == GpuBackendType::OpenGL && (m_singleRRect || m_singleVaryingRRect) && det > 1e-6f && (isSolid || (m_singleRRect && (isLinearGrad || isImagePat))) && m_rrectW > 0.0f && m_rrectH > 0.0f) {
		float col0 = std::sqrt(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1]);
		float col1 = std::sqrt(s.xform[2] * s.xform[2] + s.xform[3] * s.xform[3]);
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		float fx = (fringe > 0.0f) ? (fringe * col1 / det) : 0.0f;
		float fy = (fringe > 0.0f) ? (fringe * col0 / det) : 0.0f;

		float hw = m_rrectW * 0.5f;
		float hh = m_rrectH * 0.5f;
		float radTL = m_singleVaryingRRect ? m_radTL : m_rrectR;
		float radTR = m_singleVaryingRRect ? m_radTR : m_rrectR;
		float radBR = m_singleVaryingRRect ? m_radBR : m_rrectR;
		float radBL = m_singleVaryingRRect ? m_radBL : m_rrectR;

		float xA = m_rrectX - fx;
		float xB = m_rrectX + m_rrectW + fx;
		float yA = m_rrectY - fy;
		float yB = m_rrectY + m_rrectH + fy;

		float uA = -hw - fx;
		float uB =  hw + fx;
		float vA = -hh - fy;
		float vB =  hh + fy;

		fillPaint.innerColor.a *= s.alpha;
		fillPaint.outerColor.a *= s.alpha;
		uint32_t c = fillPaint.innerColor.premultiplied().toRGBA8();

		Vertex quad[6];
		const Scissor* effScissor = &s.scissor;
		if (isAxisAligned) {
			float x0 = xA * s.xform[0] + s.xform[4];
			float y0 = yA * s.xform[3] + s.xform[5];
			float x1 = xB * s.xform[0] + s.xform[4];
			float y1 = yB * s.xform[3] + s.xform[5];
			if (!clipAxisAlignedQuadToScissor(x0, y0, x1, y1, uA, vA, uB, vB)) return;
			quad[0] = Vertex(x0, y0, uA, vA, c);
			quad[1] = Vertex(x0, y1, uA, vB, c);
			quad[2] = Vertex(x1, y1, uB, vB, c);
			quad[3] = Vertex(x0, y0, uA, vA, c);
			quad[4] = Vertex(x1, y1, uB, vB, c);
			quad[5] = Vertex(x1, y0, uB, vA, c);
			effScissor = &quadScissor;
		} else {
			Point p0 = s.xform.transformPoint(xA, yA);
			Point p1 = s.xform.transformPoint(xB, yA);
			Point p2 = s.xform.transformPoint(xB, yB);
			Point p3 = s.xform.transformPoint(xA, yB);
			quad[0] = Vertex(p0.x, p0.y, uA, vA, c);
			quad[1] = Vertex(p3.x, p3.y, uA, vB, c);
			quad[2] = Vertex(p2.x, p2.y, uB, vB, c);
			quad[3] = Vertex(p0.x, p0.y, uA, vA, c);
			quad[4] = Vertex(p2.x, p2.y, uB, vB, c);
			quad[5] = Vertex(p1.x, p1.y, uB, vA, c);
		}

		if (isSolid) {
			fillPaint.extent[0] = hw;
			fillPaint.extent[1] = hh;
			fillPaint.radius = radTL;
			fillPaint.feather = radTR;
			fillPaint.xform[0] = radBR;
			fillPaint.xform[1] = radBL;
			m_renderer->renderTriangles(fillPaint, s.compositeOperation, *effScissor, quad, 6, m_fringeWidth, 9 /* ShaderRRect */);
			return;
		} else if (isLinearGrad || isImagePat) {
			Transform2D localToScreen = Transform2D::translate(m_rrectX + hw, m_rrectY + hh).multiply(s.xform);
			Transform2D screenToLocal;
			localToScreen.inverse(screenToLocal);
			fillPaint.xform = fillPaint.xform.multiply(screenToLocal);
			if (isLinearGrad) {
				fillPaint.extent[0] = hw;
				fillPaint.radius = hh;
				fillPaint.feather = m_rrectR;
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, *effScissor, quad, 6, m_fringeWidth, 11 /* ShaderRRectGradient */);
				return;
			} else {
				float u0 = -hw - fx, v0 = -hh - fy, u1 = hw + fx, v1 = hh + fy;
				float x0 = xA * s.xform[0] + s.xform[4];
				float y0 = yA * s.xform[3] + s.xform[5];
				float x1 = xB * s.xform[0] + s.xform[4];
				float y1 = yB * s.xform[3] + s.xform[5];
				if (isAxisAligned && std::abs(s.xform[0] - 1.0f) < 1e-4f && std::abs(s.xform[3] - 1.0f) < 1e-4f &&
				    clipAxisAlignedQuadToScissor(x0, y0, x1, y1, u0, v0, u1, v1)) {
					float R = m_rrectR;
					float cx = (m_rrectX + hw) * s.xform[0] + s.xform[4];
					float cy = (m_rrectY + hh) * s.xform[3] + s.xform[5];
					// If large rounded rect with small corners (e.g. Suite 64: 880x560, r=20), split interior & 4 corners!
					if (m_rrectW > 4.0f * R && m_rrectH > 4.0f * R && u0 <= -hw && u1 >= hw && v0 <= -hh && v1 >= hh) {
						const bool flipY = (m_renderer->textureFlags(fillPaint.image) & ImageFlags::ImageFlipY) != 0;
						Transform2D uvXform = computeImageUvTransform(s.fill.xform, fillPaint.extent[0], fillPaint.extent[1], flipY);
						auto emitTexQuad = [&](Vertex* dst, float qx0, float qy0, float qx1, float qy1) {
							Point uv00 = uvXform.transformPoint(qx0, qy0);
							Point uv01 = uvXform.transformPoint(qx0, qy1);
							Point uv11 = uvXform.transformPoint(qx1, qy1);
							Point uv10 = uvXform.transformPoint(qx1, qy0);
							dst[0] = Vertex(qx0, qy0, uv00.x, uv00.y, c);
							dst[1] = Vertex(qx0, qy1, uv01.x, uv01.y, c);
							dst[2] = Vertex(qx1, qy1, uv11.x, uv11.y, c);
							dst[3] = Vertex(qx0, qy0, uv00.x, uv00.y, c);
							dst[4] = Vertex(qx1, qy1, uv11.x, uv11.y, c);
							dst[5] = Vertex(qx1, qy0, uv10.x, uv10.y, c);
						};
						float ix0 = cx - hw + R, ix1 = cx + hw - R;
						float iy0 = cy - hh,     iy1 = cy + hh;
						float ly0 = cy - hh + R, ly1 = cy + hh - R;
						Vertex bodyVerts[18];
						emitTexQuad(&bodyVerts[0],  ix0, iy0, ix1, iy1);
						emitTexQuad(&bodyVerts[6],  cx - hw, ly0, ix0, ly1);
						emitTexQuad(&bodyVerts[12], ix1, ly0, cx + hw, ly1);
						Paint bodyPaint = fillPaint;
						bodyPaint.radius = 1.0f;
						m_renderer->renderTriangles(bodyPaint, s.compositeOperation, *effScissor, bodyVerts, 18, m_fringeWidth, 3 /* ShaderImg */);

						// 4 rounded corners with ShaderFillImg (only 4 * (R x R) pixels instead of 880x560!)
						Vertex cornerVerts[24];
						auto emitCorner = [&](int idx, float cu0, float cv0, float cu1, float cv1) {
							cornerVerts[idx + 0] = Vertex(cx + cu0, cy + cv0, cu0, cv0, c);
							cornerVerts[idx + 1] = Vertex(cx + cu0, cy + cv1, cu0, cv1, c);
							cornerVerts[idx + 2] = Vertex(cx + cu1, cy + cv1, cu1, cv1, c);
							cornerVerts[idx + 3] = Vertex(cx + cu0, cy + cv0, cu0, cv0, c);
							cornerVerts[idx + 4] = Vertex(cx + cu1, cy + cv1, cu1, cv1, c);
							cornerVerts[idx + 5] = Vertex(cx + cu1, cy + cv0, cu1, cv0, c);
						};
						emitCorner(0,  -hw - fx, -hh - fy, -hw + R,  -hh + R);
						emitCorner(6,   hw - R,  -hh - fy,  hw + fx, -hh + R);
						emitCorner(12,  hw - R,   hh - R,   hw + fx,  hh + fy);
						emitCorner(18, -hw - fx,  hh - R,  -hw + R,   hh + fy);
						fillPaint.radius = m_rrectR;
						fillPaint.feather = hw;
						fillPaint.outerColor.r = hh;
						m_renderer->renderTriangles(fillPaint, s.compositeOperation, *effScissor, cornerVerts, 24, m_fringeWidth, 1 /* ShaderFillImg */);
						return;
					}
				}
				fillPaint.radius = m_rrectR;
				fillPaint.feather = hw;
				fillPaint.outerColor.r = hh;
				m_renderer->renderTriangles(fillPaint, s.compositeOperation, *effScissor, quad, 6, m_fringeWidth, 1 /* ShaderFillImg */);
				return;
			}
		}
	}

	materializePending();
	m_tessellator->flattenCommands(m_commands);

	float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
	m_tessellator->expandFill(fringe, LineJoin::Miter, 2.4f);

	fillPaint.innerColor.a *= s.alpha;
	fillPaint.outerColor.a *= s.alpha;

	const auto& paths = m_tessellator->contours();
	const auto& allVerts = m_tessellator->vertices();
	m_renderPaths.resize(paths.size());

	for (size_t i = 0; i < paths.size(); ++i) {
		const auto& tp = paths[i];
		auto& p = m_renderPaths[i];
		p.first = tp.first;
		p.count = tp.count;
		p.closed = tp.closed;
		p.nbevel = tp.nbevel;
		p.fill = (tp.fillCount > 0) ? &allVerts[tp.fillOffset] : nullptr;
		p.fillCount = tp.fillCount;
		p.stroke = (tp.strokeCount > 0) ? &allVerts[tp.strokeOffset] : nullptr;
		p.strokeCount = tp.strokeCount;
		p.winding = tp.winding;
		p.convex = tp.convex;
	}

	m_renderer->renderFill(fillPaint, s.compositeOperation, s.scissor, m_fringeWidth,
	                       m_tessellator->bounds(), m_renderPaths.data(), static_cast<int>(m_renderPaths.size()));
}

void Context::stroke() {
	State& s = currentState();
	float scale = s.xform.averageScale();
	float strokeW = std::clamp(s.strokeWidth * scale, 0.0f, 200.0f);
	Paint strokePaint = s.stroke;

	if (strokeW < m_fringeWidth) {
		float alpha = std::clamp(strokeW / m_fringeWidth, 0.0f, 1.0f);
		strokePaint.innerColor.a *= alpha * alpha;
		strokePaint.outerColor.a *= alpha * alpha;
		strokeW = m_fringeWidth;
	}

	strokePaint.innerColor.a *= s.alpha;
	strokePaint.outerColor.a *= s.alpha;

	bool isConformal = std::abs(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1] -
	                            (s.xform[2] * s.xform[2] + s.xform[3] * s.xform[3])) < 1e-4f &&
	                   std::abs(s.xform[0] * s.xform[2] + s.xform[1] * s.xform[3]) < 1e-4f &&
	                   (s.xform[0] * s.xform[3] - s.xform[1] * s.xform[2]) > 0.0f;
	bool isSolid = (strokePaint.customShader == nullptr && !strokePaint.hasAdvancedGradient() &&
	                strokePaint.image == 0 && strokePaint.extent[0] == 0.0f && strokePaint.extent[1] == 0.0f &&
	                strokePaint.radius == 0.0f &&
	                strokePaint.innerColor.r == strokePaint.outerColor.r &&
	                strokePaint.innerColor.g == strokePaint.outerColor.g &&
	                strokePaint.innerColor.b == strokePaint.outerColor.b &&
	                strokePaint.innerColor.a == strokePaint.outerColor.a);

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleCircle && isConformal && isSolid && m_circleR > 0.0f) {
		float sc = std::sqrt(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1]);
		float rScreen = m_circleR * sc;
		Point pCenter = s.xform.transformPoint(m_circleCx, m_circleCy);
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		float halfStroke = strokeW * 0.5f;
		uint32_t c = strokePaint.innerColor.premultiplied().toRGBA8();

		if (rScreen <= 40.0f || rScreen <= strokeW * 3.0f) {
			float Rquad = rScreen + halfStroke + fringe;
			strokePaint.radius = rScreen;
			strokePaint.feather = halfStroke;

			Vertex quad[6];
			quad[0] = Vertex(pCenter.x - Rquad, pCenter.y - Rquad, -Rquad, -Rquad, c);
			quad[1] = Vertex(pCenter.x - Rquad, pCenter.y + Rquad, -Rquad,  Rquad, c);
			quad[2] = Vertex(pCenter.x + Rquad, pCenter.y + Rquad,  Rquad,  Rquad, c);
			quad[3] = Vertex(pCenter.x - Rquad, pCenter.y - Rquad, -Rquad, -Rquad, c);
			quad[4] = Vertex(pCenter.x + Rquad, pCenter.y + Rquad,  Rquad,  Rquad, c);
			quad[5] = Vertex(pCenter.x + Rquad, pCenter.y - Rquad,  Rquad, -Rquad, c);

			m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, quad, 6, m_fringeWidth, 10 /* ShaderRing */);
			return;
		} else {
			// Direct precomputed LUT triangle ring for large circles (zero interior overdraw, zero sqrt/trig)
			float hw = halfStroke + fringe * 0.5f;
			float rOut = rScreen + hw;
			float rIn  = std::max(0.0f, rScreen - hw);
			m_textVertices.resize(48 * 6);
			Vertex* dst = m_textVertices.data();
			for (int i = 0; i < 48; ++i) {
				float c0 = g_circleLut.cos48[i],     s0 = g_circleLut.sin48[i];
				float c1 = g_circleLut.cos48[i + 1], s1 = g_circleLut.sin48[i + 1];
				float ox0 = pCenter.x + rOut * c0, oy0 = pCenter.y + rOut * s0;
				float ix0 = pCenter.x + rIn  * c0, iy0 = pCenter.y + rIn  * s0;
				float ox1 = pCenter.x + rOut * c1, oy1 = pCenter.y + rOut * s1;
				float ix1 = pCenter.x + rIn  * c1, iy1 = pCenter.y + rIn  * s1;
				dst[0] = Vertex(ox0, oy0, 0.0f, 1.0f, c);
				dst[1] = Vertex(ix0, iy0, 1.0f, 1.0f, c);
				dst[2] = Vertex(ox1, oy1, 0.0f, 1.0f, c);
				dst[3] = Vertex(ix0, iy0, 1.0f, 1.0f, c);
				dst[4] = Vertex(ix1, iy1, 1.0f, 1.0f, c);
				dst[5] = Vertex(ox1, oy1, 0.0f, 1.0f, c);
				dst += 6;
			}
			strokePaint.feather = strokeW;
			m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, m_textVertices.data(), 48 * 6, m_fringeWidth, -2 /* Fast-path Stroked Solid */);
			return;
		}
	}

	bool isAxisAligned = (std::abs(s.xform[1]) < 1e-4f && std::abs(s.xform[2]) < 1e-4f &&
	                      s.xform[0] > 0.0f && s.xform[3] > 0.0f);

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleEllipse && isAxisAligned && isSolid && m_ellipseRx > 0.0f && m_ellipseRy > 0.0f) {
		float sx = s.xform[0];
		float sy = s.xform[3];
		float cx = m_ellipseCx * sx + s.xform[4];
		float cy = m_ellipseCy * sy + s.xform[5];
		float rx = m_ellipseRx * sx;
		float ry = m_ellipseRy * sy;
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		float halfStroke = strokeW * 0.5f;
		float Rqx = rx + halfStroke + fringe;
		float Rqy = ry + halfStroke + fringe;
		float scaleX = Rqx / rx;
		float scaleY = Rqy / ry;
		uint32_t c = strokePaint.innerColor.premultiplied().toRGBA8();

		strokePaint.extent[0] = -rx; // Negative extent[0] signals Stroked Ellipse in ShaderRing!
		strokePaint.extent[1] = ry;
		strokePaint.radius = 1.0f;
		strokePaint.feather = halfStroke;

		Vertex quad[6] = {
			Vertex(cx - Rqx, cy - Rqy, -scaleX, -scaleY, c),
			Vertex(cx - Rqx, cy + Rqy, -scaleX,  scaleY, c),
			Vertex(cx + Rqx, cy + Rqy,  scaleX,  scaleY, c),
			Vertex(cx - Rqx, cy - Rqy, -scaleX, -scaleY, c),
			Vertex(cx + Rqx, cy + Rqy,  scaleX,  scaleY, c),
			Vertex(cx + Rqx, cy - Rqy,  scaleX, -scaleY, c)
		};
		m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, quad, 6, m_fringeWidth, 10 /* ShaderRing */);
		return;
	}

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleRect && isAxisAligned && isSolid && s.lineJoin == LineJoin::Miter && m_rectW > 0.0f && m_rectH > 0.0f) {
		float sx = s.xform[0];
		float sy = s.xform[3];
		float x0 = m_rectX * sx + s.xform[4];
		float y0 = m_rectY * sy + s.xform[5];
		float x1 = (m_rectX + m_rectW) * sx + s.xform[4];
		float y1 = (m_rectY + m_rectH) * sy + s.xform[5];
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		float hw = strokeW * 0.5f + fringe * 0.5f;
		if ((x1 - x0) > 2.0f * hw && (y1 - y0) > 2.0f * hw) {
			uint32_t c = strokePaint.innerColor.premultiplied().toRGBA8();
			float ox[4] = {x0 - hw, x0 - hw, x1 + hw, x1 + hw};
			float oy[4] = {y0 - hw, y1 + hw, y1 + hw, y0 - hw};
			float ix[4] = {x0 + hw, x0 + hw, x1 - hw, x1 - hw};
			float iy[4] = {y0 + hw, y1 - hw, y1 - hw, y0 + hw};
			Vertex verts[24];
			for (int i = 0; i < 4; ++i) {
				int j = (i + 1) & 3;
				verts[i * 6 + 0] = Vertex(ox[i], oy[i], 0.0f, 1.0f, c);
				verts[i * 6 + 1] = Vertex(ix[i], iy[i], 1.0f, 1.0f, c);
				verts[i * 6 + 2] = Vertex(ox[j], oy[j], 0.0f, 1.0f, c);
				verts[i * 6 + 3] = Vertex(ix[i], iy[i], 1.0f, 1.0f, c);
				verts[i * 6 + 4] = Vertex(ix[j], iy[j], 1.0f, 1.0f, c);
				verts[i * 6 + 5] = Vertex(ox[j], oy[j], 0.0f, 1.0f, c);
			}
			strokePaint.feather = strokeW;
			m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, verts, 24, m_fringeWidth, -2 /* Fast-path Stroked Solid */);
			return;
		}
	}

	if (m_renderer->backendType() == GpuBackendType::OpenGL && m_singleRRect && isConformal && isSolid && m_rrectW > 0.0f && m_rrectH > 0.0f) {
		float sc = std::sqrt(s.xform[0] * s.xform[0] + s.xform[1] * s.xform[1]);
		if (sc < 1e-6f) sc = 1.0f;
		float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
		float halfStroke = strokeW * 0.5f;
		float expand = (halfStroke + fringe) / sc;

		float hw = m_rrectW * 0.5f;
		float hh = m_rrectH * 0.5f;
		float cx = m_rrectX + hw;
		float cy = m_rrectY + hh;

		float uOutX = hw + expand;
		float uOutY = hh + expand;
		float uInX = hw - expand - m_rrectR * 0.3f;
		float uInY = hh - expand - m_rrectR * 0.3f;

		uint32_t c = strokePaint.innerColor.premultiplied().toRGBA8();
		strokePaint.extent[0] = hw * sc;
		strokePaint.extent[1] = hh * sc;
		strokePaint.radius = m_rrectR * sc;
		strokePaint.feather = halfStroke;

		if (uInX > 4.0f && uInY > 4.0f) {
			Vertex quads[24];
			auto emitQuad = [&](int idx, float u0, float v0, float u1, float v1) {
				Point p0 = s.xform.transformPoint(cx + u0, cy + v0);
				Point p1 = s.xform.transformPoint(cx + u1, cy + v0);
				Point p2 = s.xform.transformPoint(cx + u1, cy + v1);
				Point p3 = s.xform.transformPoint(cx + u0, cy + v1);
				float su0 = u0 * sc, sv0 = v0 * sc, su1 = u1 * sc, sv1 = v1 * sc;
				quads[idx + 0] = Vertex(p0.x, p0.y, su0, sv0, c);
				quads[idx + 1] = Vertex(p3.x, p3.y, su0, sv1, c);
				quads[idx + 2] = Vertex(p2.x, p2.y, su1, sv1, c);
				quads[idx + 3] = Vertex(p0.x, p0.y, su0, sv0, c);
				quads[idx + 4] = Vertex(p2.x, p2.y, su1, sv1, c);
				quads[idx + 5] = Vertex(p1.x, p1.y, su1, sv0, c);
			};
			emitQuad(0,  -uOutX, -uOutY,  uOutX, -uInY);
			emitQuad(6,  -uOutX,  uInY,   uOutX,  uOutY);
			emitQuad(12, -uOutX, -uInY,  -uInX,   uInY);
			emitQuad(18,  uInX,  -uInY,   uOutX,  uInY);
			m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, quads, 24, m_fringeWidth, 10 /* ShaderRing */);
			return;
		} else {
			Point p0 = s.xform.transformPoint(cx - uOutX, cy - uOutY);
			Point p1 = s.xform.transformPoint(cx + uOutX, cy - uOutY);
			Point p2 = s.xform.transformPoint(cx + uOutX, cy + uOutY);
			Point p3 = s.xform.transformPoint(cx - uOutX, cy + uOutY);
			float suOutX = uOutX * sc, suOutY = uOutY * sc;
			Vertex quad[6];
			quad[0] = Vertex(p0.x, p0.y, -suOutX, -suOutY, c);
			quad[1] = Vertex(p3.x, p3.y, -suOutX,  suOutY, c);
			quad[2] = Vertex(p2.x, p2.y,  suOutX,  suOutY, c);
			quad[3] = Vertex(p0.x, p0.y, -suOutX, -suOutY, c);
			quad[4] = Vertex(p2.x, p2.y,  suOutX,  suOutY, c);
			quad[5] = Vertex(p1.x, p1.y,  suOutX, -suOutY, c);
			m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, quad, 6, m_fringeWidth, 10 /* ShaderRing */);
			return;
		}
	}

	// Fast-path for disjoint line segments (MoveTo + LineTo pairs, Butt/Square cap): bypass Tessellator
	if (m_renderer->backendType() == GpuBackendType::OpenGL && isSolid && !m_commands.empty() &&
	    (m_commands.size() % 2 == 0) && (s.lineCap == LineCap::Butt || s.lineCap == LineCap::Square)) {
		bool allSegments = true;
		int numSegs = static_cast<int>(m_commands.size() / 2);
		for (int i = 0; i < numSegs; ++i) {
			if (m_commands[i * 2 + 0].type != PathCommandType::MoveTo ||
			    m_commands[i * 2 + 1].type != PathCommandType::LineTo) {
				allSegments = false;
				break;
			}
		}
		if (allSegments) {
			float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
			float hw = strokeW * 0.5f + fringe * 0.5f;
			float capExt = (s.lineCap == LineCap::Square) ? (strokeW * 0.5f) : 0.0f;
			uint32_t c = strokePaint.innerColor.premultiplied().toRGBA8();
			m_textVertices.resize(numSegs * 6);
			Vertex* dst = m_textVertices.data();
			int validVerts = 0;
			for (int i = 0; i < numSegs; ++i) {
				float x0 = m_commands[i * 2 + 0].p0.x, y0 = m_commands[i * 2 + 0].p0.y;
				float x1 = m_commands[i * 2 + 1].p0.x, y1 = m_commands[i * 2 + 1].p0.y;
				float dx = x1 - x0, dy = y1 - y0;
				float len2 = dx * dx + dy * dy;
				if (len2 < 1e-8f) continue;
				float invLen = 1.0f / std::sqrt(len2);
				float ux = dx * invLen, uy = dy * invLen;
				float nx = -uy * hw, ny = ux * hw;
				if (capExt > 0.0f) {
					x0 -= ux * capExt; y0 -= uy * capExt;
					x1 += ux * capExt; y1 += uy * capExt;
				}
				dst[0] = Vertex(x0 + nx, y0 + ny, 0.0f, 1.0f, c);
				dst[1] = Vertex(x0 - nx, y0 - ny, 1.0f, 1.0f, c);
				dst[2] = Vertex(x1 + nx, y1 + ny, 0.0f, 1.0f, c);
				dst[3] = Vertex(x0 - nx, y0 - ny, 1.0f, 1.0f, c);
				dst[4] = Vertex(x1 - nx, y1 - ny, 1.0f, 1.0f, c);
				dst[5] = Vertex(x1 + nx, y1 + ny, 0.0f, 1.0f, c);
				dst += 6;
				validVerts += 6;
			}
			if (validVerts > 0) {
				strokePaint.feather = strokeW;
				m_renderer->renderTriangles(strokePaint, s.compositeOperation, s.scissor, m_textVertices.data(), validVerts, m_fringeWidth, -2 /* Fast-path Stroked Solid */);
			}
			return;
		}
	}

	materializePending();

	m_tessellator->flattenCommands(m_commands);

	float fringe = (m_renderer->edgeAntiAlias() && s.shapeAntiAlias) ? m_fringeWidth : 0.0f;
	m_tessellator->expandStroke(strokeW * 0.5f, fringe, s.lineCap, s.lineJoin, s.miterLimit);

	const auto& paths = m_tessellator->contours();
	const auto& allVerts = m_tessellator->vertices();
	m_renderPaths.resize(paths.size());

	for (size_t i = 0; i < paths.size(); ++i) {
		const auto& tp = paths[i];
		auto& p = m_renderPaths[i];
		p.first = tp.first;
		p.count = tp.count;
		p.closed = tp.closed;
		p.nbevel = tp.nbevel;
		p.fill = (tp.fillCount > 0) ? &allVerts[tp.fillOffset] : nullptr;
		p.fillCount = tp.fillCount;
		p.stroke = (tp.strokeCount > 0) ? &allVerts[tp.strokeOffset] : nullptr;
		p.strokeCount = tp.strokeCount;
		p.winding = tp.winding;
		p.convex = tp.convex;
	}

	m_renderer->renderStroke(strokePaint, s.compositeOperation, s.scissor, m_fringeWidth,
	                         strokeW, m_renderPaths.data(), static_cast<int>(m_renderPaths.size()));
}

void Context::drawTriangles(const Paint& paint, const Vertex* verts, int nverts, float fringe, int shaderType) {
	if (!m_renderer || !verts || nverts <= 0) return;

	const State& s = currentState();
	Paint p = paint;
	const bool usesVertexColorOnly = (shaderType == 4 || shaderType == -2 || shaderType == 7 || shaderType == 8 || shaderType == 9 || shaderType == 18);
	if (!usesVertexColorOnly) {
		p.innerColor.a *= s.alpha;
		p.outerColor.a *= s.alpha;
	}

	const bool identity = s.xform.isIdentity();
	const bool unitAlpha = s.alpha >= 0.9999f;
	if (identity && unitAlpha) {
		m_renderer->renderTriangles(p, s.compositeOperation, s.scissor, verts, nverts, fringe, shaderType);
		return;
	}

	auto transformVertices = [&](Vertex* dst) {
		const bool scaleVerts = usesVertexColorOnly && !unitAlpha;
		const float alpha = std::clamp(s.alpha, 0.0f, 1.0f);
		for (int i = 0; i < nverts; ++i) {
			Vertex v = verts[i];
			if (!identity) {
				Point pt = s.xform.transformPoint(v.x, v.y);
				v.x = pt.x;
				v.y = pt.y;
			}
			if (scaleVerts) {
				uint32_t c = v.color;
				uint32_t r = static_cast<uint32_t>(static_cast<float>(c & 0xffu) * alpha + 0.5f);
				uint32_t g = static_cast<uint32_t>(static_cast<float>((c >> 8) & 0xffu) * alpha + 0.5f);
				uint32_t b = static_cast<uint32_t>(static_cast<float>((c >> 16) & 0xffu) * alpha + 0.5f);
				uint32_t a = static_cast<uint32_t>(static_cast<float>((c >> 24) & 0xffu) * alpha + 0.5f);
				v.color = (a << 24) | (b << 16) | (g << 8) | r;
			}
			dst[i] = v;
		}
	};

	if (nverts <= 64) {
		std::array<Vertex, 64> stackVerts{};
		transformVertices(stackVerts.data());
		m_renderer->renderTriangles(p, s.compositeOperation, s.scissor, stackVerts.data(), nverts, fringe, shaderType);
	} else {
		m_textVertices.resize(static_cast<size_t>(nverts));
		transformVertices(m_textVertices.data());
		m_renderer->renderTriangles(p, s.compositeOperation, s.scissor, m_textVertices.data(), nverts, fringe, shaderType);
	}
}

void Context::rrectShadow(float x, float y, float w, float h, float rTL, float rTR, float rBR, float rBL, float sigma, Color color, const float* occludedBox) {
	if (!m_renderer || w <= 0.0f || h <= 0.0f || color.a <= 0.0f) return;
	const float sig = std::max(0.0f, sigma);
	const float pad = (sig > 0.01f) ? (std::ceil(sig * 3.0f) + 2.0f) : 1.5f;
	const float hw = w * 0.5f;
	const float hh = h * 0.5f;
	const float cx = x + hw;
	const float cy = y + hh;

	const float u0 = -hw - pad;
	const float v0 = -hh - pad;
	const float u1 =  hw + pad;
	const float v1 =  hh + pad;

	// Pre-normalize corner radii on CPU once per shadow (avoids 4 branches + 4 divisions per pixel in GLSL!)
	float cTL = std::max(0.0f, rTL), cTR = std::max(0.0f, rTR);
	float cBR = std::max(0.0f, rBR), cBL = std::max(0.0f, rBL);
	float rScale = 1.0f;
	if (cTL + cTR > w) rScale = std::min(rScale, w / (cTL + cTR));
	if (cBL + cBR > w) rScale = std::min(rScale, w / (cBL + cBR));
	if (cTL + cBL > h) rScale = std::min(rScale, h / (cTL + cBL));
	if (cTR + cBR > h) rScale = std::min(rScale, h / (cTR + cBR));
	cTL *= rScale; cTR *= rScale; cBR *= rScale; cBL *= rScale;

	Paint sp = Paint::color(color);
	sp.extent[0] = hw;
	sp.extent[1] = hh;
	sp.radius = cTL;
	sp.feather = cTR;
	sp.xform[0] = cBR;
	sp.xform[1] = cBL;
	sp.gradParams0[0] = sig;

	const uint32_t c = color.premultiplied().toRGBA8();

	auto emitStrip = [&](Vertex* dst, float lu0, float lv0, float lu1, float lv1) {
		const float px0 = cx + lu0, py0 = cy + lv0;
		const float px1 = cx + lu1, py1 = cy + lv1;
		dst[0] = Vertex(px0, py0, lu0, lv0, c);
		dst[1] = Vertex(px0, py1, lu0, lv1, c);
		dst[2] = Vertex(px1, py1, lu1, lv1, c);
		dst[3] = Vertex(px0, py0, lu0, lv0, c);
		dst[4] = Vertex(px1, py1, lu1, lv1, c);
		dst[5] = Vertex(px1, py0, lu1, lv0, c);
	};

	// Fast opaque-card cutout: skip shading pixels strictly occluded by the opaque foreground card!
	if (occludedBox != nullptr) {
		const float oL = std::clamp(occludedBox[0] - cx, u0, u1);
		const float oT = std::clamp(occludedBox[1] - cy, v0, v1);
		const float oR = std::clamp(occludedBox[2] - cx, u0, u1);
		const float oB = std::clamp(occludedBox[3] - cy, v0, v1);
		if (oR > oL + 2.0f && oB > oT + 2.0f) {
			Vertex strips[24];
			int nv = 0;
			if (oT > v0) { emitStrip(strips + nv, u0, v0, u1, oT); nv += 6; }
			if (v1 > oB) { emitStrip(strips + nv, u0, oB, u1, v1); nv += 6; }
			if (oL > u0) { emitStrip(strips + nv, u0, oT, oL, oB); nv += 6; }
			if (u1 > oR) { emitStrip(strips + nv, oR, oT, u1, oB); nv += 6; }
			if (nv > 0) {
				drawTriangles(sp, strips, nv, m_fringeWidth, 16);
			}
			return;
		}
	}

	const float margin = (sig >= 0.1f) ? (3.0f * sig) : 1.5f;
	const float inL = -hw + std::max(cTL, cBL) + margin;
	const float inR =  hw - std::max(cTR, cBR) - margin;
	const float inT = -hh + std::max(cTL, cTR) + margin;
	const float inB =  hh - std::max(cBL, cBR) - margin;

	if (inR > inL + 2.0f && inB > inT + 2.0f) {
		const Vertex coreQuad[6] = {
			Vertex(cx + inL, cy + inT, 0.5f, 1.0f, c),
			Vertex(cx + inL, cy + inB, 0.5f, 1.0f, c),
			Vertex(cx + inR, cy + inB, 0.5f, 1.0f, c),
			Vertex(cx + inL, cy + inT, 0.5f, 1.0f, c),
			Vertex(cx + inR, cy + inB, 0.5f, 1.0f, c),
			Vertex(cx + inR, cy + inT, 0.5f, 1.0f, c)
		};
		drawTriangles(Paint::color(color), coreQuad, 6, m_fringeWidth, 4 /* ShaderSolid */);

		Vertex strips[24];
		emitStrip(strips + 0,  u0,  v0,  u1,  inT);
		emitStrip(strips + 6,  u0,  inB, u1,  v1);
		emitStrip(strips + 12, u0,  inT, inL, inB);
		emitStrip(strips + 18, inR, inT, u1,  inB);
		drawTriangles(sp, strips, 24, m_fringeWidth, 16);
		return;
	}

	const Vertex quad[6] = {
		Vertex(cx + u0, cy + v0, u0, v0, c),
		Vertex(cx + u0, cy + v1, u0, v1, c),
		Vertex(cx + u1, cy + v1, u1, v1, c),
		Vertex(cx + u0, cy + v0, u0, v0, c),
		Vertex(cx + u1, cy + v1, u1, v1, c),
		Vertex(cx + u1, cy + v0, u1, v0, c)
	};
	drawTriangles(sp, quad, 6, m_fringeWidth, 16);
}

// -------------------------------------------------------------
// Typography & Text (Sovereign Nisaba C++20 Engine)
// -------------------------------------------------------------
int Context::createFont(const char* name, const char* filename) {
	return createFontAtIndex(name, filename, 0);
}

int Context::createFontAtIndex(const char* name, const char* filename, const int fontIndex) {
	if (!filename) return -1;
	const uint32_t idx = (fontIndex > 0) ? static_cast<uint32_t>(fontIndex) : 0u;
	const std::string_view alias = (name != nullptr) ? std::string_view(name) : std::string_view{};
	auto idOpt = m_fontSystem.load_font_file(filename, idx, alias);
	if (!idOpt) return -1;
	int id = static_cast<int>(*idOpt);
	if (name) {
		m_fontNames[name] = id;
	}
	if (currentState().fontId <= 0) {
		currentState().fontId = id;
	}
	return id;
}

int Context::createFontMem(const char* name, unsigned char* data, int ndata, int freeData) {
	return createFontMemAtIndex(name, data, ndata, freeData, 0);
}

int Context::createFontMemAtIndex(const char* name, unsigned char* data, int ndata, int freeData, const int fontIndex) {
	if (!data || ndata <= 0) return -1;
	const uint32_t idx = (fontIndex > 0) ? static_cast<uint32_t>(fontIndex) : 0u;
	const std::string_view alias = (name != nullptr) ? std::string_view(name) : std::string_view{};
	auto idOpt = m_fontSystem.load_font_data(std::span<const uint8_t>(data, static_cast<size_t>(ndata)), idx, alias);
	if (freeData && data) {
		free(data);
	}
	if (!idOpt) return -1;
	int id = static_cast<int>(*idOpt);
	if (name) {
		m_fontNames[name] = id;
	}
	if (currentState().fontId <= 0) {
		currentState().fontId = id;
	}
	return id;
}

int Context::findFont(const char* name) {
	if (!name) return -1;
	auto it = m_fontNames.find(name);
	if (it != m_fontNames.end()) return it->second;
	return -1;
}

int Context::addFallbackFontId(int baseFont, int fallbackFont) {
	if (baseFont <= 0 || fallbackFont <= 0) return 0;
	m_fallbacks[baseFont].push_back(fallbackFont);
	return 1;
}

int Context::addFallbackFont(const char* baseFont, const char* fallbackFont) {
	return addFallbackFontId(findFont(baseFont), findFont(fallbackFont));
}

void Context::resetFallbackFontsId(int baseFont) {
	m_fallbacks.erase(baseFont);
}

void Context::resetFallbackFonts(const char* baseFont) {
	resetFallbackFontsId(findFont(baseFont));
}

void Context::fontSize(float size) {
	currentState().fontSize = size;
}

void Context::fontBlur(float blur) {
	currentState().fontBlur = blur;
}

void Context::textLetterSpacing(float spacing) {
	currentState().letterSpacing = spacing;
}

void Context::textLineHeight(float lineHeight) {
	currentState().lineHeight = lineHeight;
}

void Context::textAlign(Align align) {
	currentState().textAlign = align;
}

void Context::fontFaceId(int font) {
	currentState().fontId = font;
}

void Context::fontFace(const char* font) {
	currentState().fontId = findFont(font);
}

static float getFontScale(const State& state) {
	return std::min(quantize(state.xform.averageScale(), 0.01f), 4.0f);
}

const text::TtfFont* Context::resolveGlyphFont(int primaryFontId, char32_t cp, uint16_t* outGlyphId) const {
	if (primaryFontId > 0) {
		const text::TtfFont* font = m_fontSystem.get_font(static_cast<uint32_t>(primaryFontId));
		if (font) {
			uint16_t gid = font->glyph_index(cp);
			if (gid != 0) {
				*outGlyphId = gid;
				return font;
			}
		}
		auto it = m_fallbacks.find(primaryFontId);
		if (it != m_fallbacks.end()) {
			for (int fbId : it->second) {
				const text::TtfFont* fbFont = m_fontSystem.get_font(static_cast<uint32_t>(fbId));
				if (fbFont) {
					uint16_t gid = fbFont->glyph_index(cp);
					if (gid != 0) {
						*outGlyphId = gid;
						return fbFont;
					}
				}
			}
		}
	}

	auto [fbId, gid] = m_fontSystem.find_glyph_or_fallback(static_cast<uint32_t>(std::max(1, primaryFontId)), cp);
	if (gid != 0) {
		*outGlyphId = gid;
		return m_fontSystem.get_font(fbId);
	}

	const text::TtfFont* primary = m_fontSystem.get_font(static_cast<uint32_t>(primaryFontId));
	*outGlyphId = 0;
	return primary;
}

static inline bool isStringPureAscii(const char* str, const char* end) noexcept {
	for (const char* p = str; p < end; ++p) {
		if (static_cast<unsigned char>(*p) >= 128) return false;
	}
	return true;
}

const std::vector<Context::MeasuredGlyph>& Context::layoutGlyphsInternal(
	const State& s,
	const text::TtfFont* primaryFont,
	uint32_t fontId,
	float scaledSize,
	float fontSize,
	const char* string,
	const char* end,
	float* outTotalWidth) const {

	m_measuredGlyphs.clear();
	if (!string || string == end || !primaryFont) {
		if (outTotalWidth) *outTotalWidth = 0.0f;
		return m_measuredGlyphs;
	}

	float scale = primaryFont->scale_for_size(scaledSize);
	float curX = 0.0f;
	uint16_t prevGid = 0;
	float letterSpace = s.letterSpacing * (fontSize > 0.0f ? (scaledSize / fontSize) : 1.0f);

	if (isStringPureAscii(string, end)) {
		m_measuredGlyphs.reserve(end - string);
		for (const char* p = string; p < end; ++p) {
			uint8_t c = static_cast<uint8_t>(*p);
			uint16_t gid = primaryFont->ascii_glyph_index(c);
			const text::TtfFont* font = primaryFont;
			uint32_t fid = fontId;

			if (gid == 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
				font = resolveGlyphFont(static_cast<int>(fontId), c, &gid);
				if (!font) font = primaryFont;
				fid = (font == primaryFont) ? fontId : 0;
			}

			float adv = (font == primaryFont) ?
				(static_cast<float>(primaryFont->ascii_advance(c)) * scale) :
				font->glyph_advance(gid, scaledSize);

			if (prevGid != 0 && gid != 0 && font == primaryFont) {
				int16_t kern = primaryFont->get_kerning(prevGid, gid);
				curX += static_cast<float>(kern) * scale;
			}
			prevGid = (font == primaryFont) ? gid : 0;

			MeasuredGlyph mg;
			mg.font = font;
			mg.fontId = fid;
			mg.glyphId = gid;
			mg.x = curX;
			mg.y = 0.0f;
			mg.advance = adv;
			mg.scale = scale;
			mg.str = p;
			m_measuredGlyphs.push_back(mg);

			curX += adv + letterSpace;
		}
	} else {
		std::string_view sv(string, end - string);
		auto bidiRuns = text::Bidi::segment_runs(sv);
		auto shaped = text::Bidi::shape_text(sv, bidiRuns);

		m_measuredGlyphs.reserve(shaped.size());
		for (const auto& sc : shaped) {
			uint16_t gid = 0;
			const text::TtfFont* font = resolveGlyphFont(static_cast<int>(fontId), sc.codepoint, &gid);
			if (!font) {
				gid = primaryFont->glyph_index(sc.codepoint);
				font = primaryFont;
			}

			float fScale = font->scale_for_size(scaledSize);
			float adv = font->glyph_advance_scaled(gid, fScale);

			if (prevGid != 0 && gid != 0) {
				int16_t kern = font->get_kerning(prevGid, gid);
				curX += static_cast<float>(kern) * fScale;
			}
			prevGid = gid;

			MeasuredGlyph mg;
			mg.font = font;
			mg.fontId = (font == primaryFont) ? fontId : 0;
			mg.glyphId = gid;
			mg.x = curX;
			mg.y = 0.0f;
			mg.advance = adv;
			mg.scale = fScale;
			mg.str = string + sc.byte_index;
			m_measuredGlyphs.push_back(mg);

			curX += adv + letterSpace;
		}
	}

	if (outTotalWidth) *outTotalWidth = curX;
	return m_measuredGlyphs;
}

const std::vector<Context::MeasuredGlyph>& Context::layoutGlyphs(
	const State& s,
	float scaledSize,
	const char* string,
	const char* end,
	float* outTotalWidth) const {

	if (s.fontId <= 0) {
		m_measuredGlyphs.clear();
		if (outTotalWidth) *outTotalWidth = 0.0f;
		return m_measuredGlyphs;
	}
	const text::TtfFont* primaryFont = m_fontSystem.get_font(static_cast<uint32_t>(s.fontId));
	return layoutGlyphsInternal(s, primaryFont, static_cast<uint32_t>(s.fontId), scaledSize, s.fontSize, string, end, outTotalWidth);
}

static void computeAlignmentOffsets(const State& s, const text::TtfFont* font, float scaledSize, float totalWidth, float& outAlignX, float& outAlignY) {
	outAlignX = 0.0f;
	if (hasFlag(s.textAlign, Align::Center)) {
		outAlignX = -totalWidth * 0.5f;
	} else if (hasFlag(s.textAlign, Align::Right)) {
		outAlignX = -totalWidth;
	}

	outAlignY = 0.0f;
	if (font) {
		float fontAscent = font->ascent(scaledSize);
		float fontDescent = font->descent(scaledSize);
		if (hasFlag(s.textAlign, Align::Top)) {
			outAlignY = fontAscent;
		} else if (hasFlag(s.textAlign, Align::Middle)) {
			outAlignY = (fontAscent + fontDescent) * 0.5f;
		} else if (hasFlag(s.textAlign, Align::Bottom)) {
			outAlignY = fontDescent;
		} else if (hasFlag(s.textAlign, Align::Baseline)) {
			outAlignY = 0.0f;
		}
	}
}

float Context::text(float x, float y, const char* string, const char* end) {
	State& s = currentState();
	if (s.fontId <= 0) {
		int defFont = findFont("sans");
		if (defFont <= 0 && !m_fontNames.empty()) defFont = m_fontNames.begin()->second;
		if (defFont > 0) s.fontId = defFont;
	}
	if (s.fontId <= 0 || !string) return x;
	if (!end) end = string + std::strlen(string);
	if (string == end) return x;

	float scale = getFontScale(s) * m_devicePixelRatio;
	float invscale = 1.0f / scale;
	float scaledSize = s.fontSize * scale;

	const text::TtfFont* primaryFont = m_fontSystem.get_font(static_cast<uint32_t>(s.fontId));
	if (!primaryFont) return x;

	float totalWidth = 0.0f;
	const auto& glyphs = layoutGlyphs(s, scaledSize, string, end, &totalWidth);
	if (glyphs.empty()) return x;

	float alignX = 0.0f, alignY = 0.0f;
	computeAlignmentOffsets(s, primaryFont, scaledSize, totalWidth, alignX, alignY);

	float startX = x * scale + alignX;
	float baselineY = y * scale + alignY;
	bool isFlipped = isTransformFlipped(s.xform);
	bool isIdentity = s.xform.isIdentity() && (invscale == 1.0f);

	Color textCol = s.fill.innerColor;
	textCol.a *= s.alpha;
	uint32_t vcol = textCol.premultiplied().toRGBA8();

	m_textVertices.clear();
	m_textVertices.reserve(glyphs.size() * 6);

	uint32_t sizeBits = 0;
	std::memcpy(&sizeBits, &scaledSize, sizeof(float));

	for (const auto& g : glyphs) {
		if (g.glyphId == 0 || !g.font) continue;

		float targetX = startX + g.x;
		float targetY = baselineY + g.y;

		auto [intX, xBin] = text::compute_subpixel_bin(targetX);
		auto [intY, yBin] = text::compute_subpixel_bin(targetY);

		text::CacheKey key{g.fontId, g.glyphId, sizeBits, xBin, yBin};
		const AtlasGlyphEntry* entry = m_atlas->find(key);

		if (!entry) {
			const text::CachedGlyph* cg = m_glyphCache.get_or_render(
				*g.font, g.fontId, g.glyphId, scaledSize, targetX, targetY);

			auto inserted = m_atlas->insert(key, cg);
			if (!inserted) {
				if (!m_textVertices.empty()) {
					m_atlas->flushToGpu(m_renderer.get(), m_fontTextureId);
					Paint p = s.fill;
					p.image = m_fontTextureId;
					p.radius = 1.0f;
					m_renderer->renderTriangles(p, s.compositeOperation, s.scissor,
					                           m_textVertices.data(), static_cast<int>(m_textVertices.size()), m_fringeWidth, 3);
					m_textVertices.clear();
				}
				if (m_renderer) {
					m_renderer->flush();
				}
				if (m_atlas->width() < 2048 || m_atlas->height() < 2048) {
					if (m_fontTextureId != 0 && m_renderer) {
						m_renderer->deleteTexture(m_fontTextureId);
						m_fontTextureId = 0;
					}
					m_atlas->resize(2048, 2048);
					m_glyphCache.clear();
					m_fontTextureId = m_renderer->createTexture(TextureType::Alpha, m_atlas->width(), m_atlas->height(), 0, m_atlas->data());
					m_atlas->markClean();
					inserted = m_atlas->insert(key, cg);
				} else {
					m_atlas->reset();
					m_glyphCache.clear();
					inserted = m_atlas->insert(key, cg);
				}
			}
			entry = inserted ? &(*inserted) : nullptr;
		}

		if (!entry || entry->width == 0 || entry->height == 0) continue;

		float u0 = entry->u0;
		float v0 = entry->v0;
		float u1 = entry->u1;
		float v1 = entry->v1;

		if (isIdentity) {
			float qx0 = static_cast<float>(intX + entry->offsetX);
			float qy0 = static_cast<float>(intY + entry->offsetY);
			float qx1 = qx0 + static_cast<float>(entry->width);
			float qy1 = qy0 + static_cast<float>(entry->height);

			m_textVertices.emplace_back(qx0, qy0, u0, v0, vcol);
			m_textVertices.emplace_back(qx1, qy1, u1, v1, vcol);
			m_textVertices.emplace_back(qx1, qy0, u1, v0, vcol);

			m_textVertices.emplace_back(qx0, qy0, u0, v0, vcol);
			m_textVertices.emplace_back(qx0, qy1, u0, v1, vcol);
			m_textVertices.emplace_back(qx1, qy1, u1, v1, vcol);
		} else {
			float qx0 = static_cast<float>(intX + entry->offsetX) * invscale;
			float qy0 = static_cast<float>(intY + entry->offsetY) * invscale;
			float qx1 = qx0 + static_cast<float>(entry->width) * invscale;
			float qy1 = qy0 + static_cast<float>(entry->height) * invscale;

			if (isFlipped) {
				std::swap(qy0, qy1);
				std::swap(v0, v1);
			}

			Point c0 = s.xform.transformPoint(qx0, qy0);
			Point c1 = s.xform.transformPoint(qx1, qy0);
			Point c2 = s.xform.transformPoint(qx1, qy1);
			Point c3 = s.xform.transformPoint(qx0, qy1);

			m_textVertices.emplace_back(c0.x, c0.y, u0, v0, vcol);
			m_textVertices.emplace_back(c2.x, c2.y, u1, v1, vcol);
			m_textVertices.emplace_back(c1.x, c1.y, u1, v0, vcol);

			m_textVertices.emplace_back(c0.x, c0.y, u0, v0, vcol);
			m_textVertices.emplace_back(c3.x, c3.y, u0, v1, vcol);
			m_textVertices.emplace_back(c2.x, c2.y, u1, v1, vcol);
		}
	}

	if (m_atlas->isDirty()) {
		m_atlas->flushToGpu(m_renderer.get(), m_fontTextureId);
	}

	if (!m_textVertices.empty()) {
		Paint p = s.fill;
		p.image = m_fontTextureId;
		p.radius = 1.0f;
		m_renderer->renderTriangles(p, s.compositeOperation, s.scissor,
		                           m_textVertices.data(), static_cast<int>(m_textVertices.size()), m_fringeWidth, 3);
	}

	return x + totalWidth * invscale;
}

float Context::textWithFont(float x, float y, const text::TtfFont& font, float fontSize, const char* string, const char* end) {
	State& s = currentState();
	if (!string) return x;
	if (!end) end = string + std::strlen(string);
	if (string == end) return x;

	float scale = getFontScale(s) * m_devicePixelRatio;
	float invscale = 1.0f / scale;
	float scaledSize = fontSize * scale;

	float totalWidth = 0.0f;
	uint32_t fontId = (static_cast<uint32_t>(std::hash<const text::TtfFont*>{}(&font)) & 0x7FFFFFFFu) | 0x80000000u;
	const auto& glyphs = layoutGlyphsInternal(s, &font, fontId, scaledSize, fontSize, string, end, &totalWidth);
	if (glyphs.empty()) return x;

	float alignX = 0.0f, alignY = 0.0f;
	computeAlignmentOffsets(s, &font, scaledSize, totalWidth, alignX, alignY);

	float startX = x * scale + alignX;
	float baselineY = y * scale + alignY;
	bool isFlipped = isTransformFlipped(s.xform);

	Color textCol = s.fill.innerColor;
	textCol.a *= s.alpha;
	uint32_t vcol = textCol.premultiplied().toRGBA8();

	m_textVertices.clear();
	m_textVertices.reserve(glyphs.size() * 6);

	uint32_t sizeBits = 0;
	std::memcpy(&sizeBits, &scaledSize, sizeof(float));

	for (const auto& g : glyphs) {
		if (g.glyphId == 0 || !g.font) continue;

		float targetX = startX + g.x;
		float targetY = baselineY + g.y;

		auto [intX, xBin] = text::compute_subpixel_bin(targetX);
		auto [intY, yBin] = text::compute_subpixel_bin(targetY);

		text::CacheKey key{g.fontId, g.glyphId, sizeBits, xBin, yBin};
		const AtlasGlyphEntry* entry = m_atlas->find(key);

		if (!entry) {
			const text::CachedGlyph* cg = m_glyphCache.get_or_render(
				*g.font, g.fontId, g.glyphId, scaledSize, targetX, targetY);

			auto inserted = m_atlas->insert(key, cg);
			if (!inserted) {
				if (!m_textVertices.empty()) {
					m_atlas->flushToGpu(m_renderer.get(), m_fontTextureId);
					Paint p = s.fill;
					p.image = m_fontTextureId;
					p.radius = 1.0f;
					m_renderer->renderTriangles(p, s.compositeOperation, s.scissor,
					                           m_textVertices.data(), static_cast<int>(m_textVertices.size()), m_fringeWidth, 3);
					m_textVertices.clear();
				}
				if (m_renderer) {
					m_renderer->flush();
				}
				if (m_atlas->width() < 2048 || m_atlas->height() < 2048) {
					if (m_fontTextureId != 0 && m_renderer) {
						m_renderer->deleteTexture(m_fontTextureId);
						m_fontTextureId = 0;
					}
					m_atlas->resize(2048, 2048);
					m_glyphCache.clear();
					m_fontTextureId = m_renderer->createTexture(TextureType::Alpha, m_atlas->width(), m_atlas->height(), 0, m_atlas->data());
					m_atlas->markClean();
					inserted = m_atlas->insert(key, cg);
				} else {
					m_atlas->reset();
					m_glyphCache.clear();
					inserted = m_atlas->insert(key, cg);
				}
			}
			entry = inserted ? &(*inserted) : nullptr;
		}

		if (!entry || entry->width == 0 || entry->height == 0) continue;

		float qx0 = static_cast<float>(intX + entry->offsetX) * invscale;
		float qy0 = static_cast<float>(intY + entry->offsetY) * invscale;
		float qx1 = qx0 + static_cast<float>(entry->width) * invscale;
		float qy1 = qy0 + static_cast<float>(entry->height) * invscale;

		float u0 = entry->u0;
		float v0 = entry->v0;
		float u1 = entry->u1;
		float v1 = entry->v1;

		if (isFlipped) {
			std::swap(qy0, qy1);
			std::swap(v0, v1);
		}

		Point c0 = s.xform.transformPoint(qx0, qy0);
		Point c1 = s.xform.transformPoint(qx1, qy0);
		Point c2 = s.xform.transformPoint(qx1, qy1);
		Point c3 = s.xform.transformPoint(qx0, qy1);

		m_textVertices.emplace_back(c0.x, c0.y, u0, v0, vcol);
		m_textVertices.emplace_back(c2.x, c2.y, u1, v1, vcol);
		m_textVertices.emplace_back(c1.x, c1.y, u1, v0, vcol);

		m_textVertices.emplace_back(c0.x, c0.y, u0, v0, vcol);
		m_textVertices.emplace_back(c3.x, c3.y, u0, v1, vcol);
		m_textVertices.emplace_back(c2.x, c2.y, u1, v1, vcol);
	}

	if (m_atlas->isDirty()) {
		m_atlas->flushToGpu(m_renderer.get(), m_fontTextureId);
	}

	if (!m_textVertices.empty()) {
		Paint p = s.fill;
		p.image = m_fontTextureId;
		p.radius = 1.0f;
		m_renderer->renderTriangles(p, s.compositeOperation, s.scissor,
		                           m_textVertices.data(), static_cast<int>(m_textVertices.size()), m_fringeWidth, 3);
	}

	return x + totalWidth * invscale;
}

void Context::drawTextBuffer(const text::Buffer& buffer, text::FontSystem& fontSystem, Point pos, Color defaultColor, uint32_t passMask) {
	if (passMask == 0u) return;
	bool need_shaping = false;
	for (const auto& line : buffer.lines()) {
		if (!line.is_shaped()) {
			need_shaping = true;
			break;
		}
	}
	if (need_shaping) {
		const_cast<text::Buffer&>(buffer).shape_until_scroll(fontSystem);
	}

	auto runs = buffer.layout_runs();
	if (runs.empty()) return;

	const float scale = getFontScale(currentState()) * m_devicePixelRatio;
	const float invscale = 1.0f / scale;
	const bool isFlipped = isTransformFlipped(currentState().xform);

	m_textVertices.clear();

	const uint32_t defColorRgba = defaultColor.premultiplied().toRGBA8();

	auto flushBatch = [&]() {
		if (!m_textVertices.empty()) {
			m_atlas->flushToGpu(m_renderer.get(), m_fontTextureId);
			const State& cur = currentState();
			Paint p;
			p.image = m_fontTextureId;
			p.innerColor = Color(1.0f, 1.0f, 1.0f, cur.alpha);
			p.outerColor = Color(1.0f, 1.0f, 1.0f, cur.alpha);
			m_renderer->renderTriangles(p, cur.compositeOperation, cur.scissor,
			                           m_textVertices.data(), static_cast<int>(m_textVertices.size()), m_fringeWidth, 3);
			m_textVertices.clear();
		}
	};

	auto appendSolidQuadPts = [&](std::vector<Vertex>& out, Point p0, Point p1, Point p2, Point p3, uint32_t rgba) {
		out.emplace_back(p0.x, p0.y, 0.5f, 1.0f, rgba);
		out.emplace_back(p3.x, p3.y, 0.5f, 1.0f, rgba);
		out.emplace_back(p2.x, p2.y, 0.5f, 1.0f, rgba);
		out.emplace_back(p0.x, p0.y, 0.5f, 1.0f, rgba);
		out.emplace_back(p2.x, p2.y, 0.5f, 1.0f, rgba);
		out.emplace_back(p1.x, p1.y, 0.5f, 1.0f, rgba);
	};

	auto appendSolidRect = [&](std::vector<Vertex>& out, float rx, float ry, float rw, float rh, uint32_t rgba) {
		const State& cur = currentState();
		Point p0 = cur.xform.transformPoint(rx, ry);
		Point p1 = cur.xform.transformPoint(rx + rw, ry);
		Point p2 = cur.xform.transformPoint(rx + rw, ry + rh);
		Point p3 = cur.xform.transformPoint(rx, ry + rh);
		appendSolidQuadPts(out, p0, p1, p2, p3, rgba);
	};

	auto flushSolidRects = [&](std::vector<Vertex>& verts) {
		if (!verts.empty()) {
			const State& cur = currentState();
			Paint solidPaint;
			m_renderer->renderTriangles(solidPaint, cur.compositeOperation, cur.scissor,
			                           verts.data(), static_cast<int>(verts.size()), m_fringeWidth, 4 /* ShaderSolid */);
			verts.clear();
		}
	};

	auto emitGlyphQuad = [&](const text::TtfFont* font, uint32_t font_id, uint16_t glyph_id, float font_size,
	                         float glyph_x, float glyph_y, bool synth_bold, bool synth_italic, uint32_t vcolor,
	                         std::span<const text::FontVariation> variations = {}, uint64_t variation_hash = 0) {
		float scaledSize = font_size * scale;
		uint32_t sizeBits = 0;
		std::memcpy(&sizeBits, &scaledSize, sizeof(float));

		float targetX = glyph_x * scale;
		float targetY = glyph_y * scale;

		auto [intX, xBin] = text::compute_subpixel_bin(targetX);
		auto [intY, yBin] = text::compute_subpixel_bin(targetY);

		uint8_t styleFlags = (synth_bold ? 1 : 0) | (synth_italic ? 2 : 0);
		text::CacheKey key{font_id, glyph_id, sizeBits, xBin, yBin, styleFlags, variation_hash};
		const AtlasGlyphEntry* entry = m_atlas->find(key);

		if (!entry) {
			const text::CachedGlyph* cg = m_glyphCache.get_or_render(
				*font, font_id, glyph_id, scaledSize, targetX, targetY,
				synth_bold, synth_italic, variations, variation_hash);

			auto inserted = m_atlas->insert(key, cg);
			if (!inserted) {
				flushBatch();
				if (m_renderer) {
					m_renderer->flush();
				}
				if (m_atlas->width() < 2048 || m_atlas->height() < 2048) {
					if (m_fontTextureId != 0 && m_renderer) {
						m_renderer->deleteTexture(m_fontTextureId);
						m_fontTextureId = 0;
					}
					m_atlas->resize(2048, 2048);
					m_glyphCache.clear();
					m_fontTextureId = m_renderer->createTexture(TextureType::Alpha, 2048, 2048, 0, m_atlas->data());
					m_atlas->markClean();
					inserted = m_atlas->insert(key, cg);
				} else {
					m_atlas->reset();
					m_glyphCache.clear();
					inserted = m_atlas->insert(key, cg);
				}
			}
			entry = inserted ? &(*inserted) : nullptr;
		}

		if (!entry || entry->width == 0 || entry->height == 0) return;

		float qx0 = static_cast<float>(intX + entry->offsetX) * invscale;
		float qy0 = static_cast<float>(intY + entry->offsetY) * invscale;
		float qx1 = qx0 + static_cast<float>(entry->width) * invscale;
		float qy1 = qy0 + static_cast<float>(entry->height) * invscale;

		float u0 = entry->u0;
		float v0 = entry->v0;
		float u1 = entry->u1;
		float v1 = entry->v1;

		if (isFlipped) {
			std::swap(qy0, qy1);
			std::swap(v0, v1);
		}

		const State& cur = currentState();
		Point c0 = cur.xform.transformPoint(qx0, qy0);
		Point c1 = cur.xform.transformPoint(qx1, qy0);
		Point c2 = cur.xform.transformPoint(qx1, qy1);
		Point c3 = cur.xform.transformPoint(qx0, qy1);

		m_textVertices.emplace_back(c0.x, c0.y, u0, v0, vcolor);
		m_textVertices.emplace_back(c2.x, c2.y, u1, v1, vcolor);
		m_textVertices.emplace_back(c1.x, c1.y, u1, v0, vcolor);

		m_textVertices.emplace_back(c0.x, c0.y, u0, v0, vcolor);
		m_textVertices.emplace_back(c3.x, c3.y, u0, v1, vcolor);
		m_textVertices.emplace_back(c2.x, c2.y, u1, v1, vcolor);
	};

	std::vector<Vertex> solidVerts;
	std::vector<text::ColorGlyphLayer> color_layers;

	auto resolveGlyphFont = [&](uint32_t fid) -> const text::TtfFont* {
		const text::TtfFont* f = fontSystem.get_font(fid);
		if (!f) {
			if (auto defId = fontSystem.default_font_id()) {
				f = fontSystem.get_font(*defId);
			}
		}
		return f;
	};

	auto emitGlyphWithColor = [&](const text::LayoutGlyph& glyph, const text::TtfFont* font,
	                              float gx, float gy, uint32_t overrideRgba, bool forceSolid) {
		const std::span<const text::FontVariation> vars = glyph.variation_coords_opt
			? std::span<const text::FontVariation>(*glyph.variation_coords_opt)
			: std::span<const text::FontVariation>{};
		if (font->has_color_glyphs() && font->get_color_glyph_layers(glyph.glyph_id, color_layers)) {
			for (const auto& layer : color_layers) {
				uint32_t layerRgba = (forceSolid || layer.use_foreground)
					? overrideRgba
					: Color(layer.color.to_color()).premultiplied().toRGBA8();
				emitGlyphQuad(font, glyph.font_id, layer.glyph_id, glyph.font_size,
				              gx, gy, glyph.synthetic_bold, glyph.synthetic_italic, layerRgba,
				              vars, glyph.variation_hash);
			}
			return;
		}
		emitGlyphQuad(font, glyph.font_id, glyph.glyph_id, glyph.font_size,
		              gx, gy, glyph.synthetic_bold, glyph.synthetic_italic, overrideRgba,
		              vars, glyph.variation_hash);
	};

	std::vector<float> blur_sigmas;
	bool has_sharp_shadows = false;
	if ((passMask & 0x02u) != 0u) {
		for (const auto& run : runs) {
			for (const auto& glyph : run.glyphs) {
				if (glyph.is_placeholder) continue;
				for (const auto& sh : glyph.shadows) {
					if (sh.blur_sigma > 0.1f) {
						if (std::find_if(blur_sigmas.begin(), blur_sigmas.end(), [&](float s) {
								return std::abs(s - sh.blur_sigma) < 0.05f;
							}) == blur_sigmas.end()) {
							blur_sigmas.push_back(sh.blur_sigma);
						}
					} else {
						has_sharp_shadows = true;
					}
				}
			}
		}
	}

	for (const auto& run : runs) {
		const float baseline_offset = (run.max_ascent > 0.0f) ? run.max_ascent : buffer.metrics().font_size;
		const float line_baseline = pos.y + run.line_top + baseline_offset;

		// Pass 1: Span background highlights (solid colors only; rich Paint backgrounds handled by GpuCanvas)
		if ((passMask & 0x01u) != 0u && run.line_height > 0.0f) {
			const float curAlpha = currentState().alpha;
			for (const auto& glyph : run.glyphs) {
				if (glyph.background_paint_opt != nullptr) continue;
				if (glyph.background_color_opt.has_value() && glyph.w > 0.0f) {
					Color bg = glyph.background_color_opt->to_color();
					bg.a *= curAlpha;
					appendSolidRect(solidVerts, pos.x + glyph.x, pos.y + run.line_top,
					                glyph.w, run.line_height, bg.premultiplied().toRGBA8());
				}
			}
			if (!solidVerts.empty()) {
				flushBatch();
				flushSolidRects(solidVerts);
			}
		}

		// Pass 1.5a: Sharp TextShadows
		if ((passMask & 0x02u) != 0u && has_sharp_shadows) {
			for (const auto& glyph : run.glyphs) {
				if (glyph.is_placeholder || glyph.glyph_id == 0 || glyph.shadows.empty()) continue;
				const text::TtfFont* font = resolveGlyphFont(glyph.font_id);
				if (!font) continue;
				const float gx = pos.x + glyph.x + glyph.x_offset;
				const float gy = line_baseline + glyph.y + glyph.y_offset;
				for (const auto& sh : glyph.shadows) {
					if (sh.blur_sigma <= 0.1f) {
						const uint32_t shRgba = Color(sh.color.to_color()).premultiplied().toRGBA8();
						emitGlyphWithColor(glyph, font, gx + sh.offset_x, gy + sh.offset_y, shRgba, true);
					}
				}
			}
		}

		// Pass 1.5b: Blurred TextShadows via Gaussian blur offscreen layer
		if ((passMask & 0x02u) != 0u) for (float sigma : blur_sigmas) {
			float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
			bool any_for_sigma = false;
			const Transform2D xf = currentState().xform;
			for (const auto& glyph : run.glyphs) {
				if (glyph.is_placeholder || glyph.glyph_id == 0 || glyph.shadows.empty()) continue;
				const float gx = pos.x + glyph.x + glyph.x_offset;
				for (const auto& sh : glyph.shadows) {
					if (sh.blur_sigma > 0.1f && std::abs(sh.blur_sigma - sigma) < 0.05f) {
						any_for_sigma = true;
						const float lx0 = gx + sh.offset_x - glyph.font_size * 0.5f;
						const float lx1 = gx + sh.offset_x + std::max(glyph.w, glyph.font_size) + glyph.font_size * 0.5f;
						const float ly0 = pos.y + run.line_top + sh.offset_y - glyph.font_size * 0.5f;
						const float ly1 = pos.y + run.line_top + run.line_height + sh.offset_y + glyph.font_size * 0.5f;
						const Point corners[4] = {
							xf.transformPoint(lx0, ly0),
							xf.transformPoint(lx1, ly0),
							xf.transformPoint(lx1, ly1),
							xf.transformPoint(lx0, ly1)
						};
						for (const auto& cp : corners) {
							minX = std::min(minX, cp.x);
							minY = std::min(minY, cp.y);
							maxX = std::max(maxX, cp.x);
							maxY = std::max(maxY, cp.y);
						}
					}
				}
			}
			if (!any_for_sigma) continue;

			flushBatch();
			const float pad = std::ceil(sigma * 3.0f) + 2.0f;
			LayerDesc ldesc;
			ldesc.hasBounds = true;
			ldesc.bounds[0] = std::floor(minX - pad);
			ldesc.bounds[1] = std::floor(minY - pad);
			ldesc.bounds[2] = std::ceil(maxX + pad);
			ldesc.bounds[3] = std::ceil(maxY + pad);
			ldesc.postBlurSigmaX = sigma;
			ldesc.postBlurSigmaY = sigma;
			ldesc.opacity = 1.0f;
			ldesc.compOp = currentState().compositeOperation;
			pushLayer(ldesc);

			for (const auto& glyph : run.glyphs) {
				if (glyph.is_placeholder || glyph.glyph_id == 0 || glyph.shadows.empty()) continue;
				const text::TtfFont* font = resolveGlyphFont(glyph.font_id);
				if (!font) continue;
				const float gx = pos.x + glyph.x + glyph.x_offset;
				const float gy = line_baseline + glyph.y + glyph.y_offset;
				for (const auto& sh : glyph.shadows) {
					if (sh.blur_sigma > 0.1f && std::abs(sh.blur_sigma - sigma) < 0.05f) {
						const uint32_t shRgba = Color(sh.color.to_color()).premultiplied().toRGBA8();
						emitGlyphWithColor(glyph, font, gx + sh.offset_x, gy + sh.offset_y, shRgba, true);
					}
				}
			}
			flushBatch();
			restore();
		}

		// Pass 2: Glyphs (including COLR/CPAL color layers; skips placeholders and vector rich-Paint glyphs)
		if ((passMask & 0x04u) != 0u) {
			for (const auto& glyph : run.glyphs) {
				if (glyph.is_placeholder || glyph.glyph_id == 0) continue;
				if (glyph.foreground_paint_opt && text::paint_requires_vector_path(*glyph.foreground_paint_opt)) {
					continue;
				}
				const text::TtfFont* font = resolveGlyphFont(glyph.font_id);
				if (!font) continue;

				const float glyph_x = pos.x + glyph.x + glyph.x_offset;
				const float glyph_y = line_baseline + glyph.y + glyph.y_offset;
				uint32_t fgRgba = defColorRgba;
				if (glyph.foreground_paint_opt) {
					fgRgba = Color(glyph.foreground_paint_opt->shader.solid_color()).premultiplied().toRGBA8();
				} else if (glyph.color_opt.has_value()) {
					fgRgba = Color(glyph.color_opt->to_color()).premultiplied().toRGBA8();
				}

				emitGlyphWithColor(glyph, font, glyph_x, glyph_y, fgRgba, false);
			}
		}

		// Pass 3: Decorations (underline + DecorationMode::Gaps, strikethrough, overline, all 5 styles)
		if ((passMask & 0x08u) != 0u) {
			const auto dec = text::build_run_decorations(
				run, pos.x, pos.y, buffer.metrics().font_size,
				nisaba::Color::from_rgba_unchecked(defaultColor.r, defaultColor.g, defaultColor.b, defaultColor.a),
				&fontSystem
			);
			const State& cur = currentState();
			for (const auto& r : dec.rects) {
				Color dc(r.color);
				dc.a *= cur.alpha;
				appendSolidRect(solidVerts, r.x, r.y, r.w, r.h, dc.premultiplied().toRGBA8());
			}
			for (const auto& wv : dec.waves) {
				Color dc(wv.color);
				dc.a *= cur.alpha;
				const uint32_t dcRgba = dc.premultiplied().toRGBA8();
				const float halfW = wv.stroke_width * 0.5f;
				for (size_t i = 0; i + 1 < wv.points.size(); ++i) {
					const auto& a = wv.points[i];
					const auto& b = wv.points[i + 1];
					const float dx = b.x - a.x;
					const float dy = b.y - a.y;
					const float len = std::hypot(dx, dy);
					if (len <= 1e-4f) continue;
					const float nx = (-dy / len) * halfW;
					const float ny = ( dx / len) * halfW;
					Point p0 = cur.xform.transformPoint(a.x - nx, a.y - ny);
					Point p1 = cur.xform.transformPoint(b.x - nx, b.y - ny);
					Point p2 = cur.xform.transformPoint(b.x + nx, b.y + ny);
					Point p3 = cur.xform.transformPoint(a.x + nx, a.y + ny);
					appendSolidQuadPts(solidVerts, p0, p1, p2, p3, dcRgba);
				}
			}
			if (!solidVerts.empty()) {
				flushBatch();
				flushSolidRects(solidVerts);
			}
		}
	}

	flushBatch();
}


float Context::textBounds(float x, float y, const char* string, const char* end, float* bounds) {
	State& s = currentState();
	if (s.fontId <= 0 || !string) {
		if (bounds) bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0f;
		return x;
	}
	if (!end) end = string + std::strlen(string);
	if (string == end) {
		if (bounds) bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0f;
		return x;
	}

	float scale = getFontScale(s) * m_devicePixelRatio;
	float invscale = 1.0f / scale;
	float scaledSize = s.fontSize * scale;

	const text::TtfFont* primaryFont = m_fontSystem.get_font(static_cast<uint32_t>(s.fontId));
	if (!primaryFont) {
		if (bounds) bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0f;
		return x;
	}

	float totalWidth = 0.0f;
	auto glyphs = layoutGlyphs(s, scaledSize, string, end, &totalWidth);

	float alignX = 0.0f, alignY = 0.0f;
	computeAlignmentOffsets(s, primaryFont, scaledSize, totalWidth, alignX, alignY);

	if (bounds) {
		float fontAscent = primaryFont->ascent(scaledSize);
		float fontDescent = primaryFont->descent(scaledSize);

		float minX = x * scale + alignX;
		float maxX = minX + totalWidth;
		float minY = y * scale + alignY - fontAscent;
		float maxY = y * scale + alignY - fontDescent;

		bounds[0] = minX * invscale;
		bounds[1] = minY * invscale;
		bounds[2] = maxX * invscale;
		bounds[3] = maxY * invscale;
	}

	return totalWidth * invscale;
}

void Context::textMetrics(float* ascender, float* descender, float* lineh) {
	State& s = currentState();
	if (s.fontId <= 0) {
		if (ascender) *ascender = 0.0f;
		if (descender) *descender = 0.0f;
		if (lineh) *lineh = 0.0f;
		return;
	}

	const text::TtfFont* font = m_fontSystem.get_font(static_cast<uint32_t>(s.fontId));
	if (!font) {
		if (ascender) *ascender = 0.0f;
		if (descender) *descender = 0.0f;
		if (lineh) *lineh = 0.0f;
		return;
	}

	float scale = font->scale_for_size(s.fontSize);
	if (ascender) *ascender = font->ascent() * scale;
	if (descender) *descender = font->descent() * scale;
	if (lineh) *lineh = (font->ascent() - font->descent() + font->line_gap()) * scale;
}

int Context::textBreakLines(const char* string, const char* end, float breakRowWidth, TextRow* rows, int maxRows) {
	State& s = currentState();
	if (maxRows <= 0 || s.fontId <= 0 || !string) return 0;
	if (!end) end = string + std::strlen(string);
	if (string == end) return 0;

	float scale = getFontScale(s) * m_devicePixelRatio;
	float invscale = 1.0f / scale;
	float scaledSize = s.fontSize * scale;
	breakRowWidth *= scale;

	float totalWidth = 0.0f;
	auto glyphs = layoutGlyphs(s, scaledSize, string, end, &totalWidth);
	if (glyphs.empty()) return 0;

	int nrows = 0;
	const char* rowStart = nullptr;
	const char* rowEnd = nullptr;
	float rowStartX = 0.0f;
	float rowWidth = 0.0f;

	const char* wordStart = nullptr;
	float wordStartX = 0.0f;
	const char* breakEnd = nullptr;
	float breakWidth = 0.0f;

	for (size_t i = 0; i < glyphs.size(); ++i) {
		const auto& g = glyphs[i];
		char c = *g.str;
		bool isSpace = (c == ' ' || c == '\t');
		bool isNewline = (c == '\n' || c == '\r');

		if (isNewline) {
			if (c == '\r' && (i + 1 < glyphs.size()) && *glyphs[i + 1].str == '\n') {
				// Skip \r in \r\n
				continue;
			}
			rows[nrows].start = rowStart ? rowStart : g.str;
			rows[nrows].end = rowEnd ? rowEnd : g.str;
			rows[nrows].width = rowWidth * invscale;
			rows[nrows].minx = 0.0f;
			rows[nrows].maxx = rows[nrows].width;
			rows[nrows].next = g.str + 1;
			nrows++;
			if (nrows >= maxRows) return nrows;

			rowStart = rowEnd = nullptr;
			rowWidth = 0.0f;
			wordStart = breakEnd = nullptr;
			breakWidth = 0.0f;
			continue;
		}

		if (!rowStart) {
			if (!isSpace) {
				rowStart = g.str;
				rowEnd = g.str + 1;
				rowStartX = g.x;
				rowWidth = g.advance;
				wordStart = g.str;
				wordStartX = g.x;
				breakEnd = rowStart;
				breakWidth = 0.0f;
			}
		} else {
			float nextWidth = (g.x + g.advance) - rowStartX;
			if (!isSpace) {
				rowEnd = g.str + 1;
				rowWidth = nextWidth;
			}
			if (isSpace) {
				breakEnd = g.str;
				breakWidth = rowWidth;
			} else if (i > 0 && (*glyphs[i - 1].str == ' ' || *glyphs[i - 1].str == '\t')) {
				wordStart = g.str;
				wordStartX = g.x;
			}

			if (!isSpace && nextWidth > breakRowWidth) {
				if (breakEnd && breakEnd != rowStart) {
					rows[nrows].start = rowStart;
					rows[nrows].end = breakEnd;
					rows[nrows].width = breakWidth * invscale;
					rows[nrows].minx = 0.0f;
					rows[nrows].maxx = rows[nrows].width;
					rows[nrows].next = wordStart ? wordStart : g.str;
					nrows++;
					if (nrows >= maxRows) return nrows;

					rowStart = wordStart ? wordStart : g.str;
					rowStartX = wordStartX;
					rowEnd = g.str + 1;
					rowWidth = (g.x + g.advance) - rowStartX;
				} else {
					rows[nrows].start = rowStart;
					rows[nrows].end = g.str;
					rows[nrows].width = rowWidth * invscale;
					rows[nrows].minx = 0.0f;
					rows[nrows].maxx = rows[nrows].width;
					rows[nrows].next = g.str;
					nrows++;
					if (nrows >= maxRows) return nrows;

					rowStart = g.str;
					rowStartX = g.x;
					rowEnd = g.str + 1;
					rowWidth = g.advance;
					wordStart = g.str;
					wordStartX = g.x;
				}
				breakEnd = rowStart;
				breakWidth = 0.0f;
			}
		}
	}

	if (rowStart) {
		rows[nrows].start = rowStart;
		rows[nrows].end = rowEnd ? rowEnd : end;
		rows[nrows].width = rowWidth * invscale;
		rows[nrows].minx = 0.0f;
		rows[nrows].maxx = rows[nrows].width;
		rows[nrows].next = end;
		nrows++;
	}

	return nrows;
}

void Context::textBox(float x, float y, float breakRowWidth, const char* string, const char* end) {
	State& s = currentState();
	if (s.fontId <= 0 || !string) return;

	float lineh = 0.0f;
	textMetrics(nullptr, nullptr, &lineh);

	Align oldAlign = s.textAlign;
	bool halignLeft = hasFlag(oldAlign, Align::Left);
	bool halignCenter = hasFlag(oldAlign, Align::Center);
	bool halignRight = hasFlag(oldAlign, Align::Right);

	s.textAlign = Align::Left | (oldAlign & (Align::Top | Align::Middle | Align::Bottom | Align::Baseline));

	TextRow rows[2];
	int nrows = 0;

	while ((nrows = textBreakLines(string, end, breakRowWidth, rows, 2))) {
		for (int i = 0; i < nrows; i++) {
			const TextRow& row = rows[i];
			if (halignLeft)
				text(x, y, row.start, row.end);
			else if (halignCenter)
				text(x + breakRowWidth * 0.5f - row.width * 0.5f, y, row.start, row.end);
			else if (halignRight)
				text(x + breakRowWidth - row.width, y, row.start, row.end);
			y += lineh * s.lineHeight;
		}
		string = rows[nrows - 1].next;
	}

	s.textAlign = oldAlign;
}

void Context::textBoxBounds(float x, float y, float breakRowWidth, const char* string, const char* end, float* bounds) {
	State& s = currentState();
	if (s.fontId <= 0 || !string) {
		if (bounds) bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0f;
		return;
	}

	float lineh = 0.0f;
	textMetrics(nullptr, nullptr, &lineh);

	float minx = x, miny = y, maxx = x, maxy = y;
	TextRow rows[2];
	int nrows = 0;

	while ((nrows = textBreakLines(string, end, breakRowWidth, rows, 2))) {
		for (int i = 0; i < nrows; i++) {
			const TextRow& row = rows[i];
			float rminx = x + row.minx;
			float rmaxx = x + row.maxx;
			minx = std::min(minx, rminx);
			maxx = std::max(maxx, rmaxx);
			maxy = y + lineh;
			y += lineh * s.lineHeight;
		}
		string = rows[nrows - 1].next;
	}

	if (bounds) {
		bounds[0] = minx;
		bounds[1] = miny;
		bounds[2] = maxx;
		bounds[3] = maxy;
	}
}

int Context::textGlyphPositions(float x, float y, const char* string, const char* end, GlyphPosition* positions, int maxPositions) {
	(void)y;
	State& s = currentState();
	if (s.fontId <= 0 || !string || maxPositions <= 0) return 0;
	if (!end) end = string + std::strlen(string);
	if (string == end) return 0;

	float scale = getFontScale(s) * m_devicePixelRatio;
	float invscale = 1.0f / scale;
	float scaledSize = s.fontSize * scale;

	const text::TtfFont* primaryFont = m_fontSystem.get_font(static_cast<uint32_t>(s.fontId));
	if (!primaryFont) return 0;

	float totalWidth = 0.0f;
	auto glyphs = layoutGlyphs(s, scaledSize, string, end, &totalWidth);
	if (glyphs.empty()) return 0;

	float alignX = 0.0f, alignY = 0.0f;
	computeAlignmentOffsets(s, primaryFont, scaledSize, totalWidth, alignX, alignY);

	float startX = x * scale + alignX;
	int count = std::min(static_cast<int>(glyphs.size()), maxPositions);

	for (int i = 0; i < count; ++i) {
		const auto& g = glyphs[i];
		positions[i].str = g.str;
		positions[i].x = (startX + g.x) * invscale;
		positions[i].minx = positions[i].x;
		positions[i].maxx = (startX + g.x + g.advance) * invscale;
	}

	return count;
}

} // namespace nisaba::gpu
