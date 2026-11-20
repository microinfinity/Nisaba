#pragma once

#include "nisaba/gpu/types.hpp"
#include "nisaba/gpu/renderer.hpp"
#include "nisaba/path/stroker.hpp"
#include <memory>
#include <vector>
#include "nisaba/text/font_system.hpp"
#include "nisaba/text/buffer.hpp"
#include "nisaba/text/glyph_cache.hpp"
#include <unordered_map>
#include <string>

namespace nisaba::gpu {

struct State {
	CompositeOperationState compositeOperation;
	bool shapeAntiAlias{true};
	Paint fill;
	Paint stroke;
	float strokeWidth{1.0f};
	float miterLimit{10.0f};
	LineCap lineCap{LineCap::Butt};
	LineJoin lineJoin{LineJoin::Miter};
	float alpha{1.0f};
	Transform2D xform{Transform2D::identity()};
	Scissor scissor;
	float fontSize{16.0f};
	float letterSpacing{0.0f};
	float lineHeight{1.0f};
	float fontBlur{0.0f};
	Align textAlign{Align::Left | Align::Baseline};
	int fontId{0};
	int clipDepth{0};
	bool isLayer{false};
};

class Context {
public:
	explicit Context(std::unique_ptr<Renderer> renderer, int flags = Antialias | StencilStrokes);
	~Context();

	Renderer* renderer() const { return m_renderer.get(); }

	// Frame Lifecycle
	void beginFrame(float windowWidth, float windowHeight, float devicePixelRatio);
	void cancelFrame();
	void endFrame();

	// State Handling
	void save();
	void pushLayer(const LayerDesc& desc);
	void restore();
	void reset();
	void resetState();

	// State Setting
	void shapeAntiAlias(bool enabled);
	void strokeColor(Color color);
	void strokePaint(const Paint& paint);
	void strokeShader(std::shared_ptr<class Shader> shader);
	void fillColor(Color color);
	void fillPaint(const Paint& paint);
	void fillShader(std::shared_ptr<class Shader> shader);
	void miterLimit(float limit);
	void strokeWidth(float size);
	void lineCap(LineCap cap);
	void lineCap(nisaba::LineCap cap);
	void lineJoin(LineJoin join);
	void lineJoin(nisaba::LineJoin join);
	void globalAlpha(float alpha);

	// Transforms
	void resetTransform();
	void transform(float a, float b, float c, float d, float e, float f);
	void transform(const Transform2D& t);
	void transform(const nisaba::Transform& ts);
	void translate(float x, float y);
	void rotate(float angleRad);
	void skewX(float angleRad);
	void skewY(float angleRad);
	void scale(float x, float y);
	Transform2D currentTransform() const;

	// Scissoring
	void scissor(float x, float y, float w, float h);
	void scissorScreen(float x, float y, float w, float h);
	void intersectScissor(float x, float y, float w, float h);
	void resetScissor();

	// Stencil Vector Clipping
	void clipPath(const nisaba::Path& p, Winding winding = Winding::CounterClockwise);
	void restoreClip(int targetDepth);
	void resetClip();
	[[nodiscard]] int clipDepth() const noexcept;

	// Global Compositing
	void globalCompositeOperation(CompositeOperation op);
	void globalCompositeBlendMode(BlendMode mode);
	void globalCompositeBlendFunc(BlendFactor sfactor, BlendFactor dfactor);
	void globalCompositeBlendFuncSeparate(BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha, BlendFactor dstAlpha);
	void setCompositeState(const CompositeOperationState& state);
	const CompositeOperationState& currentCompositeState() const;

	// Image Creation
	int createImage(const char* filename, int imageFlags = 0);
	int createImageMem(int imageFlags, unsigned char* data, int ndata);
	int createImageRGBA(int w, int h, int imageFlags, const unsigned char* data);
	int createImageWithFormat(int w, int h, int strideBytes, PixelFormat format, int imageFlags, const unsigned char* data);
	void updateImage(int image, const unsigned char* data);
	void imageSize(int image, int& w, int& h);
	void deleteImage(int image);

	// Paints
	Paint linearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	Paint boxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
	Paint radialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	Paint imagePattern(float cx, float cy, float w, float h, float angleRad, int image, float alpha);

	// Paths
	void beginPath();
	void path(const nisaba::Path& p, Winding defaultWinding = Winding::Preserve);
	void moveTo(float x, float y);
	void lineTo(float x, float y);
	void bezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	void quadTo(float cx, float cy, float x, float y);
	void arcTo(float x1, float y1, float x2, float y2, float radius);
	void closePath();
	void pathWinding(Winding dir);

	void arc(float cx, float cy, float r, float a0, float a1, Winding dir);
	void rect(float x, float y, float w, float h);
	void roundedRect(float x, float y, float w, float h, float r);
	void roundedRectVarying(float x, float y, float w, float h, float radTopLeft, float radTopRight, float radBottomRight, float radBottomLeft);
	void ellipse(float cx, float cy, float rx, float ry);
	void circle(float cx, float cy, float r);

	// Draw Operations
	void fill();
	void stroke();
	void drawTriangles(const Paint& paint, const Vertex* verts, int nverts, float fringe = 0.0f, int shaderType = 4);
	void rrectShadow(float x, float y, float w, float h, float rTL, float rTR, float rBR, float rBL, float sigma, Color color, const float* occludedBox = nullptr);

	// Fonts & Text
	int createFont(const char* name, const char* filename);
	int createFontAtIndex(const char* name, const char* filename, const int fontIndex);
	int createFontMem(const char* name, unsigned char* data, int ndata, int freeData);
	int createFontMemAtIndex(const char* name, unsigned char* data, int ndata, int freeData, const int fontIndex);
	int findFont(const char* name);
	int addFallbackFontId(int baseFont, int fallbackFont);
	int addFallbackFont(const char* baseFont, const char* fallbackFont);
	void resetFallbackFontsId(int baseFont);
	void resetFallbackFonts(const char* baseFont);

	void fontSize(float size);
	void fontBlur(float blur);
	void textLetterSpacing(float spacing);
	void textLineHeight(float lineHeight);
	void textAlign(Align align);
	void fontFaceId(int font);
	void fontFace(const char* font);

	float text(float x, float y, const char* string, const char* end = nullptr);
	float textWithFont(float x, float y, const text::TtfFont& font, float fontSize, const char* string, const char* end = nullptr);
	/// Draws a text::Buffer on GPU. passMask bits: 0x01=solid backgrounds, 0x02=shadows, 0x04=solid glyphs, 0x08=decorations.
	void drawTextBuffer(
		const text::Buffer& buffer,
		text::FontSystem& fontSystem,
		Point pos = Point{0.0f, 0.0f},
		Color defaultColor = Color::rgba(0, 0, 0, 255),
		uint32_t passMask = 0x0Fu
	);
	void textBox(float x, float y, float breakRowWidth, const char* string, const char* end = nullptr);
	float textBounds(float x, float y, const char* string, const char* end, float* bounds);
	void textBoxBounds(float x, float y, float breakRowWidth, const char* string, const char* end, float* bounds);
	int textGlyphPositions(float x, float y, const char* string, const char* end, GlyphPosition* positions, int maxPositions);
	void textMetrics(float* ascender, float* descender, float* lineh);
	int textBreakLines(const char* string, const char* end, float breakRowWidth, TextRow* rows, int maxRows);

	// Typography Subsystem Getters
	text::FontSystem& fontSystem() noexcept { return m_fontSystem; }
	const text::FontSystem& fontSystem() const noexcept { return m_fontSystem; }
	text::GlyphCache& glyphCache() noexcept { return m_glyphCache; }
	const text::GlyphCache& glyphCache() const noexcept { return m_glyphCache; }
	class SovereignGlyphAtlas& atlas() noexcept { return *m_atlas; }
	const class SovereignGlyphAtlas& atlas() const noexcept { return *m_atlas; }
	int fontTextureId() const noexcept { return m_fontTextureId; }

private:
	State& currentState() { return m_states.back(); }
	const State& currentState() const { return m_states.back(); }

	void appendCommand(const PathCommand& cmd);
	void setPaintColor(Paint& p, Color color);

	std::unique_ptr<Renderer> m_renderer;
	class Tessellator* m_tessellator{nullptr};
	std::unique_ptr<class SovereignGlyphAtlas> m_atlas;
	int m_fontTextureId{0};
	text::FontSystem m_fontSystem;
	text::GlyphCache m_glyphCache;
	std::unordered_map<std::string, int> m_fontNames;
	std::unordered_map<int, std::vector<int>> m_fallbacks;

	struct MeasuredGlyph {
		const text::TtfFont* font{nullptr};
		uint32_t fontId{0};
		uint16_t glyphId{0};
		float x{0.0f};
		float y{0.0f};
		float advance{0.0f};
		float scale{1.0f};
		const char* str{nullptr};
	};

	const text::TtfFont* resolveGlyphFont(int primaryFontId, char32_t cp, uint16_t* outGlyphId) const;
	const std::vector<MeasuredGlyph>& layoutGlyphsInternal(const State& s, const text::TtfFont* primaryFont, uint32_t fontId, float scaledSize, float fontSize, const char* string, const char* end, float* outTotalWidth) const;
	const std::vector<MeasuredGlyph>& layoutGlyphs(const State& s, float scaledSize, const char* string, const char* end, float* outTotalWidth) const;

	struct LayerEntry {
		LayerDesc desc;
		Scissor parentScissor;
		int parentClipDepth{0};
	};

	std::vector<State> m_states;
	std::vector<LayerEntry> m_layerStack;
	std::vector<PathCommand> m_commands;
	float m_commandX{0.0f};
	float m_commandY{0.0f};

	float m_distTol{0.01f};
	float m_tessTol{0.25f};
	float m_fringeWidth{1.0f};
	float m_devicePixelRatio{1.0f};
	float m_windowWidth{0.0f};
	float m_windowHeight{0.0f};
	int m_flags{0};

	mutable std::vector<RenderPath> m_renderPaths;
	mutable std::vector<Vertex> m_textVertices;
	mutable std::vector<MeasuredGlyph> m_measuredGlyphs;
	bool m_singleCircle{false};
	float m_circleCx{0.0f};
	float m_circleCy{0.0f};
	float m_circleR{0.0f};

	bool m_singleRect{false};
	float m_rectX{0.0f};
	float m_rectY{0.0f};
	float m_rectW{0.0f};
	float m_rectH{0.0f};

	bool m_singleRRect{false};
	float m_rrectX{0.0f};
	float m_rrectY{0.0f};
	float m_rrectW{0.0f};
	float m_rrectH{0.0f};
	float m_rrectR{0.0f};

	bool m_singleVaryingRRect{false};
	float m_radTL{0.0f};
	float m_radTR{0.0f};
	float m_radBR{0.0f};
	float m_radBL{0.0f};

	bool m_singleEllipse{false};
	float m_ellipseCx{0.0f};
	float m_ellipseCy{0.0f};
	float m_ellipseRx{0.0f};
	float m_ellipseRy{0.0f};

	void materializePending();
	void rectPath(float x, float y, float w, float h);
	void ellipsePath(float cx, float cy, float rx, float ry);
	void roundedRectVaryingPath(float x, float y, float w, float h, float radTopLeft, float radTopRight, float radBottomRight, float radBottomLeft);
};

} // namespace nisaba::gpu
