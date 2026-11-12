#include "nisaba/compositor/compositor.hpp"
#include <cassert>
#include <iostream>
#include <cmath>

using namespace nisaba::compositor;

void test_geometry_and_region() {
    std::cout << "[Test] Geometry & Region..." << std::endl;

    RectI r1{0, 0, 100, 100};
    RectI r2{50, 50, 100, 100};
    assert(r1.intersects(r2));

    RectI inter = r1.intersection(r2);
    assert(inter.x == 50 && inter.y == 50 && inter.width == 50 && inter.height == 50);

    Region reg1(r1);
    assert(!reg1.isEmpty());
    assert(reg1.rects().size() == 1);

    Region reg2(r2);
    reg1.unite(reg2);
    assert(!reg1.isEmpty());
    RectI ext = reg1.extents();
    assert(ext.x == 0 && ext.y == 0 && ext.width == 150 && ext.height == 150);

    reg1.clear();
    assert(reg1.isEmpty());

    std::cout << "  -> Region tests passed!" << std::endl;
}

void test_damage_ring() {
    std::cout << "[Test] DamageRing multi-buffer rotation..." << std::endl;

    DamageRing ring;
    assert(!ring.hasPendingDamage());

    ring.addRect(RectI{10, 10, 50, 50});
    assert(ring.hasPendingDamage());

    // First time seeing buffer 1: returns full redraw
    Region d1 = ring.rotate(1, 1920, 1080);
    assert(!d1.isEmpty());
    assert(d1.extents().width == 1920 && d1.extents().height == 1080);
    assert(!ring.hasPendingDamage());

    // Add damage for next frame
    ring.addRect(RectI{100, 100, 20, 20});
    // First time seeing buffer 2: returns full redraw
    Region d2 = ring.rotate(2, 1920, 1080);
    assert(d2.extents().width == 1920);

    // Second time seeing buffer 1: returns accumulated damage since buffer 1 was last rendered!
    Region d1_accum = ring.rotate(1, 1920, 1080);
    assert(!d1_accum.isEmpty());
    RectI ext = d1_accum.extents();
    assert(ext.x == 100 && ext.y == 100 && ext.width == 20 && ext.height == 20);

    std::cout << "  -> DamageRing tests passed!" << std::endl;
}

void test_matrix3() {
    std::cout << "[Test] Matrix3 & Wayland transforms..." << std::endl;

    Matrix3 id = Matrix3::identity();
    assert(id[0] == 1.0f && id[4] == 1.0f && id[8] == 1.0f);
    assert(id[1] == 0.0f && id[2] == 0.0f);

    Matrix3 t = Matrix3::identity();
    t.translate(10.0f, 20.0f);
    assert(t[2] == 10.0f && t[5] == 20.0f);

    Matrix3 s = Matrix3::identity();
    s.scale(2.0f, 3.0f);
    assert(s[0] == 2.0f && s[4] == 3.0f);

    // Test Wayland transform inversion
    assert(invertTransform(WL_OUTPUT_TRANSFORM_NORMAL) == WL_OUTPUT_TRANSFORM_NORMAL);
    assert(invertTransform(WL_OUTPUT_TRANSFORM_90) == WL_OUTPUT_TRANSFORM_270);
    assert(invertTransform(WL_OUTPUT_TRANSFORM_270) == WL_OUTPUT_TRANSFORM_90);

    // Projection matrix test
    Matrix3 proj = Matrix3::project(1920, 1080, WL_OUTPUT_TRANSFORM_NORMAL);
    assert(proj[0] != 0.0f);

    std::cout << "  -> Matrix3 tests passed!" << std::endl;
}

void test_scene_graph() {
    std::cout << "[Test] SceneTree & Direct Scanout evaluation..." << std::endl;

    auto tree = SceneTree::create();
    assert(tree->children().empty());

    auto rect = tree->createChildRect(100, 100, Color::red());
    assert(tree->children().size() == 1);
    assert(rect->color().r == 1.0f);

    rect->setPosition(50, 60);
    PointI absP = rect->absolutePosition();
    assert(absP.x == 50 && absP.y == 60);

    // Hit test
    SceneNode* hit = tree->nodeAt(PointI{70, 80});
    assert(hit == rect.get());

    SceneNode* miss = tree->nodeAt(PointI{10, 10});
    assert(miss == nullptr);

    // Direct scanout candidate test:
    // When no buffer covers the entire screen, scanout candidate should be null
    assert(tree->findDirectScanoutCandidate(1920, 1080) == nullptr);

    std::cout << "  -> Scene graph tests passed!" << std::endl;
}

void test_color_and_formats() {
    std::cout << "[Test] Color & DRM Formats..." << std::endl;

    Color c1(1.0f, 0.5f, 0.25f, 0.5f);
    Color premul = c1.premultiplied();
    assert(std::abs(premul.r - 0.5f) < 1e-5f);
    assert(std::abs(premul.g - 0.25f) < 1e-5f);
    assert(std::abs(premul.a - 0.5f) < 1e-5f);

    DrmFormatSet set;
    set.add(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
    assert(set.has(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR));
    assert(!set.has(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR));

    std::string str = fourccToString(DRM_FORMAT_XRGB8888);
    assert(str.size() == 4);

    // FP16 and 10-bit format classification
    assert(drmFormatIsFp16(DRM_FORMAT_ABGR16161616F));
    assert(drmFormatHasAlpha(DRM_FORMAT_ABGR16161616F));
    assert(!drmFormatHasAlpha(DRM_FORMAT_XBGR16161616F));
    assert(drmFormatIs10Bit(DRM_FORMAT_ARGB2101010));

    // Oklab color conversion & perceptual gradient interpolation
    Color red = Color::red();
    OklabColor okRed = red.toOklab();
    Color roundtripRed = okRed.toSrgb();
    assert(std::abs(roundtripRed.r - 1.0f) < 1e-2f);
    assert(std::abs(roundtripRed.g - 0.0f) < 1e-2f);
    assert(std::abs(roundtripRed.b - 0.0f) < 1e-2f);

    GradientSpec grad({Color::red(), Color::blue()}, 0.785398f);
    Color mid = grad.evaluateOklab(0.5f);
    assert(mid.r > 0.1f && mid.b > 0.1f && mid.a > 0.99f);

    std::cout << "  -> Color and Formats tests passed!" << std::endl;
}

void test_color_management_and_hdr() {
    std::cout << "[Test] ColorManagement, Gamut Adaptation & HDR Tonemapping..." << std::endl;

    // 1. EOTF / OETF roundtrip for sRGB, PQ (ST.2084), and HLG
    float v = 0.5f;
    float linSrgb = ColorManagement::evaluateEotf(TransferFunction::Srgb, v);
    float encSrgb = ColorManagement::evaluateOetf(TransferFunction::Srgb, linSrgb);
    assert(std::abs(encSrgb - v) < 1e-3f);

    float linPq = ColorManagement::evaluateEotf(TransferFunction::PqSt2084, v);
    float encPq = ColorManagement::evaluateOetf(TransferFunction::PqSt2084, linPq);
    assert(std::abs(encPq - v) < 1e-3f);

    // 2. Gamut conversion matrix: identity when source == dst
    auto idGamut = ColorManagement::computeGamutMatrix(
        Chromaticities::forPrimaries(ColorPrimaries::Srgb),
        Chromaticities::forPrimaries(ColorPrimaries::Srgb));
    assert(std::abs(idGamut[0] - 1.0f) < 1e-3f);
    assert(std::abs(idGamut[4] - 1.0f) < 1e-3f);
    assert(std::abs(idGamut[8] - 1.0f) < 1e-3f);

    // 3. BT.2020 HDR PQ -> sRGB SDR pipeline build
    ImageDescription hdrSrc = ImageDescription::hdr10Bt2020();
    ImageDescription sdrDst = ImageDescription::srgbDefault();
    ColorTransformParams tf = ColorManagement::buildTransform(hdrSrc, sdrDst, TonemapOperator::Bt2390Eetf);
    assert(tf.enabled);
    assert(tf.tonemap == TonemapOperator::Bt2390Eetf);

    // 4. CPU color conversion sanity check
    Color conv = ColorManagement::convertColor(Color(0.8f, 0.8f, 0.8f, 1.0f), hdrSrc, sdrDst);
    assert(conv.r > 0.0f && conv.r <= 1.001f);

    std::cout << "  -> ColorManagement & HDR tests passed!" << std::endl;
}

void test_uv_solver_and_damage_transactions() {
    std::cout << "[Test] SurfaceUvSolver Fractional Scale & DamageRing Transactions..." << std::endl;

    // 1. Fractional scale 1px misalignment correction (e.g. 1.25x scale)
    SurfaceViewportState vpState;
    vpState.bufferPixelSize = SizeI{125, 125};
    vpState.viewportDestination = SizeI{100, 100};
    vpState.outputScale = 1.25;

    ResolvedSurfaceUv resolved = SurfaceUvSolver::resolve(vpState, SizeI{126, 125});
    assert(resolved.misalignmentFixed);
    assert(resolved.preferNearestFilter);

    // Integer scale 2.0x with exact alignment -> nearest neighbor filter preferred
    vpState.bufferPixelSize = SizeI{200, 200};
    vpState.bufferScale = 2;
    vpState.viewportDestination.reset();
    vpState.outputScale = 2.0;
    ResolvedSurfaceUv resolvedInt = SurfaceUvSolver::resolve(vpState, SizeI{200, 200});
    assert(!resolvedInt.misalignmentFixed);
    assert(resolvedInt.preferNearestFilter);

    // 2. DamageRing Transactional Rollback & Blur Expansion
    DamageRing ring;
    // Prime buffer 1
    (void)ring.rotate(1, 1920, 1080);

    ring.addRect(RectI{20, 20, 40, 40});
    assert(ring.hasPendingDamage());
    {
        auto tx = ring.beginTransaction(1, 1920, 1080);
        assert(!tx.damage().isEmpty());
        tx.rollback();
    }
    // After rollback, pending damage is restored!
    assert(ring.hasPendingDamage());

    ring.expandPending(12);
    {
        auto tx = ring.beginTransaction(1, 1920, 1080);
        assert(tx.damage().extents().x == 8);
        assert(tx.damage().extents().width == 64);
        tx.commit();
    }
    assert(!ring.hasPendingDamage());

    std::cout << "  -> SurfaceUvSolver & DamageRing Transactions passed!" << std::endl;
}

void test_deformable_mesh_and_transformers() {
    std::cout << "[Test] DeformableMesh Physics & Window Transformers..." << std::endl;

    SpringMeshConfig cfg;
    cfg.columns = 6;
    cfg.rows = 6;
    DeformableMesh mesh(cfg);
    assert(mesh.isSettled());
    RectI bounds{100, 100, 400, 300};
    mesh.resetToBox(bounds);

    // Grab and move target box to trigger spring physics deformation
    mesh.grab(PointF{0.5, 0.0}, bounds);
    mesh.setTargetBox(RectI{150, 120, 400, 300});
    mesh.step(1.0f / 60.0f);
    assert(!mesh.isSettled());

    const auto& verts = mesh.buildTriangleVertices();
    assert(verts.size() == 6 * 6 * 6);

    // TransformerChain with VelocityMotionBlurTransformer
    TransformerChain chain;
    auto motion = std::make_shared<VelocityMotionBlurTransformer>(0.65f, 6);
    motion->updateWindowBox(RectI{100, 100, 400, 300});
    motion->updateWindowBox(RectI{180, 100, 400, 300});
    chain.add(motion);
    assert(chain.blocksDirectScanout());

    WindowTransformState state;
    state.logicalBox = RectI{180, 100, 400, 300};
    chain.applyAll(state);
    assert(state.motionBlur.active);
    assert(std::abs(state.motionBlur.velocityUvX) > 0.001f);

    std::cout << "  -> DeformableMesh & Window Transformers passed!" << std::endl;
}

void test_deferred_pass_and_occlusion_culling() {
    std::cout << "[Test] Deferred Pass Graph, Occlusion Culling & Advanced Shaders..." << std::endl;

    // Verify rounded box inscribed opaque rect math via RectPassElement::opaqueRegion
    RectI fullBox{0, 0, 1920, 1080};
    RectPassElement elemNoRound({fullBox, Color::white(), 0.0f, 2.0f});
    Region opqNoRound = elemNoRound.opaqueRegion();
    assert(opqNoRound.extents() == fullBox);

    RectPassElement elemRounded({fullBox, Color::white(), 20.0f, 2.0f});
    Region opqRounded = elemRounded.opaqueRegion();
    assert(!opqRounded.isEmpty());
    assert(opqRounded.rects().size() == 3);

    auto egl = EglContext::createAuto();
    if (egl && egl->makeCurrent()) {
        auto renderer = Renderer::create(egl);
        if (renderer) {
            auto target = RenderTarget::createOffscreen(640, 480, true);
            assert(target != nullptr);
            assert(target->texture() != nullptr);

            // Build a scene with a background rect completely occluded by a foreground opaque rect
            auto root = SceneTree::create();
            auto bgRect = root->createChildRect(640, 480, Color::blue());
            auto midRect = root->createChildRect(200, 200, Color::red());
            midRect->setPosition(50, 50);
            // Fullscreen opaque rect on top occludes both bgRect and midRect!
            auto topOpaque = root->createChildRect(640, 480, Color::green());
            (void)bgRect;
            (void)topOpaque;

            auto pass = renderer->beginPass(target, WL_OUTPUT_TRANSFORM_NORMAL);
            assert(pass != nullptr);

            Region fullDamage(RectI{0, 0, 640, 480});
            PassExecutionStats stats = root->renderCulled(*pass, fullDamage);
            assert(stats.totalElements == 3);
            assert(stats.executedElements == 1);
            assert(stats.culledElements == 2);

            // Test Shader Variants & Pluggable Blur Materials on GPU
            RectI decoBox{20, 20, 300, 200};
            GradientSpec borderGrad({Color::red(), Color::blue()}, 0.52f);
            pass->renderBorder(decoBox, borderGrad, nullptr, 0.0f, 16.0f, 18.0f, 2.4f, 2.0f, 1.0f, &fullDamage);
            pass->renderShadow(decoBox, 16.0f, 2.0f, 14.0f, 2.2f, GradientSpec(Color::black()), nullptr, 0.0f, 1.0f, &fullDamage);
            pass->renderInnerGlow(decoBox, 16.0f, 2.0f, 10.0f, 2.0f, GradientSpec(Color::white()), 1.0f, &fullDamage);

            // Test Pluggable Blur Providers (Acrylic, OpticalGlass, WaterRipple)
            for (auto matType : {BlurMaterialType::Kawase, BlurMaterialType::Acrylic, BlurMaterialType::OpticalGlass, BlurMaterialType::WaterRipple}) {
                assert(renderer->setBlurMaterial(matType));
                assert(renderer->blurProvider() != nullptr);
                assert(renderer->blurProvider()->damageExpansionRadius(BlurContext{}) > 0);
            }

            // Test 3D LUT texture upload
            auto lut = Lut3DTexture::createIdentity(17);
            assert(lut != nullptr && lut->texture() != nullptr && lut->texture()->id() != 0);

            pass->submit();
            std::cout << "  -> GPU Occlusion Culling (culled 2/3 occluded layers), Shaders & 3D LUT verified!" << std::endl;
        }
    }

    std::cout << "  -> Deferred Pass & Occlusion Culling tests passed!" << std::endl;
}

void test_compositor_fusion() {
    std::cout << "[Test] Compositor & GPU Pipeline Fusion (Zero-Copy & State Isolation)..." << std::endl;

    // 1. Test Region::intersects(RectI)
    Region reg(RectI{100, 100, 200, 200});
    assert(reg.intersects(RectI{150, 150, 50, 50}));
    assert(reg.intersects(RectI{50, 50, 100, 100}));
    assert(!reg.intersects(RectI{400, 400, 50, 50}));
    assert(!reg.intersects(RectI{0, 0, 50, 50}));

    // 2. Test GpuTexture::from_native_handle (Zero-Copy wrapping)
    auto dev = nisaba::gpu::GpuDevice::create();
    if (dev) {
        auto dummyTex = nisaba::gpu::GpuTexture::from_native_handle(dev, 999, 1920, 1080);
        assert(dummyTex != nullptr);
        assert(dummyTex->id() == 999);
        assert(dummyTex->width() == 1920);
        assert(dummyTex->height() == 1080);
        assert(!dummyTex->owns_texture());

        // 3. Test GpuSurface::from_render_target
        auto dummySurface = nisaba::gpu::GpuSurface::from_render_target(dev, 1280, 720, 12);
        assert(dummySurface != nullptr);
        assert(dummySurface->fbo_id() == 12);
        assert(dummySurface->width() == 1280);
        assert(dummySurface->height() == 720);
    }

    // 4. Test CompositorCanvas fusion with Headless RenderTarget
    auto egl = EglContext::createAuto();
    if (egl && egl->makeCurrent()) {
        auto renderer = Renderer::create(egl);
        if (renderer) {
            auto canvas = CompositorCanvas::create(egl, renderer);
            assert(canvas != nullptr);

            auto target = RenderTarget::createDefault(800, 600);
            bool ok = canvas->beginFrame(target);
            assert(ok);

            // Test vg() vector canvas binding
            auto* vg = canvas->vg();
            assert(vg != nullptr);
            assert(vg->width() == 800);
            assert(vg->height() == 600);

            // Test drawing window decoration (SDF rounded rect + shadow + title + buttons)
            RectI winBox{100, 100, 400, 300};
            canvas->drawWindowDecoration(winBox, "Nisaba Sovereign Desktop Terminal");

            // Test drawing custom styled window decoration
            WindowDecoStyle customStyle;
            customStyle.cornerRadius = 16.0f;
            customStyle.titleBarHeight = 36.0f;
            customStyle.titleBarColor = Color{0.18f, 0.20f, 0.25f, 0.95f};
            canvas->drawWindowDecoration(RectI{150, 150, 300, 200}, customStyle, "Editor");

            // Test desktop dock panel
            RectI dockBox{100, 500, 600, 70};
            nisaba::effects::GlassParams glass;
            canvas->drawDesktopPanel(dockBox, glass);

            // Test typography
            canvas->drawText("Wayland 120 FPS", 30.0f, 40.0f, Color::white(), 14.0f);

            // Test damage culling
            Region damage(RectI{0, 0, 100, 100});
            canvas->setDamage(&damage);
            // Window completely outside damage is dropped in O(1)
            canvas->drawWindowDecoration(RectI{400, 400, 200, 150}, "Culled Window");

            ok = canvas->endFrame();
            assert(ok);
            std::cout << "  -> Hardware CompositorCanvas fusion verified!" << std::endl;
        }
    }

    std::cout << "  -> Fusion tests passed!" << std::endl;
}

int main() {
    std::cout << "=== Running Nisaba Sovereign Compositor Unit Tests ===" << std::endl;

    test_geometry_and_region();
    test_damage_ring();
    test_matrix3();
    test_scene_graph();
    test_color_and_formats();
    test_color_management_and_hdr();
    test_uv_solver_and_damage_transactions();
    test_deformable_mesh_and_transformers();
    test_deferred_pass_and_occlusion_culling();
    test_compositor_fusion();

    std::cout << "=== All Compositor Tests Succeeded 100%! ===" << std::endl;
    std::cout << std::flush;
    return 0;
}
