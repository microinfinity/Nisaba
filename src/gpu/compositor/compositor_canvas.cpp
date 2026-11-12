#include "nisaba/compositor/compositor_canvas.hpp"
#include "nisaba/compositor/log.hpp"

namespace nisaba::compositor {

std::unique_ptr<CompositorCanvas> CompositorCanvas::create(
    std::shared_ptr<EglContext> egl,
    std::shared_ptr<Renderer> renderer) {
    if (!renderer) {
        renderer = Renderer::create(egl);
    }
    if (!renderer) {
        logError("CompositorCanvas::create: Failed to create Renderer");
        return nullptr;
    }

    if (!egl) {
        egl = renderer->egl();
    }

    return std::make_unique<CompositorCanvas>(std::move(egl), std::move(renderer));
}

CompositorCanvas::CompositorCanvas(
    std::shared_ptr<EglContext> egl,
    std::shared_ptr<Renderer> renderer)
    : m_egl(std::move(egl)), m_renderer(std::move(renderer)) {}

CompositorCanvas::~CompositorCanvas() = default;

bool CompositorCanvas::beginFrame(
    std::shared_ptr<RenderTarget> target,
    wl_output_transform transform) {
    if (!m_renderer || !target) return false;

    m_currentDamage = nullptr;
    m_currentTarget = target;
    m_currentPass = m_renderer->beginPass(target, transform);
    return m_currentPass != nullptr;
}

void CompositorCanvas::setDamage(const Region* damage) {
    m_currentDamage = damage;
}

void CompositorCanvas::clear(const Color& color) {
    if (m_currentPass) {
        m_currentPass->clear(color);
    }
}

void CompositorCanvas::drawWindowSurface(
    const Texture& texture,
    const RectI& destBox,
    const RectF& srcBox,
    float alpha,
    wl_output_transform transform) {
    if (m_currentPass) {
        m_currentPass->renderTexture(texture, destBox, srcBox, alpha, transform, m_currentDamage);
    }
}

void CompositorCanvas::drawRect(const RectI& box, const Color& color) {
    if (m_currentPass) {
        m_currentPass->renderRect(box, color, m_currentDamage);
    }
}

void CompositorCanvas::drawFrostedGlass(
    const Texture& source,
    const RectI& destBox,
    const RectI& srcBox,
    const DualKawaseConfig& config,
    const Texture* maskTexture) {
    if (m_currentPass && m_renderer && m_renderer->blur()) {
        m_currentPass->renderBlur(*m_renderer->blur(), source, destBox, srcBox, config, maskTexture);
    }
}

void CompositorCanvas::drawBlurMaterial(
    const Texture& source,
    const RectI& destBox,
    const BlurContext& ctx,
    const RectI& srcBox) {
    if (m_currentPass && m_renderer && m_renderer->blurProvider() && m_renderer->blur()) {
        m_currentPass->renderBlurMaterial(
            *m_renderer->blurProvider(), *m_renderer->blur(), source, destBox, srcBox, ctx);
    }
}

void CompositorCanvas::drawScene(const SceneTree& tree) {
    if (m_currentPass) {
        tree.render(*m_currentPass, PointI{0, 0}, m_currentDamage);
    }
}

PassExecutionStats CompositorCanvas::drawSceneCulled(
    const SceneTree& tree,
    const ImageDescription& outputDesc) {
    if (!m_currentPass) return {};
    Region fullDmg;
    if (m_currentDamage) {
        fullDmg = *m_currentDamage;
    } else if (m_currentTarget) {
        fullDmg = Region(RectI{0, 0, m_currentTarget->width(), m_currentTarget->height()});
    }
    return tree.renderCulled(*m_currentPass, fullDmg, outputDesc);
}

gpu::GpuCanvas* CompositorCanvas::vg() {
    if (!m_currentTarget) return nullptr;

    if (!m_gpuDevice) {
        m_gpuDevice = gpu::GpuDevice::create();
    }
    if (!m_gpuDevice) return nullptr;

    uint32_t w = static_cast<uint32_t>(m_currentTarget->width());
    uint32_t h = static_cast<uint32_t>(m_currentTarget->height());
    uint32_t fbo = m_currentTarget->fbo();

    if (!m_gpuSurface || m_gpuSurface->width() != w || m_gpuSurface->height() != h || m_gpuSurface->fbo_id() != fbo) {
        m_gpuCanvas.reset();
        m_gpuSurface = gpu::GpuSurface::from_render_target(m_gpuDevice, w, h, fbo);
        if (m_gpuSurface) {
            m_gpuCanvas = std::make_unique<gpu::GpuCanvas>(m_gpuSurface);
            if (m_gpuCanvas && m_gpuCanvas->context()) {
                const char* sansPaths[] = {
                    "fonts/Inter-Regular.ttf", "../fonts/Inter-Regular.ttf",
                    "fonts/NotoSans-Regular.ttf", "../fonts/NotoSans-Regular.ttf"
                };
                for (const auto* p : sansPaths) {
                    if (m_gpuCanvas->context()->createFont("sans", p) != -1) break;
                }
                const char* monoPaths[] = {
                    "fonts/FiraMono-Medium.ttf", "../fonts/FiraMono-Medium.ttf"
                };
                for (const auto* p : monoPaths) {
                    if (m_gpuCanvas->context()->createFont("mono", p) != -1) break;
                }
            }
        }
    }

    if (m_gpuCanvas) {
        if (m_currentDamage) {
            auto ext = m_currentDamage->extents();
            auto clipRect = nisaba::Rect::from_xywh(
                static_cast<float>(ext.x),
                static_cast<float>(ext.y),
                static_cast<float>(ext.width),
                static_cast<float>(ext.height)
            );
            if (clipRect) {
                m_gpuCanvas->clip_rect(*clipRect);
            }
        } else {
            m_gpuCanvas->reset_clip();
        }
    }

    return m_gpuCanvas.get();
}

void CompositorCanvas::drawWindowDecoration(
    const RectI& clientBox,
    std::string_view title) {
    drawWindowDecoration(clientBox, WindowDecoStyle{}, title);
}

void CompositorCanvas::drawWindowDecoration(
    const RectI& clientBox,
    const WindowDecoStyle& style,
    std::string_view title) {
    if (!m_currentPass) return;

    RectI fullBox{
        clientBox.x,
        clientBox.y - static_cast<int>(style.titleBarHeight),
        clientBox.width,
        clientBox.height + static_cast<int>(style.titleBarHeight)
    };

    int shadowPad = static_cast<int>(std::ceil(style.shadow.sigma * 2.5f + std::max(std::abs(style.shadow.dx), std::abs(style.shadow.dy))));
    RectI expandedBounds{
        fullBox.x - shadowPad,
        fullBox.y - shadowPad,
        fullBox.width + shadowPad * 2,
        fullBox.height + shadowPad * 2
    };

    if (m_currentDamage && !m_currentDamage->intersects(expandedBounds)) {
        return; // Early discard via O(1) damage culling
    }

    // 1. Native Compositor Analytical Drop Shadow (zero GlStateGuard overhead)
    Color shadowCol(
        style.shadow.color.red(),
        style.shadow.color.green(),
        style.shadow.color.blue(),
        style.shadow.color.alpha());
    if (style.shadow.sigma > 0.5f && shadowCol.a > 0.001f) {
        RectI shadowFullBox{
            expandedBounds.x + static_cast<int>(std::round(style.shadow.dx)),
            expandedBounds.y + static_cast<int>(std::round(style.shadow.dy)),
            expandedBounds.width,
            expandedBounds.height
        };
        m_currentPass->renderShadow(
            shadowFullBox,
            style.cornerRadius,
            style.roundingPower,
            style.shadow.sigma,
            2.2f,
            GradientSpec(shadowCol),
            nullptr,
            0.0f,
            1.0f,
            m_currentDamage);
    }

    // 2. Native Squircle Window Body fill
    m_currentPass->renderRoundedRect(
        fullBox,
        style.bodyColor,
        style.cornerRadius,
        style.roundingPower,
        m_currentDamage);

    // 3. Titlebar background
    if (style.titleBarHeight > 0.5f) {
        RectI titleClipBox{
            fullBox.x,
            fullBox.y,
            fullBox.width,
            static_cast<int>(style.titleBarHeight)
        };
        Region titleDamage(titleClipBox);
        if (m_currentDamage) {
            titleDamage.intersect(*m_currentDamage);
        }
        if (!titleDamage.isEmpty()) {
            RectI titleRoundBox{
                fullBox.x,
                fullBox.y,
                fullBox.width,
                static_cast<int>(style.titleBarHeight + style.cornerRadius)
            };
            m_currentPass->renderRoundedRect(
                titleRoundBox,
                style.titleBarColor,
                style.cornerRadius,
                style.roundingPower,
                &titleDamage);
        }
    }

    // 4. Native Inner Glow (optional)
    if (style.innerGlowRange > 0.5f && style.innerGlowColor.a > 0.001f) {
        m_currentPass->renderInnerGlow(
            fullBox,
            style.cornerRadius,
            style.roundingPower,
            style.innerGlowRange,
            2.0f,
            GradientSpec(style.innerGlowColor),
            1.0f,
            m_currentDamage);
    }

    // 5. Native Oklab / Solid Squircle Border
    if (style.borderWidth > 0.0f) {
        GradientSpec borderGrad = !style.borderGradient.isEmpty()
                                      ? style.borderGradient
                                      : GradientSpec(style.borderColor);
        m_currentPass->renderBorder(
            fullBox,
            borderGrad,
            nullptr,
            0.0f,
            style.cornerRadius,
            style.cornerRadius + style.borderWidth,
            style.roundingPower,
            style.borderWidth,
            1.0f,
            m_currentDamage);
    }

    // 6. Window control buttons & Title text
    if (style.showButtons) {
        int btnSize = 11;
        int btnY = fullBox.y + static_cast<int>((style.titleBarHeight - static_cast<float>(btnSize)) * 0.5f);
        int startBtnX = fullBox.x + 11;
        m_currentPass->renderRoundedRect(
            RectI{startBtnX, btnY, btnSize, btnSize},
            style.closeButtonColor, 5.5f, 2.0f, m_currentDamage);
        m_currentPass->renderRoundedRect(
            RectI{startBtnX + 16, btnY, btnSize, btnSize},
            style.minButtonColor, 5.5f, 2.0f, m_currentDamage);
        m_currentPass->renderRoundedRect(
            RectI{startBtnX + 32, btnY, btnSize, btnSize},
            style.maxButtonColor, 5.5f, 2.0f, m_currentDamage);
    }

    if (!title.empty()) {
        GlStateGuard guard;
        auto* canvas = vg();
        if (canvas) {
            float textX = static_cast<float>(fullBox.x) + (style.showButtons ? 72.0f : 16.0f);
            float textY = static_cast<float>(fullBox.y) + style.titleBarHeight * 0.70f;
            nisaba::Paint textPaint(nisaba::Color::from_rgba_unchecked(
                style.titleTextColor.r, style.titleTextColor.g, style.titleTextColor.b, style.titleTextColor.a
            ));
            canvas->draw_text(textX, textY, title, textPaint, style.titleFontSize, "sans");
        }
    }
}

void CompositorCanvas::drawDesktopPanel(
    const RectI& panelBounds,
    const effects::GlassParams& glassParams,
    const std::vector<const svg::SvgDocument*>& launcherIcons) {
    if (m_currentDamage && !m_currentDamage->intersects(panelBounds)) return;

    GlStateGuard guard;
    auto* canvas = vg();
    if (!canvas) return;

    auto rect = nisaba::Rect::from_xywh(
        static_cast<float>(panelBounds.x),
        static_cast<float>(panelBounds.y),
        static_cast<float>(panelBounds.width),
        static_cast<float>(panelBounds.height)
    );
    if (!rect) return;

    canvas->draw_glass_panel(*rect, 16.0f, 16.0f, glassParams);

    if (!launcherIcons.empty()) {
        float iconSize = std::min(static_cast<float>(panelBounds.height) * 0.65f, 44.0f);
        float spacing = 16.0f;
        float totalW = static_cast<float>(launcherIcons.size()) * iconSize + static_cast<float>(launcherIcons.size() - 1) * spacing;
        float startX = static_cast<float>(panelBounds.x) + (static_cast<float>(panelBounds.width) - totalW) * 0.5f;
        float startY = static_cast<float>(panelBounds.y) + (static_cast<float>(panelBounds.height) - iconSize) * 0.5f;

        for (size_t i = 0; i < launcherIcons.size(); ++i) {
            if (launcherIcons[i]) {
                auto iconRect = nisaba::Rect::from_xywh(startX + static_cast<float>(i) * (iconSize + spacing), startY, iconSize, iconSize);
                if (iconRect) {
                    canvas->draw_svg(*launcherIcons[i], *iconRect);
                }
            }
        }
    }
}

void CompositorCanvas::applyWindowShader(
    const Texture& windowTexture,
    const RectI& destBox,
    std::shared_ptr<gpu::RuntimeEffect> effect,
    gpu::RuntimeShaderBuilder uniforms) {
    if (destBox.isEmpty() || !windowTexture.id() || !effect) return;
    if (m_currentDamage && !m_currentDamage->intersects(destBox)) return;

    GlStateGuard guard;
    auto* canvas = vg();
    if (!canvas) return;

    auto rect = nisaba::Rect::from_xywh(
        static_cast<float>(destBox.x),
        static_cast<float>(destBox.y),
        static_cast<float>(destBox.width),
        static_cast<float>(destBox.height)
    );
    if (!rect) return;

    auto customShader = uniforms.makeShader(static_cast<const nisaba::Transform*>(nullptr));
    if (!customShader) return;

    nisaba::Paint p;
    p.custom_shader = customShader;
    canvas->fill_rect(*rect, p);
}

void CompositorCanvas::drawText(
    std::string_view text,
    float x,
    float y,
    const Color& color,
    float font_size,
    const char* font_name) {
    if (text.empty()) return;

    GlStateGuard guard;
    auto* canvas = vg();
    if (!canvas) return;

    nisaba::Paint p(nisaba::Color::from_rgba_unchecked(color.r, color.g, color.b, color.a));
    canvas->draw_text(x, y, text, p, font_size, font_name ? font_name : "sans");
}

void CompositorCanvas::drawSvg(
    const svg::SvgDocument& doc,
    const RectI& destBounds) {
    if (m_currentDamage && !m_currentDamage->intersects(destBounds)) return;

    GlStateGuard guard;
    auto* canvas = vg();
    if (!canvas) return;

    auto rect = nisaba::Rect::from_xywh(
        static_cast<float>(destBounds.x),
        static_cast<float>(destBounds.y),
        static_cast<float>(destBounds.width),
        static_cast<float>(destBounds.height)
    );
    if (rect) {
        canvas->draw_svg(doc, *rect);
    }
}

bool CompositorCanvas::endFrame() {
    if (m_gpuCanvas) {
        GlStateGuard guard;
        m_gpuCanvas->flush();
    }

    if (!m_currentPass) return false;

    bool ok = m_currentPass->submit();
    m_currentPass.reset();
    m_currentDamage = nullptr;
    m_currentTarget = nullptr;
    return ok;
}

} // namespace nisaba::compositor
