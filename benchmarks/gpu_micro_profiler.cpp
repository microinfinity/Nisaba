//
// Nisaba vs Google Skia (Ganesh GL) — 20-Suite GPU Micro-Profiler
// Strict 1:1 Parity • Single-Layer Micro-Benchmarking Suite
//

#include <GL/glew.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include "nisaba/backend_os/platform.hpp"
#include "nisaba/backend_os/window.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#define SK_GL 1
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkFont.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkPath.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkVertices.h"
#include "include/core/SkM44.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/gl/GrGLInterface.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#ifdef HAVE_SKOTTIE
#define SkottieProperty_DEFINED
namespace skottie { class PropertyObserver; }
#include "modules/skottie/include/Skottie.h"
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif


#include "nisaba/gpu/context.hpp"
#include "nisaba/gpu/gl3_renderer.hpp"
#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/text/paragraph_cache.hpp"
#include "nisaba/gpu/gpu_texture.hpp"
#include "nisaba/lottie/animation.hpp"
#include "nisaba/svg/svg_document.hpp"
#include "nisaba/svg/svg_cache.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/mesh/mesh.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <cmath>
#include <sstream>
#include <memory>
#include <string>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

const char* resolveFont(const char* name) {
	static char resolvedPaths[4][512];
	static int nextIdx = 0;
	char* outBuf = resolvedPaths[nextIdx++ % 4];

	const char* prefixes[] = {
		"nisaba/fonts/",
		"fonts/",
		"../fonts/",
		"../../fonts/"
	};

	for (const auto& prefix : prefixes) {
		std::snprintf(outBuf, 512, "%s%s", prefix, name);
		FILE* f = std::fopen(outBuf, "rb");
		if (f) {
			std::fclose(f);
			return outBuf;
		}
	}
	return name;
}

struct GpuMicroStat {
	std::string name;
	double nisaba_cpu_us{0.0};
	double nisaba_gpu_us{0.0};
	double nisaba_total_us{0.0};
	double skia_cpu_us{0.0};
	double skia_gpu_us{0.0};
	double skia_total_us{0.0};
};

} // namespace

int main(int argc, char** argv) {
	int iterations = 100;
	int msaaSamples = 0;
	int fromSuite = 1;
	std::string filterSubstr;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
			iterations = std::max(1, std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
			msaaSamples = std::max(0, std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
			fromSuite = std::max(1, std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
			filterSubstr = argv[++i];
		}
	}

	std::cout << "=========================================================================================================\n";
	std::cout << "          NISABA GPU vs GOOGLE SKIA (GANESH GL) — 134-SUITE GRANULAR GPU MICRO-PROFILER                  \n";
	std::cout << "                   (Strict 1:1 Parity • 1080x720 Framebuffer • " << msaaSamples << "x MSAA)                         \n";
	std::cout << "=========================================================================================================\n\n";

	auto plat_res = nisaba::backend_os::Platform::create();
	if (!plat_res.isOk()) {
		std::cerr << "Failed to initialize Nisaba native platform: " << plat_res.error().message << "\n";
		return 1;
	}
	auto platform = std::move(plat_res.value());

	const int W = 1080;
	const int H = 720;

	nisaba::backend_os::WindowConfig win_cfg;
	win_cfg.title = "Nisaba vs Skia Micro-Profiler";
	win_cfg.width = W;
	win_cfg.height = H;
	win_cfg.vsync = false;

	auto win_res = nisaba::backend_os::Window::create(*platform, win_cfg);
	if (!win_res.isOk()) {
		std::cerr << "Failed to create OpenGL window: " << win_res.error().message << "\n";
		return 1;
	}
	auto window = std::move(win_res.value());
	window->makeCurrent();

	glewExperimental = GL_TRUE;
	GLenum glewErr = glewInit();
	if (glewErr != GLEW_OK && glewErr != GLEW_ERROR_NO_GLX_DISPLAY) {
		std::cerr << "Failed to initialize GLEW: " << (const char*)glewGetErrorString(glewErr) << "\n";
		return 1;
	}
	while (glGetError() != GL_NO_ERROR) {}

	std::cout << "GPU Driver   : " << glGetString(GL_RENDERER) << "\n";
	std::cout << "OpenGL Core  : " << glGetString(GL_VERSION) << "\n";
	std::cout << "Iterations   : " << iterations << " runs per benchmark suite\n";
	std::cout << "Target Size  : " << W << "x" << H << " (" << msaaSamples << "x MSAA)\n\n";

	// 1. Initialize Nisaba GPU Context + High-Level GpuDevice / GpuSurface / GpuCanvas
	auto nisabaCtx = nisaba::gpu::createContextGL3(nisaba::gpu::CreateFlags::Antialias);
	if (!nisabaCtx) {
		std::cerr << "Failed to create Nisaba Context\n";
		return 1;
	}
	int nisabaFont = nisabaCtx->createFont("sans", resolveFont("Inter-Regular.ttf"));
	(void)nisabaFont;

	auto gpuDevice = nisaba::gpu::GpuDevice::create(nisaba::gpu::GpuBackendType::OpenGL);
	if (!gpuDevice) {
		std::cerr << "Failed to adopt OpenGL context in Nisaba GpuDevice\n";
		return 1;
	}
	std::shared_ptr<nisaba::gpu::GpuSurface> nisabaSurface;
	if (msaaSamples > 0) {
		nisabaSurface = nisaba::gpu::GpuSurface::create(gpuDevice, static_cast<uint32_t>(W), static_cast<uint32_t>(H), static_cast<uint32_t>(msaaSamples));
	} else {
		nisabaSurface = nisaba::gpu::GpuSurface::from_screen(gpuDevice, static_cast<uint32_t>(W), static_cast<uint32_t>(H), 0);
	}
	if (!nisabaSurface) {
		std::cerr << "Failed to create Nisaba GpuSurface\n";
		return 1;
	}
	auto gpuCanvas = std::make_unique<nisaba::gpu::GpuCanvas>(nisabaSurface);
	gpuCanvas->create_font("sans", resolveFont("Inter-Regular.ttf"));
	const GLuint nisabaFbo = nisabaSurface->fbo_id();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	while (glGetError() != GL_NO_ERROR) {}

	// 2. Initialize Skia GrDirectContext & Surface
	auto glInterface = GrGLMakeAssembledInterface(nullptr, [](void*, const char* name) -> GrGLFuncPtr {
		using PFN_glXGetProcAddress = void* (*)(const unsigned char*);
		static void* s_libGL = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
		static auto s_glXGetProcAddress = s_libGL ? reinterpret_cast<PFN_glXGetProcAddress>(dlsym(s_libGL, "glXGetProcAddressARB")) : nullptr;
		if (s_glXGetProcAddress) {
			if (void* proc = s_glXGetProcAddress(reinterpret_cast<const unsigned char*>(name))) {
				return reinterpret_cast<GrGLFuncPtr>(proc);
			}
		}
		if (void* proc = (void*)eglGetProcAddress(name)) {
			return reinterpret_cast<GrGLFuncPtr>(proc);
		}
		if (void* proc = dlsym(RTLD_DEFAULT, name)) {
			return reinterpret_cast<GrGLFuncPtr>(proc);
		}
		if (s_libGL) {
			if (void* proc = dlsym(s_libGL, name)) return reinterpret_cast<GrGLFuncPtr>(proc);
		}
		static void* s_libOpenGL = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_LOCAL);
		if (s_libOpenGL) {
			if (void* proc = dlsym(s_libOpenGL, name)) return reinterpret_cast<GrGLFuncPtr>(proc);
		}
		return nullptr;
	});

	if (!glInterface) {
		std::cerr << "Failed to assemble Skia GrGLInterface\n";
		return 1;
	}

	auto dContext = GrDirectContext::MakeGL(glInterface);
	if (!dContext) {
		std::cerr << "Failed to create Skia GrDirectContext\n";
		return 1;
	}

	GLint stencilBits = 8;
	glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
	if (stencilBits == 0) stencilBits = 8;

	int skiaSamples = msaaSamples;
	if (skiaSamples > 0) {
		int maxSkSamples = dContext->maxSurfaceSampleCountForColorType(kRGBA_8888_SkColorType);
		if (maxSkSamples < skiaSamples) skiaSamples = maxSkSamples;
	}
	GrGLFramebufferInfo fbInfo;
	fbInfo.fFBOID = nisabaFbo;
	fbInfo.fFormat = GL_RGBA8;
	GrBackendRenderTarget backendRT(W, H, skiaSamples, stencilBits, fbInfo);
	auto skiaSurface = SkSurface::MakeFromBackendRenderTarget(
		dContext.get(), backendRT, kBottomLeft_GrSurfaceOrigin,
		kRGBA_8888_SkColorType, nullptr, nullptr);

	if (!skiaSurface) {
		std::cerr << "Failed to create Skia Surface\n";
		return 1;
	}
	auto skiaCanvas = skiaSurface->getCanvas();
	sk_sp<SkTypeface> skiaTypeface = SkTypeface::MakeFromFile(resolveFont("Inter-Regular.ttf"));

	
	// 3. Pre-allocated 64x64 RGBA Texture for GPU Image/Pattern Benchmarks (Suites 61-68)
	std::vector<uint8_t> pattern_rgba(64 * 64 * 4);
	for (int y = 0; y < 64; ++y) {
		for (int x = 0; x < 64; ++x) {
			bool check = ((x / 8) + (y / 8)) % 2 == 0;
			int idx = (y * 64 + x) * 4;
			pattern_rgba[idx + 0] = check ? 255 : 50;
			pattern_rgba[idx + 1] = check ? 120 : 180;
			pattern_rgba[idx + 2] = check ? 60  : 240;
			pattern_rgba[idx + 3] = 255;
		}
	}
	int nisabaTex = nisabaCtx->createImageRGBA(64, 64, nisaba::gpu::ImageRepeatX | nisaba::gpu::ImageRepeatY, pattern_rgba.data());
	SkImageInfo img_info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
	SkPixmap sk_pixmap(img_info, pattern_rgba.data(), 64 * 4);
	sk_sp<SkImage> skiaImg = SkImage::MakeRasterCopy(sk_pixmap);

	std::vector<GpuMicroStat> stats;

	auto should_run_suite = [&](const std::string& name) -> bool {
		int num = std::atoi(name.c_str());
		if (num > 0 && num < fromSuite) return false;
		if (!filterSubstr.empty() && name.find(filterSubstr) == std::string::npos) return false;
		return true;
	};

	auto clear_skia_target = [&]() {
		glBindFramebuffer(GL_FRAMEBUFFER, nisabaFbo);
		glViewport(0, 0, W, H);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	};

	// Generic Benchmark Runner Function (Low-Level Context — Suites 1-100)
	auto benchmark_op = [&](const std::string& name, auto op_nisaba, auto op_skia) {
		if (!should_run_suite(name)) return;
		std::cout << "Running suite: " << name << "..." << std::flush;

		// Warmup Phase (5 iterations each)
		for (int i = 0; i < 5; ++i) {
			glBindFramebuffer(GL_FRAMEBUFFER, nisabaFbo);
			glViewport(0, 0, W, H);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			nisabaCtx->beginFrame(static_cast<float>(W), static_cast<float>(H), 1.0f);
			op_nisaba(*nisabaCtx);
			nisabaCtx->endFrame();
			glFinish();

			dContext->resetContext();
			clear_skia_target();
			op_skia(skiaCanvas);
			dContext->flush();
			dContext->submit(false);
			glFinish();
		}

		// Measure Nisaba GPU Engine (True CPU vs GPU Separation)
		double n_cpu_total = 0.0;
		double n_gpu_total = 0.0;
		double n_all_total = 0.0;

		for (int i = 0; i < iterations; ++i) {
			glBindFramebuffer(GL_FRAMEBUFFER, nisabaFbo);
			glViewport(0, 0, W, H);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

			auto t0 = std::chrono::high_resolution_clock::now();
			nisabaCtx->beginFrame(static_cast<float>(W), static_cast<float>(H), 1.0f);
			op_nisaba(*nisabaCtx);
			nisabaCtx->endFrame();
			auto t1 = std::chrono::high_resolution_clock::now();

			glFinish();
			auto t2 = std::chrono::high_resolution_clock::now();

			n_cpu_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
			n_gpu_total += std::chrono::duration<double, std::micro>(t2 - t1).count();
			n_all_total += std::chrono::duration<double, std::micro>(t2 - t0).count();
		}

		double nisaba_cpu = n_cpu_total / iterations;
		double nisaba_gpu = n_gpu_total / iterations;
		double nisaba_all = n_all_total / iterations;

		// Measure Google Skia Ganesh Engine (True CPU vs GPU Separation)
		double s_cpu_total = 0.0;
		double s_gpu_total = 0.0;
		double s_all_total = 0.0;

		dContext->resetContext();

		for (int i = 0; i < iterations; ++i) {
			clear_skia_target();

			auto t0 = std::chrono::high_resolution_clock::now();
			op_skia(skiaCanvas);
			dContext->flush();
			dContext->submit(false);
			auto t1 = std::chrono::high_resolution_clock::now();

			glFinish();
			auto t2 = std::chrono::high_resolution_clock::now();

			s_cpu_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
			s_gpu_total += std::chrono::duration<double, std::micro>(t2 - t1).count();
			s_all_total += std::chrono::duration<double, std::micro>(t2 - t0).count();
		}

		double skia_cpu = s_cpu_total / iterations;
		double skia_gpu = s_gpu_total / iterations;
		double skia_all = s_all_total / iterations;

		stats.push_back({name, nisaba_cpu, nisaba_gpu, nisaba_all, skia_cpu, skia_gpu, skia_all});
		std::cout << " Done. (Nisaba: " << std::fixed << std::setprecision(1) << nisaba_all
		          << " µs, Skia: " << skia_all << " µs)\n";
	};

	// High-Level GpuCanvas Benchmark Runner (Suites 101-134: Lottie, SVG, Glass, Shadows, Filters, Gradients, Blends, Clips, Text, 3D, Vertices, Dash, DRRect)
	auto benchmark_canvas_op = [&](const std::string& name, auto op_nisaba_canvas, auto op_skia) {
		if (!should_run_suite(name)) return;
		std::cout << "Running suite: " << name << "..." << std::flush;

		// Warmup Phase (5 iterations each)
		for (int i = 0; i < 5; ++i) {
			gpuCanvas->reset_state();
			nisabaSurface->bind();
			glViewport(0, 0, W, H);
			glClearColor(0.08f, 0.10f, 0.14f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			op_nisaba_canvas(*gpuCanvas);
			gpuCanvas->flush();
			glFinish();

			dContext->resetContext();
			clear_skia_target();
			op_skia(skiaCanvas);
			dContext->flush();
			dContext->submit(false);
			glFinish();
		}

		// Measure Nisaba GpuCanvas Engine
		double n_cpu_total = 0.0;
		double n_gpu_total = 0.0;
		double n_all_total = 0.0;

		gpuCanvas->reset_state();
		for (int i = 0; i < iterations; ++i) {
			nisabaSurface->bind();
			glViewport(0, 0, W, H);
			glClearColor(0.08f, 0.10f, 0.14f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

			auto t0 = std::chrono::high_resolution_clock::now();
			op_nisaba_canvas(*gpuCanvas);
			gpuCanvas->flush();
			auto t1 = std::chrono::high_resolution_clock::now();

			glFinish();
			auto t2 = std::chrono::high_resolution_clock::now();

			n_cpu_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
			n_gpu_total += std::chrono::duration<double, std::micro>(t2 - t1).count();
			n_all_total += std::chrono::duration<double, std::micro>(t2 - t0).count();
		}

		double nisaba_cpu = n_cpu_total / iterations;
		double nisaba_gpu = n_gpu_total / iterations;
		double nisaba_all = n_all_total / iterations;

		// Measure Google Skia Ganesh Engine
		double s_cpu_total = 0.0;
		double s_gpu_total = 0.0;
		double s_all_total = 0.0;

		dContext->resetContext();
		for (int i = 0; i < iterations; ++i) {
			clear_skia_target();

			auto t0 = std::chrono::high_resolution_clock::now();
			op_skia(skiaCanvas);
			dContext->flush();
			dContext->submit(false);
			auto t1 = std::chrono::high_resolution_clock::now();

			glFinish();
			auto t2 = std::chrono::high_resolution_clock::now();

			s_cpu_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
			s_gpu_total += std::chrono::duration<double, std::micro>(t2 - t1).count();
			s_all_total += std::chrono::duration<double, std::micro>(t2 - t0).count();
		}

		double skia_cpu = s_cpu_total / iterations;
		double skia_gpu = s_gpu_total / iterations;
		double skia_all = s_all_total / iterations;

		stats.push_back({name, nisaba_cpu, nisaba_gpu, nisaba_all, skia_cpu, skia_gpu, skia_all});
		std::cout << " Done. (Nisaba: " << std::fixed << std::setprecision(1) << nisaba_all
		          << " µs, Skia: " << skia_all << " µs)\n";
	};

	// =========================================================================
	// Suite 1: Solid Background Clear (1080x720)
	// =========================================================================
	benchmark_op("1. Solid Background Clear (1080x720)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			ctx.rect(0, 0, W, H);
			ctx.fillColor(nisaba::gpu::Color::rgba(15, 23, 42, 255));
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setColor(SkColorSetARGB(255, 15, 23, 42));
			canvas->drawRect(SkRect::MakeWH(W, H), p);
		}
	);

	// =========================================================================
	// Suite 2: 100 Grid Lines (1px Hairline Strokes)
	// =========================================================================
	benchmark_op("2. 100 Grid Lines (1px Hairlines)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			for (float x = 0; x <= W; x += W / 50.0f) {
				ctx.moveTo(x, 0);
				ctx.lineTo(x, H);
			}
			for (float y = 0; y <= H; y += H / 50.0f) {
				ctx.moveTo(0, y);
				ctx.lineTo(W, y);
			}
			ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 30));
			ctx.strokeWidth(1.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPath path;
			for (float x = 0; x <= W; x += W / 50.0f) {
				path.moveTo(x, 0);
				path.lineTo(x, H);
			}
			for (float y = 0; y <= H; y += H / 50.0f) {
				path.moveTo(0, y);
				path.lineTo(W, y);
			}
			SkPaint p;
			p.setAntiAlias(true);
			p.setStyle(SkPaint::kStroke_Style);
			p.setStrokeWidth(1.0f);
			p.setColor(SkColorSetARGB(30, 255, 255, 255));
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 3: 20 Thick Diagonal Lines (16px Strokes, Miter & Round Caps)
	// =========================================================================
	benchmark_op("3. 20 Thick Diagonal Lines (16px Round)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineCap(nisaba::gpu::LineCap::Round);
			for (int i = 0; i < 20; ++i) {
				float x0 = i * 50.0f;
				ctx.beginPath();
				ctx.moveTo(x0, 50.0f);
				ctx.lineTo(x0 + 100.0f, H - 50.0f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(0, 200, 255, 180));
				ctx.strokeWidth(16.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setStyle(SkPaint::kStroke_Style);
			p.setStrokeWidth(16.0f);
			p.setStrokeCap(SkPaint::kRound_Cap);
			p.setColor(SkColorSetARGB(180, 0, 200, 255));
			for (int i = 0; i < 20; ++i) {
				float x0 = i * 50.0f;
				canvas->drawLine(x0, 50.0f, x0 + 100.0f, H - 50.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 4: 100 Solid Rectangles (Varied Bounds)
	// =========================================================================
	benchmark_op("4. 100 Solid Rectangles (Varied Bounds)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 100; ++i) {
				float x = (i * 37) % (W - 80);
				float y = (i * 53) % (H - 60);
				ctx.beginPath();
				ctx.rect(x, y, 75.0f, 55.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(40 + (i * 2) % 200, 80 + (i * 3) % 150, 180, 255));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			for (int i = 0; i < 100; ++i) {
				float x = (i * 37) % (W - 80);
				float y = (i * 53) % (H - 60);
				p.setColor(SkColorSetARGB(255, 40 + (i * 2) % 200, 80 + (i * 3) % 150, 180));
				canvas->drawRect(SkRect::MakeXYWH(x, y, 75.0f, 55.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 5: 100 Alpha Rectangles (Porter-Duff Blend Stack)
	// =========================================================================
	benchmark_op("5. 100 Alpha Rectangles (Overlapping Blend)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 100; ++i) {
				float x = (i * 41) % (W - 120);
				float y = (i * 29) % (H - 100);
				ctx.beginPath();
				ctx.rect(x, y, 110.0f, 85.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(255, (i * 5) % 255, 100, 120));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			for (int i = 0; i < 100; ++i) {
				float x = (i * 41) % (W - 120);
				float y = (i * 29) % (H - 100);
				p.setColor(SkColorSetARGB(120, 255, (i * 5) % 255, 100));
				canvas->drawRect(SkRect::MakeXYWH(x, y, 110.0f, 85.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 6: 50 Simple Rounded Rectangles (12px Radius Cards)
	// =========================================================================
	benchmark_op("6. 50 Simple Rounded Rects (12px Radius)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				float x = (i * 67) % (W - 140);
				float y = (i * 43) % (H - 90);
				ctx.beginPath();
				ctx.roundedRect(x, y, 130.0f, 80.0f, 12.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(30, 41, 59, 235));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(235, 30, 41, 59));
			for (int i = 0; i < 50; ++i) {
				float x = (i * 67) % (W - 140);
				float y = (i * 43) % (H - 90);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 130.0f, 80.0f), 12.0f, 12.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 7: 50 Non-Uniform Rounded Rects (Varying Corner Radii)
	// =========================================================================
	benchmark_op("7. 50 Varying-Radius Rounded Rects",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				float x = (i * 71) % (W - 150);
				float y = (i * 39) % (H - 100);
				ctx.beginPath();
				ctx.roundedRectVarying(x, y, 140.0f, 90.0f, 24.0f, 4.0f, 18.0f, 8.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(15, 118, 110, 220));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(220, 15, 118, 110));
			SkVector radii[4] = { {24.0f, 24.0f}, {4.0f, 4.0f}, {18.0f, 18.0f}, {8.0f, 8.0f} };
			for (int i = 0; i < 50; ++i) {
				float x = (i * 71) % (W - 150);
				float y = (i * 39) % (H - 100);
				SkRRect rr;
				rr.setRectRadii(SkRect::MakeXYWH(x, y, 140.0f, 90.0f), radii);
				canvas->drawRRect(rr, p);
			}
		}
	);

	// =========================================================================
	// Suite 8: 50 Stroked Rounded Rects (1.5px Borders)
	// =========================================================================
	benchmark_op("8. 50 Stroked Rounded Rects (1.5px Border)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				float x = (i * 73) % (W - 140);
				float y = (i * 47) % (H - 90);
				ctx.beginPath();
				ctx.roundedRect(x + 0.5f, y + 0.5f, 130.0f - 1.0f, 80.0f - 1.0f, 12.0f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(0, 229, 255, 200));
				ctx.strokeWidth(1.5f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setStyle(SkPaint::kStroke_Style);
			p.setStrokeWidth(1.5f);
			p.setColor(SkColorSetARGB(200, 0, 229, 255));
			for (int i = 0; i < 50; ++i) {
				float x = (i * 73) % (W - 140);
				float y = (i * 47) % (H - 90);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x + 0.5f, y + 0.5f, 130.0f - 1.0f, 80.0f - 1.0f), 12.0f, 12.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 9: 100 Alpha Circles / Disks (Filled)
	// =========================================================================
	benchmark_op("9. 100 Alpha Circles / Disks (Fill)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 100; ++i) {
				float cx = 40.0f + (i * 39) % (W - 80);
				float cy = 40.0f + (i * 51) % (H - 80);
				ctx.beginPath();
				ctx.circle(cx, cy, 28.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(236, 72, 153, 160));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(160, 236, 72, 153));
			for (int i = 0; i < 100; ++i) {
				float cx = 40.0f + (i * 39) % (W - 80);
				float cy = 40.0f + (i * 51) % (H - 80);
				canvas->drawCircle(cx, cy, 28.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 10: 25 Concentric Stroked Circles (2px Stroke)
	// =========================================================================
	benchmark_op("10. 25 Concentric Rings (2px Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			float cx = W * 0.5f;
			float cy = H * 0.5f;
			for (int i = 1; i <= 25; ++i) {
				ctx.beginPath();
				ctx.circle(cx, cy, i * 13.0f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(147, 51, 234, 210));
				ctx.strokeWidth(2.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setStyle(SkPaint::kStroke_Style);
			p.setStrokeWidth(2.0f);
			p.setColor(SkColorSetARGB(210, 147, 51, 234));
			float cx = W * 0.5f;
			float cy = H * 0.5f;
			for (int i = 1; i <= 25; ++i) {
				canvas->drawCircle(cx, cy, i * 13.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 11: 50 Two-Stop Linear Gradients
	// =========================================================================
	benchmark_op("11. 50 Two-Stop Linear Gradients",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				float x = (i * 61) % (W - 150);
				float y = (i * 41) % (H - 90);
				auto paint = ctx.linearGradient(x, y, x, y + 80.0f,
					nisaba::gpu::Color::rgba(0, 160, 255, 235),
					nisaba::gpu::Color::rgba(80, 50, 230, 245));
				ctx.beginPath();
				ctx.roundedRect(x, y, 140.0f, 80.0f, 10.0f);
				ctx.fillPaint(paint);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(235, 0, 160, 255), SkColorSetARGB(245, 80, 50, 230) };
			for (int i = 0; i < 50; ++i) {
				float x = (i * 61) % (W - 150);
				float y = (i * 41) % (H - 90);
				SkPoint pts[2] = { {x, y}, {x, y + 80.0f} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 140.0f, 80.0f), 10.0f, 10.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 12: 50 Diagonal Linear Gradients (45° Angle)
	// =========================================================================
	benchmark_op("12. 50 Diagonal Gradients (45° Angle)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				float x = (i * 59) % (W - 150);
				float y = (i * 47) % (H - 90);
				auto paint = ctx.linearGradient(x, y, x + 140.0f, y + 80.0f,
					nisaba::gpu::Color::rgba(255, 107, 107, 240),
					nisaba::gpu::Color::rgba(255, 217, 61, 240));
				ctx.beginPath();
				ctx.roundedRect(x, y, 140.0f, 80.0f, 10.0f);
				ctx.fillPaint(paint);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(240, 255, 107, 107), SkColorSetARGB(240, 255, 217, 61) };
			for (int i = 0; i < 50; ++i) {
				float x = (i * 59) % (W - 150);
				float y = (i * 47) % (H - 90);
				SkPoint pts[2] = { {x, y}, {x + 140.0f, y + 80.0f} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 140.0f, 80.0f), 10.0f, 10.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 13: 25 Radial Glow Gradients (300px Radius)
	// =========================================================================
	benchmark_op("13. 25 Radial Glow Gradients (300px)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 25; ++i) {
				float cx = (i * 79) % (W - 200) + 100.0f;
				float cy = (i * 61) % (H - 200) + 100.0f;
				auto glow = ctx.radialGradient(cx, cy, 30.0f, 250.0f,
					nisaba::gpu::Color::rgba(0, 255, 200, 60),
					nisaba::gpu::Color::rgba(0, 255, 200, 0));
				ctx.beginPath();
				ctx.rect(cx - 250.0f, cy - 250.0f, 500.0f, 500.0f);
				ctx.fillPaint(glow);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(60, 0, 255, 200), SkColorSetARGB(0, 0, 255, 200) };
			for (int i = 0; i < 25; ++i) {
				float cx = (i * 79) % (W - 200) + 100.0f;
				float cy = (i * 61) % (H - 200) + 100.0f;
				p.setShader(SkGradientShader::MakeRadial(SkPoint::Make(cx, cy), 250.0f, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawRect(SkRect::MakeXYWH(cx - 250.0f, cy - 250.0f, 500.0f, 500.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 14: 20 Analytical Box Blur / Drop Shadows (15px Feather)
	// =========================================================================
	benchmark_op("14. 20 Box Blur / Drop Shadows (15px)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 20; ++i) {
				float x = (i * 97) % (W - 200);
				float y = (i * 71) % (H - 150);
				auto shadow = ctx.boxGradient(x, y + 4.0f, 180.0f, 120.0f, 14.0f, 15.0f,
					nisaba::gpu::Color::rgba(0, 0, 0, 160),
					nisaba::gpu::Color::rgba(0, 0, 0, 0));
				ctx.beginPath();
				ctx.rect(x - 15.0f, y - 15.0f, 180.0f + 30.0f, 120.0f + 30.0f + 4.0f);
				ctx.fillPaint(shadow);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(160, 0, 0, 0));
			p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 15.0f * 0.45f));
			for (int i = 0; i < 20; ++i) {
				float x = (i * 97) % (W - 200);
				float y = (i * 71) % (H - 150);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y + 4.0f, 180.0f, 120.0f), 14.0f, 14.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 15: 10 Concave 10-Point Stars (Winding Path Fill)
	// =========================================================================
	benchmark_op("15. 10 Concave 10-Point Stars (Fill)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 10; ++i) {
				float cx = 100.0f + i * 90.0f;
				float cy = 360.0f;
				ctx.beginPath();
				for (int p = 0; p < 20; ++p) {
					float r = (p % 2 == 0) ? 65.0f : 28.0f;
					float a = p * static_cast<float>(M_PI) / 10.0f;
					float px = cx + std::cos(a) * r;
					float py = cy + std::sin(a) * r;
					if (p == 0) ctx.moveTo(px, py);
					else ctx.lineTo(px, py);
				}
				ctx.closePath();
				ctx.fillColor(nisaba::gpu::Color::rgba(245, 158, 11, 230));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(230, 245, 158, 11));
			SkPath path;
			path.incReserve(22);
			for (int i = 0; i < 10; ++i) {
				float cx = 100.0f + i * 90.0f;
				float cy = 360.0f;
				path.rewind();
				for (int pt = 0; pt < 20; ++pt) {
					float r = (pt % 2 == 0) ? 65.0f : 28.0f;
					float a = pt * static_cast<float>(M_PI) / 10.0f;
					float px = cx + std::cos(a) * r;
					float py = cy + std::sin(a) * r;
					if (pt == 0) path.moveTo(px, py);
					else path.lineTo(px, py);
				}
				path.close();
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 16: 10 Concave 10-Point Stars (3px Stroke)
	// =========================================================================
	benchmark_op("16. 10 Concave 10-Point Stars (3px Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 10; ++i) {
				float cx = 100.0f + i * 90.0f;
				float cy = 360.0f;
				ctx.beginPath();
				for (int p = 0; p < 20; ++p) {
					float r = (p % 2 == 0) ? 65.0f : 28.0f;
					float a = p * static_cast<float>(M_PI) / 10.0f;
					float px = cx + std::cos(a) * r;
					float py = cy + std::sin(a) * r;
					if (p == 0) ctx.moveTo(px, py);
					else ctx.lineTo(px, py);
				}
				ctx.closePath();
				ctx.strokeColor(nisaba::gpu::Color::rgba(255, 64, 129, 240));
				ctx.strokeWidth(3.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setStyle(SkPaint::kStroke_Style);
			p.setStrokeWidth(3.0f);
			p.setColor(SkColorSetARGB(240, 255, 64, 129));
			SkPath path;
			path.incReserve(22);
			for (int i = 0; i < 10; ++i) {
				float cx = 100.0f + i * 90.0f;
				float cy = 360.0f;
				path.rewind();
				for (int pt = 0; pt < 20; ++pt) {
					float r = (pt % 2 == 0) ? 65.0f : 28.0f;
					float a = pt * static_cast<float>(M_PI) / 10.0f;
					float px = cx + std::cos(a) * r;
					float py = cy + std::sin(a) * r;
					if (pt == 0) path.moveTo(px, py);
					else path.lineTo(px, py);
				}
				path.close();
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 17: 1000-Point Sine Waveform (Dense Polyline Stroke)
	// =========================================================================
	benchmark_op("17. 1000-Point Waveform (Dense Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			for (int i = 0; i < 1000; ++i) {
				float x = (static_cast<float>(i) / 1000.0f) * W;
				float y = 360.0f + std::sin(i * 0.05f) * 140.0f + std::cos(i * 0.02f) * 60.0f;
				if (i == 0) ctx.moveTo(x, y);
				else ctx.lineTo(x, y);
			}
			ctx.strokeColor(nisaba::gpu::Color::rgba(0, 255, 180, 240));
			ctx.strokeWidth(2.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPath path;
			path.incReserve(1005);
			for (int i = 0; i < 1000; ++i) {
				float x = (static_cast<float>(i) / 1000.0f) * W;
				float y = 360.0f + std::sin(i * 0.05f) * 140.0f + std::cos(i * 0.02f) * 60.0f;
				if (i == 0) path.moveTo(x, y);
				else path.lineTo(x, y);
			}
			SkPaint p;
			p.setAntiAlias(true);
			p.setStyle(SkPaint::kStroke_Style);
			p.setStrokeWidth(2.0f);
			p.setColor(SkColorSetARGB(240, 0, 255, 180));
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 18: 50 Rotated & Scaled Cards (Affine Matrix Transforms)
	// =========================================================================
	benchmark_op("18. 50 Rotated Cards (Affine Transforms)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				ctx.save();
				float cx = (i * 73) % (W - 120) + 60.0f;
				float cy = (i * 47) % (H - 120) + 60.0f;
				ctx.translate(cx, cy);
				ctx.rotate(i * 0.12f);
				ctx.scale(0.8f + (i % 5) * 0.1f, 0.8f + (i % 5) * 0.1f);
				ctx.beginPath();
				ctx.roundedRect(-45.0f, -30.0f, 90.0f, 60.0f, 8.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(99, 102, 241, 200));
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 99, 102, 241));
			for (int i = 0; i < 50; ++i) {
				canvas->save();
				float cx = (i * 73) % (W - 120) + 60.0f;
				float cy = (i * 47) % (H - 120) + 60.0f;
				canvas->translate(cx, cy);
				canvas->rotate(i * 0.12f * (180.0f / static_cast<float>(M_PI)));
				canvas->scale(0.8f + (i % 5) * 0.1f, 0.8f + (i % 5) * 0.1f);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(-45.0f, -30.0f, 90.0f, 60.0f), 8.0f, 8.0f), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 19: 25 Scissor Clipped Viewports (Nested Clipping)
	// =========================================================================
	benchmark_op("19. 25 Scissor Clipped Viewports",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 25; ++i) {
				ctx.save();
				float x = (i * 91) % (W - 160);
				float y = (i * 63) % (H - 120);
				ctx.scissor(x, y, 140.0f, 90.0f);
				// Draw oversized inner shape that must be clipped
				ctx.beginPath();
				ctx.circle(x + 70.0f, y + 45.0f, 65.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(16, 185, 129, 220));
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(220, 16, 185, 129));
			for (int i = 0; i < 25; ++i) {
				canvas->save();
				float x = (i * 91) % (W - 160);
				float y = (i * 63) % (H - 120);
				canvas->clipRect(SkRect::MakeXYWH(x, y, 140.0f, 90.0f));
				canvas->drawCircle(x + 70.0f, y + 45.0f, 65.0f, p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 20: 50 UI Text Labels (Inter-Regular Font)
	// =========================================================================
	static const char* labels[] = {
		"7", "8", "9", "×", "sin", "cos",
		"4", "5", "6", "-", "ln", "log",
		"1", "2", "3", "+", "tan", "sqrt",
		"0", ".", "=", "÷", "MR", "M+",
		"Calculate", "Result: 425.8", "Performance [V]", "Options", "History Tape"
	};
	constexpr int numLabels = sizeof(labels) / sizeof(labels[0]);

	// Pre-compute text centering metrics for Skia to match Nisaba's native cached alignment
	SkFont skiaFont(skiaTypeface, 16.0f);
	skiaFont.setEdging(SkFont::Edging::kSubpixelAntiAlias);
	struct LabelOffset { float dx; float dy; };
	std::vector<LabelOffset> labelOffsets(numLabels);
	for (int i = 0; i < numLabels; ++i) {
		SkRect b;
		skiaFont.measureText(labels[i], std::strlen(labels[i]), SkTextEncoding::kUTF8, &b);
		labelOffsets[i] = { -b.centerX(), -b.centerY() };
	}

	benchmark_op("20. 50 UI Text Labels (Inter-Regular)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fontFaceId(nisabaFont); // Directly use Sovereign Font ID (0 string map lookups)
			ctx.fontSize(16.0f);
			ctx.textAlign(nisaba::gpu::Align::Center | nisaba::gpu::Align::Middle);
			ctx.fillColor(nisaba::gpu::Color::rgba(255, 255, 255, 240));
			for (int i = 0; i < 50; ++i) {
				float x = (i * 59) % (W - 100) + 50.0f;
				float y = (i * 37) % (H - 60) + 30.0f;
				ctx.text(x, y, labels[i % numLabels]);
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setColor(SkColorSetARGB(240, 255, 255, 255));
			p.setAntiAlias(true);
			for (int i = 0; i < 50; ++i) {
				float x = (i * 59) % (W - 100) + 50.0f;
				float y = (i * 37) % (H - 60) + 30.0f;
				int idx = i % numLabels;
				canvas->drawString(labels[idx], x + labelOffsets[idx].dx, y + labelOffsets[idx].dy, skiaFont, p);
			}
		}
	);

	// =========================================================================
	

	// =========================================================================
	// Suite 21: 25 Quadratic Bezier Curves (Fill + Stroke)
	// =========================================================================
	benchmark_op("21. 25 Quadratic Bezier Curves (Fill+Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 25; ++i) {
				float x0 = 80.0f + (i % 5) * 190.0f;
				float y0 = 60.0f + (i / 5) * 130.0f;
				ctx.beginPath();
				ctx.moveTo(x0, y0 + 60.0f);
				ctx.quadTo(x0 + 75.0f, y0 - 30.0f, x0 + 150.0f, y0 + 60.0f);
				ctx.closePath();
				ctx.fillColor(nisaba::gpu::Color::rgba(244, 63, 94, 200));
				ctx.fill();
				ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 220));
				ctx.strokeWidth(2.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint pFill, pStroke;
			pFill.setAntiAlias(true); pFill.setColor(SkColorSetARGB(200, 244, 63, 94));
			pStroke.setAntiAlias(true); pStroke.setStyle(SkPaint::kStroke_Style);
			pStroke.setStrokeWidth(2.0f); pStroke.setColor(SkColorSetARGB(220, 255, 255, 255));
			SkPath path;
			for (int i = 0; i < 25; ++i) {
				float x0 = 80.0f + (i % 5) * 190.0f;
				float y0 = 60.0f + (i / 5) * 130.0f;
				path.rewind();
				path.moveTo(x0, y0 + 60.0f);
				path.quadTo(x0 + 75.0f, y0 - 30.0f, x0 + 150.0f, y0 + 60.0f);
				path.close();
				canvas->drawPath(path, pFill);
				canvas->drawPath(path, pStroke);
			}
		}
	);

	// =========================================================================
	// Suite 22: 20 Cubic Bezier Splines (Smooth S-Curves 4px Stroke)
	// =========================================================================
	benchmark_op("22. 20 Cubic Bezier Splines (4px Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(4.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(14, 165, 233, 230));
			for (int i = 0; i < 20; ++i) {
				float y = 50.0f + i * 32.0f;
				ctx.beginPath();
				ctx.moveTo(80.0f, y);
				ctx.bezierTo(350.0f, y - 40.0f, 650.0f, y + 40.0f, 1000.0f, y);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(4.0f);
			p.setColor(SkColorSetARGB(230, 14, 165, 233));
			SkPath path;
			for (int i = 0; i < 20; ++i) {
				float y = 50.0f + i * 32.0f;
				path.rewind();
				path.moveTo(80.0f, y);
				path.cubicTo(350.0f, y - 40.0f, 650.0f, y + 40.0f, 1000.0f, y);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 23: Complex Bezier Ribbon (24px Ribbon with Alpha)
	// =========================================================================
	benchmark_op("23. Complex Bezier Ribbon (24px Alpha)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			ctx.moveTo(80.0f, 120.0f);
			ctx.bezierTo(300.0f, 650.0f, 780.0f, 80.0f, 1000.0f, 600.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(234, 88, 12, 210));
			ctx.strokeWidth(24.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(24.0f);
			p.setColor(SkColorSetARGB(210, 234, 88, 12));
			SkPath path;
			path.moveTo(80.0f, 120.0f);
			path.cubicTo(300.0f, 650.0f, 780.0f, 80.0f, 1000.0f, 600.0f);
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 24: Circular Arcs with arcTo (20 Filleted Corner Segments)
	// =========================================================================
	benchmark_op("24. Circular Arcs with arcTo (20 Corners)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(3.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(168, 85, 247, 230));
			for (int i = 0; i < 20; ++i) {
				float x = 80.0f + (i % 5) * 190.0f;
				float y = 80.0f + (i / 5) * 150.0f;
				ctx.beginPath();
				ctx.moveTo(x, y);
				ctx.arcTo(x + 120.0f, y, x + 120.0f, y + 100.0f, 30.0f);
				ctx.lineTo(x + 120.0f, y + 100.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(3.0f);
			p.setColor(SkColorSetARGB(230, 168, 85, 247));
			SkPath path;
			for (int i = 0; i < 20; ++i) {
				float x = 80.0f + (i % 5) * 190.0f;
				float y = 80.0f + (i / 5) * 150.0f;
				path.rewind();
				path.moveTo(x, y);
				path.arcTo(x + 120.0f, y, x + 120.0f, y + 100.0f, 30.0f);
				path.lineTo(x + 120.0f, y + 100.0f);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 25: Directional Arcs with arc() (30 Partial Pie Arcs CW/CCW)
	// =========================================================================
	benchmark_op("25. Directional Arcs with arc() (30 Arcs)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(3.5f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(20, 184, 166, 230));
			for (int i = 0; i < 30; ++i) {
				float cx = 70.0f + (i % 6) * 170.0f;
				float cy = 70.0f + (i / 6) * 125.0f;
				float a0 = i * 0.2f;
				float a1 = a0 + 2.5f;
				ctx.beginPath();
				ctx.arc(cx, cy, 35.0f, a0, a1, (i % 2 == 0) ? nisaba::gpu::Winding::CounterClockwise : nisaba::gpu::Winding::Clockwise);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(3.5f);
			p.setColor(SkColorSetARGB(230, 20, 184, 166));
			SkPath path;
			for (int i = 0; i < 30; ++i) {
				float cx = 70.0f + (i % 6) * 170.0f;
				float cy = 70.0f + (i / 6) * 125.0f;
				float a0 = i * 0.2f;
				float a1 = a0 + 2.5f;
				float startDeg = a0 * 180.0f / static_cast<float>(M_PI);
				float sweepDeg = (a1 - a0) * 180.0f / static_cast<float>(M_PI);
				if (i % 2 != 0) sweepDeg = -sweepDeg;
				path.rewind();
				path.addArc(SkRect::MakeXYWH(cx - 35.0f, cy - 35.0f, 70.0f, 70.0f), startDeg, sweepDeg);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 26: Compound Path with Holes (Donut Shape CCW Outer + CW Hole)
	// =========================================================================
	benchmark_op("26. Compound Path with Holes (15 Donuts)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(139, 92, 246, 220));
			for (int i = 0; i < 15; ++i) {
				float cx = 110.0f + (i % 5) * 200.0f;
				float cy = 120.0f + (i / 5) * 180.0f;
				ctx.beginPath();
				ctx.circle(cx, cy, 55.0f);
				ctx.pathWinding(nisaba::gpu::Winding::Hole);
				ctx.circle(cx, cy, 25.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(220, 139, 92, 246));
			SkPath path;
			for (int i = 0; i < 15; ++i) {
				float cx = 110.0f + (i % 5) * 200.0f;
				float cy = 120.0f + (i / 5) * 180.0f;
				path.rewind();
				path.setFillType(SkPathFillType::kEvenOdd);
				path.addCircle(cx, cy, 55.0f, SkPathDirection::kCCW);
				path.addCircle(cx, cy, 25.0f, SkPathDirection::kCW);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 27: Trefoil Figure-8 Cubic Knot (Closed Self-Intersecting Loop)
	// =========================================================================
	benchmark_op("27. Trefoil Figure-8 Cubic Knot (8 Loops)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(5.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(236, 72, 153, 240));
			for (int i = 0; i < 8; ++i) {
				float cx = 140.0f + (i % 4) * 250.0f;
				float cy = 200.0f + (i / 4) * 280.0f;
				ctx.beginPath();
				ctx.moveTo(cx, cy);
				ctx.bezierTo(cx + 100.0f, cy - 100.0f, cx + 100.0f, cy + 100.0f, cx, cy);
				ctx.bezierTo(cx - 100.0f, cy - 100.0f, cx - 100.0f, cy + 100.0f, cx, cy);
				ctx.closePath();
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(5.0f);
			p.setColor(SkColorSetARGB(240, 236, 72, 153));
			SkPath path;
			for (int i = 0; i < 8; ++i) {
				float cx = 140.0f + (i % 4) * 250.0f;
				float cy = 200.0f + (i / 4) * 280.0f;
				path.rewind();
				path.moveTo(cx, cy);
				path.cubicTo(cx + 100.0f, cy - 100.0f, cx + 100.0f, cy + 100.0f, cx, cy);
				path.cubicTo(cx - 100.0f, cy - 100.0f, cx - 100.0f, cy + 100.0f, cx, cy);
				path.close();
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 28: Archimedean Spiral Polyline (500 Points Spiral Stroke)
	// =========================================================================
	benchmark_op("28. Archimedean Spiral Polyline (500 Pts)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 0; i < 500; ++i) {
				float a = i * 0.08f;
				float r = a * 8.0f;
				float px = cx + std::cos(a) * r;
				float py = cy + std::sin(a) * r;
				if (i == 0) ctx.moveTo(px, py);
				else ctx.lineTo(px, py);
			}
			ctx.strokeColor(nisaba::gpu::Color::rgba(251, 191, 36, 230));
			ctx.strokeWidth(2.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPath path;
			path.incReserve(505);
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 0; i < 500; ++i) {
				float a = i * 0.08f;
				float r = a * 8.0f;
				float px = cx + std::cos(a) * r;
				float py = cy + std::sin(a) * r;
				if (i == 0) path.moveTo(px, py);
				else path.lineTo(px, py);
			}
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(2.0f);
			p.setColor(SkColorSetARGB(230, 251, 191, 36));
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 29: Multi-Contour Disjoint Paths (25 Sub-Paths in 1 Batch)
	// =========================================================================
	benchmark_op("29. Multi-Contour Disjoint Paths (25 Sub-Paths)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			for (int i = 0; i < 25; ++i) {
				float cx = 70.0f + (i % 5) * 200.0f;
				float cy = 70.0f + (i / 5) * 135.0f;
				ctx.circle(cx, cy, 30.0f);
			}
			ctx.fillColor(nisaba::gpu::Color::rgba(34, 197, 94, 210));
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPath path;
			for (int i = 0; i < 25; ++i) {
				float cx = 70.0f + (i % 5) * 200.0f;
				float cy = 70.0f + (i / 5) * 135.0f;
				path.addCircle(cx, cy, 30.0f);
			}
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 34, 197, 94));
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 30: Sharp Zigzag Mountain Polygon (50 Vertices Fill + Stroke)
	// =========================================================================
	benchmark_op("30. Sharp Zigzag Mountain (50 Vertices)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.beginPath();
			ctx.moveTo(40.0f, 600.0f);
			for (int i = 0; i < 48; ++i) {
				float x = 40.0f + (i + 1) * 20.0f;
				float y = (i % 2 == 0) ? 200.0f : 450.0f;
				ctx.lineTo(x, y);
			}
			ctx.lineTo(1040.0f, 600.0f);
			ctx.closePath();
			ctx.fillColor(nisaba::gpu::Color::rgba(59, 130, 246, 180));
			ctx.fill();
			ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 220));
			ctx.strokeWidth(2.5f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPath path;
			path.moveTo(40.0f, 600.0f);
			for (int i = 0; i < 48; ++i) {
				float x = 40.0f + (i + 1) * 20.0f;
				float y = (i % 2 == 0) ? 200.0f : 450.0f;
				path.lineTo(x, y);
			}
			path.lineTo(1040.0f, 600.0f);
			path.close();
			SkPaint pFill, pStroke;
			pFill.setAntiAlias(true); pFill.setColor(SkColorSetARGB(180, 59, 130, 246));
			pStroke.setAntiAlias(true); pStroke.setStyle(SkPaint::kStroke_Style); pStroke.setStrokeWidth(2.5f); pStroke.setColor(SkColorSetARGB(220, 255, 255, 255));
			canvas->drawPath(path, pFill);
			canvas->drawPath(path, pStroke);
		}
	);

	// =========================================================================
	// Suite 31: Stroke Join: Miter (Acute Zigzag Join 12px)
	// =========================================================================
	benchmark_op("31. Stroke Join: Miter (12px Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineJoin(nisaba::gpu::LineJoin::Miter);
			ctx.strokeWidth(12.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(239, 68, 68, 220));
			ctx.beginPath();
			ctx.moveTo(80.0f, 150.0f);
			for (int i = 0; i < 12; ++i) {
				ctx.lineTo(80.0f + (i + 1) * 75.0f, (i % 2 == 0) ? 550.0f : 150.0f);
			}
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(12.0f);
			p.setStrokeJoin(SkPaint::kMiter_Join); p.setColor(SkColorSetARGB(220, 239, 68, 68));
			SkPath path;
			path.moveTo(80.0f, 150.0f);
			for (int i = 0; i < 12; ++i) {
				path.lineTo(80.0f + (i + 1) * 75.0f, (i % 2 == 0) ? 550.0f : 150.0f);
			}
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 32: Stroke Join: Bevel (Acute Zigzag Join 12px)
	// =========================================================================
	benchmark_op("32. Stroke Join: Bevel (12px Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineJoin(nisaba::gpu::LineJoin::Bevel);
			ctx.strokeWidth(12.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(245, 158, 11, 220));
			ctx.beginPath();
			ctx.moveTo(80.0f, 150.0f);
			for (int i = 0; i < 12; ++i) {
				ctx.lineTo(80.0f + (i + 1) * 75.0f, (i % 2 == 0) ? 550.0f : 150.0f);
			}
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(12.0f);
			p.setStrokeJoin(SkPaint::kBevel_Join); p.setColor(SkColorSetARGB(220, 245, 158, 11));
			SkPath path;
			path.moveTo(80.0f, 150.0f);
			for (int i = 0; i < 12; ++i) {
				path.lineTo(80.0f + (i + 1) * 75.0f, (i % 2 == 0) ? 550.0f : 150.0f);
			}
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 33: Stroke Join: Round (Acute Zigzag Join 12px)
	// =========================================================================
	benchmark_op("33. Stroke Join: Round (12px Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineJoin(nisaba::gpu::LineJoin::Round);
			ctx.strokeWidth(12.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(16, 185, 129, 220));
			ctx.beginPath();
			ctx.moveTo(80.0f, 150.0f);
			for (int i = 0; i < 12; ++i) {
				ctx.lineTo(80.0f + (i + 1) * 75.0f, (i % 2 == 0) ? 550.0f : 150.0f);
			}
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(12.0f);
			p.setStrokeJoin(SkPaint::kRound_Join); p.setColor(SkColorSetARGB(220, 16, 185, 129));
			SkPath path;
			path.moveTo(80.0f, 150.0f);
			for (int i = 0; i < 12; ++i) {
				path.lineTo(80.0f + (i + 1) * 75.0f, (i % 2 == 0) ? 550.0f : 150.0f);
			}
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 34: Stroke Cap: Butt (30 Segment Strokes 12px)
	// =========================================================================
	benchmark_op("34. Stroke Cap: Butt (30 Segments 12px)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineCap(nisaba::gpu::LineCap::Butt);
			ctx.strokeWidth(12.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(59, 130, 246, 220));
			for (int i = 0; i < 30; ++i) {
				float y = 50.0f + i * 21.0f;
				ctx.beginPath();
				ctx.moveTo(80.0f, y);
				ctx.lineTo(1000.0f, y);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(12.0f);
			p.setStrokeCap(SkPaint::kButt_Cap); p.setColor(SkColorSetARGB(220, 59, 130, 246));
			for (int i = 0; i < 30; ++i) {
				float y = 50.0f + i * 21.0f;
				canvas->drawLine(80.0f, y, 1000.0f, y, p);
			}
		}
	);

	// =========================================================================
	// Suite 35: Stroke Cap: Square (30 Segment Strokes 12px)
	// =========================================================================
	benchmark_op("35. Stroke Cap: Square (30 Segments 12px)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineCap(nisaba::gpu::LineCap::Square);
			ctx.strokeWidth(12.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(99, 102, 241, 220));
			for (int i = 0; i < 30; ++i) {
				float y = 50.0f + i * 21.0f;
				ctx.beginPath();
				ctx.moveTo(80.0f, y);
				ctx.lineTo(1000.0f, y);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(12.0f);
			p.setStrokeCap(SkPaint::kSquare_Cap); p.setColor(SkColorSetARGB(220, 99, 102, 241));
			for (int i = 0; i < 30; ++i) {
				float y = 50.0f + i * 21.0f;
				canvas->drawLine(80.0f, y, 1000.0f, y, p);
			}
		}
	);

	// =========================================================================
	// Suite 36: Stroke Cap: Round (30 Segment Strokes 12px)
	// =========================================================================
	benchmark_op("36. Stroke Cap: Round (30 Segments 12px)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineCap(nisaba::gpu::LineCap::Round);
			ctx.strokeWidth(12.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(168, 85, 247, 220));
			for (int i = 0; i < 30; ++i) {
				float y = 50.0f + i * 21.0f;
				ctx.beginPath();
				ctx.moveTo(80.0f, y);
				ctx.lineTo(1000.0f, y);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(12.0f);
			p.setStrokeCap(SkPaint::kRound_Cap); p.setColor(SkColorSetARGB(220, 168, 85, 247));
			for (int i = 0; i < 30; ++i) {
				float y = 50.0f + i * 21.0f;
				canvas->drawLine(80.0f, y, 1000.0f, y, p);
			}
		}
	);

	// =========================================================================
	// Suite 37: Miter Limit Clamping (Sharp 10° Acute Spikes 8px Stroke)
	// =========================================================================
	benchmark_op("37. Miter Limit Clamping (10° Acute Spikes)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.miterLimit(3.0f);
			ctx.strokeWidth(8.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(236, 72, 153, 230));
			for (int i = 0; i < 15; ++i) {
				float x = 60.0f + i * 65.0f;
				ctx.beginPath();
				ctx.moveTo(x, 600.0f);
				ctx.lineTo(x + 10.0f, 100.0f);
				ctx.lineTo(x + 20.0f, 600.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(8.0f);
			p.setStrokeMiter(3.0f); p.setColor(SkColorSetARGB(230, 236, 72, 153));
			SkPath path;
			for (int i = 0; i < 15; ++i) {
				float x = 60.0f + i * 65.0f;
				path.rewind();
				path.moveTo(x, 600.0f);
				path.lineTo(x + 10.0f, 100.0f);
				path.lineTo(x + 20.0f, 600.0f);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 38: Sub-Pixel Hairline Strokes (0.25px Fine Lines)
	// =========================================================================
	benchmark_op("38. Sub-Pixel Hairline Strokes (0.25px Lines)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(0.25f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 180));
			for (int i = 0; i < 60; ++i) {
				ctx.beginPath();
				ctx.moveTo(40.0f, 40.0f + i * 11.0f);
				ctx.lineTo(1040.0f, 40.0f + i * 11.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(0.25f);
			p.setColor(SkColorSetARGB(180, 255, 255, 255));
			for (int i = 0; i < 60; ++i) {
				canvas->drawLine(40.0f, 40.0f + i * 11.0f, 1040.0f, 40.0f + i * 11.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 39: Ultra-Heavy Geometric Ribbon (40px Wide Segment Stroke)
	// =========================================================================
	benchmark_op("39. Ultra-Heavy Geometric Ribbon (40px Wide)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineCap(nisaba::gpu::LineCap::Round);
			ctx.lineJoin(nisaba::gpu::LineJoin::Round);
			ctx.strokeWidth(40.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(14, 165, 233, 190));
			ctx.beginPath();
			ctx.moveTo(100.0f, 200.0f);
			ctx.lineTo(540.0f, 550.0f);
			ctx.lineTo(980.0f, 200.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(40.0f);
			p.setStrokeCap(SkPaint::kRound_Cap); p.setStrokeJoin(SkPaint::kRound_Join);
			p.setColor(SkColorSetARGB(190, 14, 165, 233));
			SkPath path;
			path.moveTo(100.0f, 200.0f);
			path.lineTo(540.0f, 550.0f);
			path.lineTo(980.0f, 200.0f);
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 40: Stepped Variable Stroke Widths (10 Paths 1px to 20px)
	// =========================================================================
	benchmark_op("40. Stepped Variable Stroke Widths (1-20px)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeColor(nisaba::gpu::Color::rgba(244, 63, 94, 220));
			for (int i = 0; i < 10; ++i) {
				float w = 1.0f + i * 2.0f;
				float y = 80.0f + i * 58.0f;
				ctx.strokeWidth(w);
				ctx.beginPath();
				ctx.moveTo(80.0f, y);
				ctx.bezierTo(350.0f, y - 25.0f, 650.0f, y + 25.0f, 1000.0f, y);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style);
			p.setColor(SkColorSetARGB(220, 244, 63, 94));
			SkPath path;
			for (int i = 0; i < 10; ++i) {
				float w = 1.0f + i * 2.0f;
				float y = 80.0f + i * 58.0f;
				p.setStrokeWidth(w);
				path.rewind();
				path.moveTo(80.0f, y);
				path.cubicTo(350.0f, y - 25.0f, 650.0f, y + 25.0f, 1000.0f, y);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 41: 50 Ellipses (Varied Aspect Ratio rx=60, ry=25)
	// =========================================================================
	benchmark_op("41. 50 Ellipses (rx=60, ry=25 Fill)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(168, 85, 247, 200));
			for (int i = 0; i < 50; ++i) {
				float cx = 70.0f + (i * 73) % (W - 140);
				float cy = 50.0f + (i * 47) % (H - 100);
				ctx.beginPath();
				ctx.ellipse(cx, cy, 60.0f, 25.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(200, 168, 85, 247));
			for (int i = 0; i < 50; ++i) {
				float cx = 70.0f + (i * 73) % (W - 140);
				float cy = 50.0f + (i * 47) % (H - 100);
				canvas->drawOval(SkRect::MakeXYWH(cx - 60.0f, cy - 25.0f, 120.0f, 50.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 42: 50 Stroked Ellipses (2.5px Border)
	// =========================================================================
	benchmark_op("42. 50 Stroked Ellipses (2.5px Border)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(2.5f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(56, 189, 248, 220));
			for (int i = 0; i < 50; ++i) {
				float cx = 70.0f + (i * 71) % (W - 140);
				float cy = 50.0f + (i * 43) % (H - 100);
				ctx.beginPath();
				ctx.ellipse(cx, cy, 55.0f, 30.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(2.5f);
			p.setColor(SkColorSetARGB(220, 56, 189, 248));
			for (int i = 0; i < 50; ++i) {
				float cx = 70.0f + (i * 71) % (W - 140);
				float cy = 50.0f + (i * 43) % (H - 100);
				canvas->drawOval(SkRect::MakeXYWH(cx - 55.0f, cy - 30.0f, 110.0f, 60.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 43: 50 Pill / Capsule Badges (Rounded Rect r=h/2)
	// =========================================================================
	benchmark_op("43. 50 Pill / Capsule Badges (r=h/2)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(16, 185, 129, 210));
			for (int i = 0; i < 50; ++i) {
				float x = (i * 61) % (W - 130);
				float y = (i * 41) % (H - 60);
				ctx.beginPath();
				ctx.roundedRect(x, y, 120.0f, 44.0f, 22.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 16, 185, 129));
			for (int i = 0; i < 50; ++i) {
				float x = (i * 61) % (W - 130);
				float y = (i * 41) % (H - 60);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 120.0f, 44.0f), 22.0f, 22.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 44: 50 Asymmetric Teardrop Rounded Rects (Varying Radii)
	// =========================================================================
	benchmark_op("44. 50 Asymmetric Teardrop Rounded Rects",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(245, 158, 11, 220));
			for (int i = 0; i < 50; ++i) {
				float x = (i * 67) % (W - 110);
				float y = (i * 47) % (H - 110);
				ctx.beginPath();
				ctx.roundedRectVarying(x, y, 90.0f, 90.0f, 45.0f, 45.0f, 45.0f, 4.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(220, 245, 158, 11));
			SkVector radii[4] = { {45.0f, 45.0f}, {45.0f, 45.0f}, {45.0f, 45.0f}, {4.0f, 4.0f} };
			for (int i = 0; i < 50; ++i) {
				float x = (i * 67) % (W - 110);
				float y = (i * 47) % (H - 110);
				SkRRect rr;
				rr.setRectRadii(SkRect::MakeXYWH(x, y, 90.0f, 90.0f), radii);
				canvas->drawRRect(rr, p);
			}
		}
	);

	// =========================================================================
	// Suite 45: High-Density Disks Cloud (500 Small Filled Disks 8px)
	// =========================================================================
	benchmark_op("45. High-Density Disks Cloud (500 Disks)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(255, 255, 255, 160));
			for (int i = 0; i < 500; ++i) {
				float cx = (i * 37) % (W - 40) + 20.0f;
				float cy = (i * 53) % (H - 40) + 20.0f;
				ctx.beginPath();
				ctx.circle(cx, cy, 8.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(160, 255, 255, 255));
			for (int i = 0; i < 500; ++i) {
				float cx = (i * 37) % (W - 40) + 20.0f;
				float cy = (i * 53) % (H - 40) + 20.0f;
				canvas->drawCircle(cx, cy, 8.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 46: High-Density Stroked Rings (250 Small Stroked Rings 1.5px)
	// =========================================================================
	benchmark_op("46. High-Density Stroked Rings (250 Rings)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(1.5f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(234, 179, 8, 200));
			for (int i = 0; i < 250; ++i) {
				float cx = (i * 43) % (W - 50) + 25.0f;
				float cy = (i * 31) % (H - 50) + 25.0f;
				ctx.beginPath();
				ctx.circle(cx, cy, 14.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(1.5f);
			p.setColor(SkColorSetARGB(200, 234, 179, 8));
			for (int i = 0; i < 250; ++i) {
				float cx = (i * 43) % (W - 50) + 25.0f;
				float cy = (i * 31) % (H - 50) + 25.0f;
				canvas->drawCircle(cx, cy, 14.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 47: 250 Axis-Aligned Filled Rectangles Batch
	// =========================================================================
	benchmark_op("47. 250 Filled Rectangles Batch",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(59, 130, 246, 170));
			for (int i = 0; i < 250; ++i) {
				float x = (i * 41) % (W - 60);
				float y = (i * 29) % (H - 60);
				ctx.beginPath();
				ctx.rect(x, y, 48.0f, 36.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(170, 59, 130, 246));
			for (int i = 0; i < 250; ++i) {
				float x = (i * 41) % (W - 60);
				float y = (i * 29) % (H - 60);
				canvas->drawRect(SkRect::MakeXYWH(x, y, 48.0f, 36.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 48: 150 Axis-Aligned Stroked Rectangles (2px Border)
	// =========================================================================
	benchmark_op("48. 150 Stroked Rectangles (2px Border)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(2.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(236, 72, 153, 200));
			for (int i = 0; i < 150; ++i) {
				float x = (i * 47) % (W - 80);
				float y = (i * 37) % (H - 80);
				ctx.beginPath();
				ctx.rect(x, y, 64.0f, 48.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(2.0f);
			p.setColor(SkColorSetARGB(200, 236, 72, 153));
			for (int i = 0; i < 150; ++i) {
				float x = (i * 47) % (W - 80);
				float y = (i * 37) % (H - 80);
				canvas->drawRect(SkRect::MakeXYWH(x, y, 64.0f, 48.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 49: 40 Crosshair Aim Reticles (Circle + Crossed Lines)
	// =========================================================================
	benchmark_op("49. 40 Crosshair Aim Reticles",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(1.5f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(34, 197, 94, 220));
			for (int i = 0; i < 40; ++i) {
				float cx = 60.0f + (i % 8) * 125.0f;
				float cy = 60.0f + (i / 8) * 135.0f;
				ctx.beginPath();
				ctx.circle(cx, cy, 24.0f);
				ctx.moveTo(cx - 32.0f, cy); ctx.lineTo(cx + 32.0f, cy);
				ctx.moveTo(cx, cy - 32.0f); ctx.lineTo(cx, cy + 32.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(1.5f);
			p.setColor(SkColorSetARGB(220, 34, 197, 94));
			SkPath path;
			for (int i = 0; i < 40; ++i) {
				float cx = 60.0f + (i % 8) * 125.0f;
				float cy = 60.0f + (i / 8) * 135.0f;
				path.rewind();
				path.addCircle(cx, cy, 24.0f);
				path.moveTo(cx - 32.0f, cy); path.lineTo(cx + 32.0f, cy);
				path.moveTo(cx, cy - 32.0f); path.lineTo(cx, cy + 32.0f);
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 50: 25 Regular Octagon Polygons (8 Vertices Fill + Stroke)
	// =========================================================================
	benchmark_op("50. 25 Regular Octagons (Fill + Stroke)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 25; ++i) {
				float cx = 80.0f + (i % 5) * 190.0f;
				float cy = 80.0f + (i / 5) * 135.0f;
				ctx.beginPath();
				for (int v = 0; v < 8; ++v) {
					float a = v * static_cast<float>(M_PI) / 4.0f;
					float px = cx + std::cos(a) * 45.0f;
					float py = cy + std::sin(a) * 45.0f;
					if (v == 0) ctx.moveTo(px, py);
					else ctx.lineTo(px, py);
				}
				ctx.closePath();
				ctx.fillColor(nisaba::gpu::Color::rgba(147, 51, 234, 190));
				ctx.fill();
				ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 220));
				ctx.strokeWidth(2.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint pFill, pStroke;
			pFill.setAntiAlias(true); pFill.setColor(SkColorSetARGB(190, 147, 51, 234));
			pStroke.setAntiAlias(true); pStroke.setStyle(SkPaint::kStroke_Style); pStroke.setStrokeWidth(2.0f); pStroke.setColor(SkColorSetARGB(220, 255, 255, 255));
			SkPath path;
			for (int i = 0; i < 25; ++i) {
				float cx = 80.0f + (i % 5) * 190.0f;
				float cy = 80.0f + (i / 5) * 135.0f;
				path.rewind();
				for (int v = 0; v < 8; ++v) {
					float a = v * static_cast<float>(M_PI) / 4.0f;
					float px = cx + std::cos(a) * 45.0f;
					float py = cy + std::sin(a) * 45.0f;
					if (v == 0) path.moveTo(px, py);
					else path.lineTo(px, py);
				}
				path.close();
				canvas->drawPath(path, pFill);
				canvas->drawPath(path, pStroke);
			}
		}
	);

	// =========================================================================
	// Suite 51: 30 Vertical Card Linear Gradients
	// =========================================================================
	benchmark_op("51. 30 Vertical Card Linear Gradients",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 30; ++i) {
				float x = 60.0f + (i % 6) * 165.0f;
				float y = 50.0f + (i / 6) * 135.0f;
				auto p = ctx.linearGradient(x, y, x, y + 100.0f,
					nisaba::gpu::Color::rgba(14, 165, 233, 230),
					nisaba::gpu::Color::rgba(99, 102, 241, 230));
				ctx.beginPath();
				ctx.roundedRect(x, y, 145.0f, 100.0f, 10.0f);
				ctx.fillPaint(p);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(230, 14, 165, 233), SkColorSetARGB(230, 99, 102, 241) };
			for (int i = 0; i < 30; ++i) {
				float x = 60.0f + (i % 6) * 165.0f;
				float y = 50.0f + (i / 6) * 135.0f;
				SkPoint pts[2] = { {x, y}, {x, y + 100.0f} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 145.0f, 100.0f), 10.0f, 10.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 52: 30 Horizontal Bar Progress Gradients
	// =========================================================================
	benchmark_op("52. 30 Horizontal Bar Progress Gradients",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 30; ++i) {
				float x = 70.0f;
				float y = 40.0f + i * 22.0f;
				auto p = ctx.linearGradient(x, y, x + 940.0f, y,
					nisaba::gpu::Color::rgba(239, 68, 68, 230),
					nisaba::gpu::Color::rgba(245, 158, 11, 230));
				ctx.beginPath();
				ctx.roundedRect(x, y, 940.0f, 12.0f, 6.0f);
				ctx.fillPaint(p);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(230, 239, 68, 68), SkColorSetARGB(230, 245, 158, 11) };
			for (int i = 0; i < 30; ++i) {
				float x = 70.0f;
				float y = 40.0f + i * 22.0f;
				SkPoint pts[2] = { {x, y}, {x + 940.0f, y} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 940.0f, 12.0f), 6.0f, 6.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 53: 15 Concentric Radial Gradient Spotlights (120px)
	// =========================================================================
	benchmark_op("53. 15 Concentric Radial Spotlights",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 15; ++i) {
				float cx = 110.0f + (i % 5) * 200.0f;
				float cy = 120.0f + (i / 5) * 220.0f;
				auto p = ctx.radialGradient(cx, cy, 10.0f, 80.0f,
					nisaba::gpu::Color::rgba(244, 63, 94, 220),
					nisaba::gpu::Color::rgba(244, 63, 94, 0));
				ctx.beginPath();
				ctx.circle(cx, cy, 80.0f);
				ctx.fillPaint(p);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(220, 244, 63, 94), SkColorSetARGB(0, 244, 63, 94) };
			for (int i = 0; i < 15; ++i) {
				float cx = 110.0f + (i % 5) * 200.0f;
				float cy = 120.0f + (i / 5) * 220.0f;
				p.setShader(SkGradientShader::MakeRadial(SkPoint::Make(cx, cy), 80.0f, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawCircle(cx, cy, 80.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 54: 25 Soft Ambient Card Shadows (Box Gradient 12px)
	// =========================================================================
	benchmark_op("54. 25 Soft Ambient Card Shadows",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 25; ++i) {
				float x = 70.0f + (i % 5) * 195.0f;
				float y = 50.0f + (i / 5) * 135.0f;
				auto shadow = ctx.boxGradient(x, y + 4.0f, 150.0f, 90.0f, 10.0f, 12.0f,
					nisaba::gpu::Color::rgba(0, 0, 0, 180),
					nisaba::gpu::Color::rgba(0, 0, 0, 0));
				ctx.beginPath();
				ctx.rect(x - 12.0f, y - 12.0f, 150.0f + 24.0f, 90.0f + 24.0f + 4.0f);
				ctx.fillPaint(shadow);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(180, 0, 0, 0));
			p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 12.0f * 0.45f));
			for (int i = 0; i < 25; ++i) {
				float x = 70.0f + (i % 5) * 195.0f;
				float y = 50.0f + (i / 5) * 135.0f;
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y + 4.0f, 150.0f, 90.0f), 10.0f, 10.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 55: 15 Neon Button Glows (High-Intensity Box Gradient)
	// =========================================================================
	benchmark_op("55. 15 Neon Button Glows (Cyan Intense)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 15; ++i) {
				float x = 90.0f + (i % 5) * 190.0f;
				float y = 80.0f + (i / 5) * 200.0f;
				auto glow = ctx.boxGradient(x, y, 140.0f, 60.0f, 12.0f, 20.0f,
					nisaba::gpu::Color::rgba(6, 182, 212, 220),
					nisaba::gpu::Color::rgba(6, 182, 212, 0));
				ctx.beginPath();
				ctx.rect(x - 20.0f, y - 20.0f, 180.0f, 100.0f);
				ctx.fillPaint(glow);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(220, 6, 182, 212));
			p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 20.0f * 0.45f));
			for (int i = 0; i < 15; ++i) {
				float x = 90.0f + (i % 5) * 190.0f;
				float y = 80.0f + (i / 5) * 200.0f;
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 140.0f, 60.0f), 12.0f, 12.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 56: 16 Multi-Angle Gradient Fan Slices (Rotated Vectors)
	// =========================================================================
	benchmark_op("56. 16 Multi-Angle Gradient Fan Slices",
		[&](nisaba::gpu::Context& ctx) {
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 0; i < 16; ++i) {
				float a = i * static_cast<float>(M_PI) / 8.0f;
				float ex = cx + std::cos(a) * 250.0f;
				float ey = cy + std::sin(a) * 250.0f;
				auto p = ctx.linearGradient(cx, cy, ex, ey,
					nisaba::gpu::Color::rgba(255, 100, 50, 200),
					nisaba::gpu::Color::rgba(50, 100, 255, 200));
				ctx.beginPath();
				ctx.moveTo(cx, cy);
				ctx.arc(cx, cy, 240.0f, a - 0.18f, a + 0.18f, nisaba::gpu::Winding::CounterClockwise);
				ctx.closePath();
				ctx.fillPaint(p);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			float cx = W * 0.5f, cy = H * 0.5f;
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(200, 255, 100, 50), SkColorSetARGB(200, 50, 100, 255) };
			SkPath path;
			for (int i = 0; i < 16; ++i) {
				float a = i * static_cast<float>(M_PI) / 8.0f;
				float ex = cx + std::cos(a) * 250.0f;
				float ey = cy + std::sin(a) * 250.0f;
				SkPoint pts[2] = { {cx, cy}, {ex, ey} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				float startDeg = (a - 0.18f) * 180.0f / static_cast<float>(M_PI);
				float sweepDeg = 0.36f * 180.0f / static_cast<float>(M_PI);
				path.rewind();
				path.moveTo(cx, cy);
				path.addArc(SkRect::MakeXYWH(cx - 240.0f, cy - 240.0f, 480.0f, 480.0f), startDeg, sweepDeg);
				path.close();
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 57: Full-Screen Diagonal Horizon Gradient (1080x720)
	// =========================================================================
	benchmark_op("57. Full-Screen Diagonal Horizon Gradient",
		[&](nisaba::gpu::Context& ctx) {
			auto grad = ctx.linearGradient(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H),
				nisaba::gpu::Color::rgba(30, 27, 75, 255),
				nisaba::gpu::Color::rgba(236, 72, 153, 255));
			ctx.beginPath();
			ctx.rect(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H));
			ctx.fillPaint(grad);
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(255, 30, 27, 75), SkColorSetARGB(255, 236, 72, 153) };
			SkPoint pts[2] = { {0.0f, 0.0f}, {static_cast<float>(W), static_cast<float>(H)} };
			p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
			canvas->drawRect(SkRect::MakeWH(W, H), p);
		}
	);

	// =========================================================================
	// Suite 58: 20 Inset Well Shadows (Recessed Box Gradient)
	// =========================================================================
	benchmark_op("58. 20 Inset Well Shadows (Recessed)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 20; ++i) {
				float x = 70.0f + (i % 5) * 190.0f;
				float y = 60.0f + (i / 5) * 160.0f;
				auto inset = ctx.boxGradient(x, y + 2.0f, 150.0f, 100.0f, 8.0f, 10.0f,
					nisaba::gpu::Color::rgba(0, 0, 0, 0),
					nisaba::gpu::Color::rgba(0, 0, 0, 150));
				ctx.beginPath();
				ctx.roundedRect(x, y, 150.0f, 100.0f, 8.0f);
				ctx.fillPaint(inset);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(150, 0, 0, 0));
			p.setMaskFilter(SkMaskFilter::MakeBlur(kInner_SkBlurStyle, 10.0f * 0.45f));
			for (int i = 0; i < 20; ++i) {
				float x = 70.0f + (i % 5) * 190.0f;
				float y = 60.0f + (i / 5) * 160.0f;
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 150.0f, 100.0f), 8.0f, 8.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 59: 10 Pulsing Circular Radar Wave Glows
	// =========================================================================
	benchmark_op("59. 10 Pulsing Circular Radar Wave Glows",
		[&](nisaba::gpu::Context& ctx) {
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 1; i <= 10; ++i) {
				float r = i * 32.0f;
				auto glow = ctx.radialGradient(cx, cy, r - 15.0f, r + 15.0f,
					nisaba::gpu::Color::rgba(16, 185, 129, 180),
					nisaba::gpu::Color::rgba(16, 185, 129, 0));
				ctx.beginPath();
				ctx.circle(cx, cy, r + 15.0f);
				ctx.fillPaint(glow);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			float cx = W * 0.5f, cy = H * 0.5f;
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(180, 16, 185, 129), SkColorSetARGB(0, 16, 185, 129) };
			for (int i = 1; i <= 10; ++i) {
				float r = i * 32.0f;
				p.setShader(SkGradientShader::MakeRadial(SkPoint::Make(cx, cy), r + 15.0f, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawCircle(cx, cy, r + 15.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 60: Waveform Polyline Stroked with Linear Gradient
	// =========================================================================
	benchmark_op("60. Waveform Polyline Stroked with Gradient",
		[&](nisaba::gpu::Context& ctx) {
			auto grad = ctx.linearGradient(0.0f, 360.0f, static_cast<float>(W), 360.0f,
				nisaba::gpu::Color::rgba(244, 63, 94, 255),
				nisaba::gpu::Color::rgba(59, 130, 246, 255));
			ctx.beginPath();
			for (int i = 0; i < 500; ++i) {
				float x = (static_cast<float>(i) / 500.0f) * W;
				float y = 360.0f + std::sin(i * 0.08f) * 120.0f;
				if (i == 0) ctx.moveTo(x, y);
				else ctx.lineTo(x, y);
			}
			ctx.strokePaint(grad);
			ctx.strokeWidth(4.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPath path;
			path.incReserve(505);
			for (int i = 0; i < 500; ++i) {
				float x = (static_cast<float>(i) / 500.0f) * W;
				float y = 360.0f + std::sin(i * 0.08f) * 120.0f;
				if (i == 0) path.moveTo(x, y);
				else path.lineTo(x, y);
			}
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(4.0f);
			SkColor colors[2] = { SkColorSetARGB(255, 244, 63, 94), SkColorSetARGB(255, 59, 130, 246) };
			SkPoint pts[2] = { {0.0f, 360.0f}, {static_cast<float>(W), 360.0f} };
			p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 61: Repeated Texture Tiling (64x64 Checker Pattern)
	// =========================================================================
	benchmark_op("61. Repeated Texture Tiling (64x64 Pattern)",
		[&](nisaba::gpu::Context& ctx) {
			auto pat = ctx.imagePattern(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, nisabaTex, 1.0f);
			ctx.beginPath();
			ctx.rect(100.0f, 80.0f, 880.0f, 560.0f);
			ctx.fillPaint(pat);
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setShader(skiaImg->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions()));
			canvas->drawRect(SkRect::MakeXYWH(100.0f, 80.0f, 880.0f, 560.0f), p);
		}
	);

	// =========================================================================
	// Suite 62: Scaled Image Quad (2x Upscaled 512x512 Texture Blit)
	// =========================================================================
	benchmark_op("62. Scaled Image Quad (2x 512x512 Blit)",
		[&](nisaba::gpu::Context& ctx) {
			auto pat = ctx.imagePattern(284.0f, 114.0f, 512.0f, 512.0f, 0.0f, nisabaTex, 1.0f);
			ctx.beginPath();
			ctx.rect(284.0f, 114.0f, 512.0f, 512.0f);
			ctx.fillPaint(pat);
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			canvas->drawImageRect(skiaImg.get(), SkRect::MakeXYWH(284.0f, 114.0f, 512.0f, 512.0f), SkSamplingOptions(SkFilterMode::kLinear), &p);
		}
	);

	// =========================================================================
	// Suite 63: 45° Rotated Pattern Fill in Circle
	// =========================================================================
	benchmark_op("63. 45° Rotated Pattern Fill in Circle",
		[&](nisaba::gpu::Context& ctx) {
			auto pat = ctx.imagePattern(540.0f, 370.0f, 64.0f, 64.0f, static_cast<float>(M_PI) * 0.25f, nisabaTex, 1.0f);
			ctx.beginPath();
			ctx.circle(540.0f, 370.0f, 220.0f);
			ctx.fillPaint(pat);
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkMatrix mat;
			mat.setRotate(45.0f, 540.0f, 370.0f);
			p.setShader(skiaImg->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions(), &mat));
			canvas->drawCircle(540.0f, 370.0f, 220.0f, p);
		}
	);

	// =========================================================================
	// Suite 64: Translucent Texture Overlay (50% Alpha Blending)
	// =========================================================================
	benchmark_op("64. Translucent Texture Overlay (50% Alpha)",
		[&](nisaba::gpu::Context& ctx) {
			auto pat = ctx.imagePattern(100.0f, 80.0f, 64.0f, 64.0f, 0.0f, nisabaTex, 0.5f);
			ctx.beginPath();
			ctx.roundedRect(100.0f, 80.0f, 880.0f, 560.0f, 20.0f);
			ctx.fillPaint(pat);
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setAlphaf(0.5f);
			p.setShader(skiaImg->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions()));
			canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(100.0f, 80.0f, 880.0f, 560.0f), 20.0f, 20.0f), p);
		}
	);

	// =========================================================================
	// Suite 65: Multi-Avatar Grid (16 Rounded Texture Avatars)
	// =========================================================================
	benchmark_op("65. Multi-Avatar Grid (16 Avatars)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 16; ++i) {
				float x = 120.0f + (i % 4) * 220.0f;
				float y = 80.0f + (i / 4) * 140.0f;
				auto pat = ctx.imagePattern(x, y, 90.0f, 90.0f, 0.0f, nisabaTex, 1.0f);
				ctx.beginPath();
				ctx.roundedRect(x, y, 90.0f, 90.0f, 45.0f);
				ctx.fillPaint(pat);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			for (int i = 0; i < 16; ++i) {
				float x = 120.0f + (i % 4) * 220.0f;
				float y = 80.0f + (i / 4) * 140.0f;
				canvas->drawImageRect(skiaImg.get(), SkRect::MakeXYWH(x, y, 90.0f, 90.0f), SkSamplingOptions(), &p);
			}
		}
	);

	// =========================================================================
	// Suite 66: Texture Pattern Fill over 10-Point Star Path
	// =========================================================================
	benchmark_op("66. Texture Pattern on Star Path",
		[&](nisaba::gpu::Context& ctx) {
			auto pat = ctx.imagePattern(540.0f, 370.0f, 64.0f, 64.0f, 0.0f, nisabaTex, 1.0f);
			ctx.beginPath();
			for (int p = 0; p < 20; ++p) {
				float r = (p % 2 == 0) ? 220.0f : 100.0f;
				float a = p * static_cast<float>(M_PI) / 10.0f;
				float px = 540.0f + std::cos(a) * r;
				float py = 370.0f + std::sin(a) * r;
				if (p == 0) ctx.moveTo(px, py);
				else ctx.lineTo(px, py);
			}
			ctx.closePath();
			ctx.fillPaint(pat);
			ctx.fill();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setShader(skiaImg->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions()));
			SkPath path;
			for (int pt = 0; pt < 20; ++pt) {
				float r = (pt % 2 == 0) ? 220.0f : 100.0f;
				float a = pt * static_cast<float>(M_PI) / 10.0f;
				float px = 540.0f + std::cos(a) * r;
				float py = 370.0f + std::sin(a) * r;
				if (pt == 0) path.moveTo(px, py);
				else path.lineTo(px, py);
			}
			path.close();
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 67: Thick 20px Curved Path Stroked with Texture
	// =========================================================================
	benchmark_op("67. 20px Curved Path Stroked with Texture",
		[&](nisaba::gpu::Context& ctx) {
			auto pat = ctx.imagePattern(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, nisabaTex, 1.0f);
			ctx.beginPath();
			ctx.moveTo(100.0f, 150.0f);
			ctx.bezierTo(300.0f, 600.0f, 750.0f, 100.0f, 980.0f, 550.0f);
			ctx.strokePaint(pat);
			ctx.strokeWidth(20.0f);
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(20.0f);
			p.setShader(skiaImg->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions()));
			SkPath path;
			path.moveTo(100.0f, 150.0f);
			path.cubicTo(300.0f, 600.0f, 750.0f, 100.0f, 980.0f, 550.0f);
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 68: Minified Texture Quad (0.25x Downscale to 16x16)
	// =========================================================================
	benchmark_op("68. Minified Texture Quad (0.25x Downscale)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 50; ++i) {
				float x = 70.0f + (i * 73) % (W - 100);
				float y = 50.0f + (i * 47) % (H - 80);
				auto pat = ctx.imagePattern(x, y, 16.0f, 16.0f, 0.0f, nisabaTex, 1.0f);
				ctx.beginPath();
				ctx.rect(x, y, 16.0f, 16.0f);
				ctx.fillPaint(pat);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			for (int i = 0; i < 50; ++i) {
				float x = 70.0f + (i * 73) % (W - 100);
				float y = 50.0f + (i * 47) % (H - 80);
				canvas->drawImageRect(skiaImg.get(), SkRect::MakeXYWH(x, y, 16.0f, 16.0f), SkSamplingOptions(), &p);
			}
		}
	);

	// =========================================================================
	// Suite 69: Pure Translation Stack (50 Translated Cards)
	// =========================================================================
	benchmark_op("69. Pure Translation Stack (50 Cards)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(14, 165, 233, 210));
			for (int i = 0; i < 50; ++i) {
				ctx.save();
				ctx.translate(60.0f + (i * 71) % (W - 120), 40.0f + (i * 43) % (H - 90));
				ctx.beginPath();
				ctx.roundedRect(0.0f, 0.0f, 90.0f, 60.0f, 8.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 14, 165, 233));
			for (int i = 0; i < 50; ++i) {
				canvas->save();
				canvas->translate(60.0f + (i * 71) % (W - 120), 40.0f + (i * 43) % (H - 90));
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(0.0f, 0.0f, 90.0f, 60.0f), 8.0f, 8.0f), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 70: Pure Rotation Cluster (36 Radial Spokes at 10°)
	// =========================================================================
	benchmark_op("70. Pure Rotation Cluster (36 Spokes 10°)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(244, 63, 94, 210));
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 0; i < 36; ++i) {
				ctx.save();
				ctx.translate(cx, cy);
				ctx.rotate(i * static_cast<float>(M_PI) / 18.0f);
				ctx.beginPath();
				ctx.rect(30.0f, -5.0f, 180.0f, 10.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 244, 63, 94));
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 0; i < 36; ++i) {
				canvas->save();
				canvas->translate(cx, cy);
				canvas->rotate(i * 10.0f);
				canvas->drawRect(SkRect::MakeXYWH(30.0f, -5.0f, 180.0f, 10.0f), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 71: Pure Scale Zoom Progression (20 Scaled Rects)
	// =========================================================================
	benchmark_op("71. Pure Scale Zoom Progression (20 Rects)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(2.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(251, 191, 36, 220));
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 1; i <= 20; ++i) {
				ctx.save();
				ctx.translate(cx, cy);
				float s = 0.1f * i;
				ctx.scale(s, s);
				ctx.beginPath();
				ctx.rect(-100.0f, -60.0f, 200.0f, 120.0f);
				ctx.stroke();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(2.0f);
			p.setColor(SkColorSetARGB(220, 251, 191, 36));
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 1; i <= 20; ++i) {
				canvas->save();
				canvas->translate(cx, cy);
				float s = 0.1f * i;
				canvas->scale(s, s);
				canvas->drawRect(SkRect::MakeXYWH(-100.0f, -60.0f, 200.0f, 120.0f), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 72: Shear / Skew Parallelograms (30 Sheared Quads)
	// =========================================================================
	benchmark_op("72. Shear / Skew Parallelograms (30 Quads)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(139, 92, 246, 200));
			for (int i = 0; i < 30; ++i) {
				ctx.save();
				float x = 80.0f + (i % 6) * 160.0f;
				float y = 60.0f + (i / 6) * 130.0f;
				ctx.translate(x, y);
				ctx.skewX(0.35f);
				ctx.beginPath();
				ctx.rect(0.0f, 0.0f, 80.0f, 60.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(200, 139, 92, 246));
			for (int i = 0; i < 30; ++i) {
				canvas->save();
				float x = 80.0f + (i % 6) * 160.0f;
				float y = 60.0f + (i / 6) * 130.0f;
				canvas->translate(x, y);
				canvas->skew(0.35f, 0.0f);
				canvas->drawRect(SkRect::MakeXYWH(0.0f, 0.0f, 80.0f, 60.0f), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 73: Combined Affine Transforms (Translate+Rotate+Scale+Skew)
	// =========================================================================
	benchmark_op("73. Combined Affine Transforms (30 Shapes)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(236, 72, 153, 200));
			for (int i = 0; i < 30; ++i) {
				ctx.save();
				ctx.translate(80.0f + (i % 6) * 160.0f, 60.0f + (i / 6) * 130.0f);
				ctx.rotate(i * 0.15f);
				ctx.scale(0.85f, 0.85f);
				ctx.skewX(0.2f);
				ctx.beginPath();
				ctx.roundedRect(-35.0f, -25.0f, 70.0f, 50.0f, 8.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(200, 236, 72, 153));
			for (int i = 0; i < 30; ++i) {
				canvas->save();
				canvas->translate(80.0f + (i % 6) * 160.0f, 60.0f + (i / 6) * 130.0f);
				canvas->rotate(i * 0.15f * 180.0f / static_cast<float>(M_PI));
				canvas->scale(0.85f, 0.85f);
				canvas->skew(0.2f, 0.0f);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(-35.0f, -25.0f, 70.0f, 50.0f), 8.0f, 8.0f), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 74: Deep Hierarchical State Stack (20 Nested Levels)
	// =========================================================================
	benchmark_op("74. Deep Hierarchical State Stack (20 Levels)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(20, 184, 166, 210));
			for (int i = 0; i < 20; ++i) {
				ctx.save();
				ctx.translate(15.0f, 10.0f);
				ctx.rotate(0.04f);
				ctx.scale(0.96f, 0.96f);
				ctx.beginPath();
				ctx.rect(100.0f, 100.0f, 120.0f, 80.0f);
				ctx.fill();
			}
			for (int i = 0; i < 20; ++i) {
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 20, 184, 166));
			for (int i = 0; i < 20; ++i) {
				canvas->save();
				canvas->translate(15.0f, 10.0f);
				canvas->rotate(0.04f * 180.0f / static_cast<float>(M_PI));
				canvas->scale(0.96f, 0.96f);
				canvas->drawRect(SkRect::MakeXYWH(100.0f, 100.0f, 120.0f, 80.0f), p);
			}
			for (int i = 0; i < 20; ++i) {
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 75: Reset Transform Stress (50 Reset-and-Draw Cycles)
	// =========================================================================
	benchmark_op("75. Reset Transform Stress (50 Cycles)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(244, 63, 94, 180));
			for (int i = 0; i < 50; ++i) {
				ctx.translate(100.0f + i * 2.0f, 50.0f + i * 3.0f);
				ctx.rotate(0.2f);
				ctx.resetTransform();
				ctx.beginPath();
				ctx.rect(50.0f + (i * 19) % (W - 80), 50.0f + (i * 13) % (H - 80), 30.0f, 30.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(180, 244, 63, 94));
			for (int i = 0; i < 50; ++i) {
				canvas->translate(100.0f + i * 2.0f, 50.0f + i * 3.0f);
				canvas->rotate(0.2f * 180.0f / static_cast<float>(M_PI));
				canvas->resetMatrix();
				canvas->drawRect(SkRect::MakeXYWH(50.0f + (i * 19) % (W - 80), 50.0f + (i * 13) % (H - 80), 30.0f, 30.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 76: Planetary Orbit Hierarchy (Multi-Body Planetary Transforms)
	// =========================================================================
	benchmark_op("76. Planetary Orbit Hierarchy (10 Systems)",
		[&](nisaba::gpu::Context& ctx) {
			float cx = W * 0.5f, cy = H * 0.5f;
			ctx.fillColor(nisaba::gpu::Color::rgba(255, 255, 255, 220));
			for (int i = 0; i < 10; ++i) {
				ctx.save();
				ctx.translate(cx, cy);
				ctx.rotate(i * 0.6f);
				ctx.translate(140.0f + i * 15.0f, 0.0f);
				ctx.beginPath();
				ctx.circle(0.0f, 0.0f, 16.0f);
				ctx.fill();
				ctx.rotate(i * 1.5f);
				ctx.translate(35.0f, 0.0f);
				ctx.beginPath();
				ctx.circle(0.0f, 0.0f, 6.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(220, 255, 255, 255));
			float cx = W * 0.5f, cy = H * 0.5f;
			for (int i = 0; i < 10; ++i) {
				canvas->save();
				canvas->translate(cx, cy);
				canvas->rotate(i * 0.6f * 180.0f / static_cast<float>(M_PI));
				canvas->translate(140.0f + i * 15.0f, 0.0f);
				canvas->drawCircle(0.0f, 0.0f, 16.0f, p);
				canvas->rotate(i * 1.5f * 180.0f / static_cast<float>(M_PI));
				canvas->translate(35.0f, 0.0f);
				canvas->drawCircle(0.0f, 0.0f, 6.0f, p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 77: Multi-Scissor Grid (16 Tiled Viewport Cells)
	// =========================================================================
	benchmark_op("77. Multi-Scissor Grid (16 Viewports)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(14, 165, 233, 220));
			for (int i = 0; i < 16; ++i) {
				float cellX = 80.0f + (i % 4) * 230.0f;
				float cellY = 60.0f + (i / 4) * 155.0f;
				ctx.save();
				ctx.scissor(cellX, cellY, 180.0f, 120.0f);
				ctx.beginPath();
				ctx.circle(cellX + 90.0f, cellY + 60.0f, 95.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(220, 14, 165, 233));
			for (int i = 0; i < 16; ++i) {
				float cellX = 80.0f + (i % 4) * 230.0f;
				float cellY = 60.0f + (i / 4) * 155.0f;
				canvas->save();
				canvas->clipRect(SkRect::MakeXYWH(cellX, cellY, 180.0f, 120.0f), true);
				canvas->drawCircle(cellX + 90.0f, cellY + 60.0f, 95.0f, p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 78: Hierarchical Intersecting Scissors (4 Nested Clip Levels)
	// =========================================================================
	benchmark_op("78. Hierarchical Intersecting Scissors (4 Levels)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(234, 88, 12, 220));
			for (int i = 0; i < 10; ++i) {
				ctx.save();
				float x = 100.0f + i * 20.0f;
				float y = 80.0f + i * 15.0f;
				ctx.scissor(x, y, 600.0f, 400.0f);
				ctx.intersectScissor(x + 50.0f, y + 40.0f, 500.0f, 320.0f);
				ctx.intersectScissor(x + 100.0f, y + 80.0f, 400.0f, 240.0f);
				ctx.intersectScissor(x + 150.0f, y + 120.0f, 300.0f, 160.0f);
				ctx.beginPath();
				ctx.rect(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H));
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(220, 234, 88, 12));
			for (int i = 0; i < 10; ++i) {
				canvas->save();
				float x = 100.0f + i * 20.0f;
				float y = 80.0f + i * 15.0f;
				canvas->clipRect(SkRect::MakeXYWH(x, y, 600.0f, 400.0f), true);
				canvas->clipRect(SkRect::MakeXYWH(x + 50.0f, y + 40.0f, 500.0f, 320.0f), true);
				canvas->clipRect(SkRect::MakeXYWH(x + 100.0f, y + 80.0f, 400.0f, 240.0f), true);
				canvas->clipRect(SkRect::MakeXYWH(x + 150.0f, y + 120.0f, 300.0f, 160.0f), true);
				canvas->drawRect(SkRect::MakeWH(W, H), p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 79: Scissored Stroked Waves (1000-Pt Wave in Center Box)
	// =========================================================================
	benchmark_op("79. Scissored Stroked Waves (1000 Pts Clipped)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.save();
			ctx.scissor(200.0f, 150.0f, 680.0f, 440.0f);
			ctx.beginPath();
			for (int i = 0; i < 1000; ++i) {
				float x = (static_cast<float>(i) / 1000.0f) * W;
				float y = 370.0f + std::sin(i * 0.04f) * 220.0f;
				if (i == 0) ctx.moveTo(x, y);
				else ctx.lineTo(x, y);
			}
			ctx.strokeWidth(3.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(255, 64, 129, 230));
			ctx.stroke();
			ctx.restore();
		},
		[&](SkCanvas* canvas) {
			canvas->save();
			canvas->clipRect(SkRect::MakeXYWH(200.0f, 150.0f, 680.0f, 440.0f), true);
			SkPath path;
			path.incReserve(1005);
			for (int i = 0; i < 1000; ++i) {
				float x = (static_cast<float>(i) / 1000.0f) * W;
				float y = 370.0f + std::sin(i * 0.04f) * 220.0f;
				if (i == 0) path.moveTo(x, y);
				else path.lineTo(x, y);
			}
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(3.0f);
			p.setColor(SkColorSetARGB(230, 255, 64, 129));
			canvas->drawPath(path, p);
			canvas->restore();
		}
	);

	// =========================================================================
	// Suite 80: Scissored Gradient Card (Gradient Fill Clipped by Bounds)
	// =========================================================================
	benchmark_op("80. Scissored Gradient Card (Clipped)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 15; ++i) {
				float x = 80.0f + (i % 5) * 190.0f;
				float y = 80.0f + (i / 5) * 190.0f;
				ctx.save();
				ctx.scissor(x, y, 150.0f, 120.0f);
				auto grad = ctx.linearGradient(x, y, x + 200.0f, y + 200.0f,
					nisaba::gpu::Color::rgba(147, 51, 234, 230),
					nisaba::gpu::Color::rgba(236, 72, 153, 230));
				ctx.beginPath();
				ctx.circle(x + 75.0f, y + 60.0f, 100.0f);
				ctx.fillPaint(grad);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(230, 147, 51, 234), SkColorSetARGB(230, 236, 72, 153) };
			for (int i = 0; i < 15; ++i) {
				float x = 80.0f + (i % 5) * 190.0f;
				float y = 80.0f + (i / 5) * 190.0f;
				canvas->save();
				canvas->clipRect(SkRect::MakeXYWH(x, y, 150.0f, 120.0f), true);
				SkPoint pts[2] = { {x, y}, {x + 200.0f, y + 200.0f} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawCircle(x + 75.0f, y + 60.0f, 100.0f, p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 81: Circular Overflow Clip (Oversized Disk Inside Small Box)
	// =========================================================================
	benchmark_op("81. Circular Overflow Clip (25 Boxes)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(16, 185, 129, 210));
			for (int i = 0; i < 25; ++i) {
				float x = 60.0f + (i % 5) * 200.0f;
				float y = 60.0f + (i / 5) * 130.0f;
				ctx.save();
				ctx.scissor(x, y, 110.0f, 80.0f);
				ctx.beginPath();
				ctx.circle(x + 55.0f, y + 40.0f, 75.0f);
				ctx.fill();
				ctx.restore();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 16, 185, 129));
			for (int i = 0; i < 25; ++i) {
				float x = 60.0f + (i % 5) * 200.0f;
				float y = 60.0f + (i / 5) * 130.0f;
				canvas->save();
				canvas->clipRect(SkRect::MakeXYWH(x, y, 110.0f, 80.0f), true);
				canvas->drawCircle(x + 55.0f, y + 40.0f, 75.0f, p);
				canvas->restore();
			}
		}
	);

	// =========================================================================
	// Suite 82: Scissor Invalidation / Reset Cycle (25 Toggle Cycles)
	// =========================================================================
	benchmark_op("82. Scissor Invalidation / Reset (25 Cycles)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.fillColor(nisaba::gpu::Color::rgba(59, 130, 246, 190));
			for (int i = 0; i < 25; ++i) {
				ctx.scissor(100.0f + i * 5.0f, 80.0f + i * 4.0f, 400.0f, 300.0f);
				ctx.resetScissor();
				ctx.beginPath();
				ctx.rect(80.0f + (i * 29) % (W - 100), 60.0f + (i * 23) % (H - 80), 50.0f, 40.0f);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(190, 59, 130, 246));
			for (int i = 0; i < 25; ++i) {
				canvas->save();
				canvas->clipRect(SkRect::MakeXYWH(100.0f + i * 5.0f, 80.0f + i * 4.0f, 400.0f, 300.0f), true);
				canvas->restore();
				canvas->drawRect(SkRect::MakeXYWH(80.0f + (i * 29) % (W - 100), 60.0f + (i * 23) % (H - 80), 50.0f, 40.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 83: Composite Op: Source-Over (Standard Porter-Duff Stack)
	// =========================================================================
	benchmark_op("83. Composite Op: Source-Over (30 Shapes)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
			for (int i = 0; i < 30; ++i) {
				ctx.beginPath();
				ctx.circle(80.0f + (i * 33) % (W - 120), 80.0f + (i * 21) % (H - 120), 45.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(239, 68, 68, 170));
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(170, 239, 68, 68));
			p.setBlendMode(SkBlendMode::kSrcOver);
			for (int i = 0; i < 30; ++i) {
				canvas->drawCircle(80.0f + (i * 33) % (W - 120), 80.0f + (i * 21) % (H - 120), 45.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 84: Composite Op: Lighter / Plus (30 Overlapping Particles)
	// =========================================================================
	benchmark_op("84. Composite Op: Lighter / Plus (30 Particles)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::Lighter);
			ctx.fillColor(nisaba::gpu::Color::rgba(59, 130, 246, 160));
			for (int i = 0; i < 30; ++i) {
				ctx.beginPath();
				ctx.circle(100.0f + (i % 6) * 160.0f, 120.0f + (i / 6) * 110.0f, 60.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(160, 59, 130, 246));
			p.setBlendMode(SkBlendMode::kPlus);
			for (int i = 0; i < 30; ++i) {
				canvas->drawCircle(100.0f + (i % 6) * 160.0f, 120.0f + (i / 6) * 110.0f, 60.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 85: Composite Op: Source-In (Alpha Masking Overlap)
	// =========================================================================
	benchmark_op("85. Composite Op: Source-In (Alpha Masking)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceIn);
			ctx.fillColor(nisaba::gpu::Color::rgba(245, 158, 11, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 245, 158, 11));
			p.setBlendMode(SkBlendMode::kSrcIn);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 86: Composite Op: Source-Out (Cutout Silhouette)
	// =========================================================================
	benchmark_op("86. Composite Op: Source-Out (Cutout)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOut);
			ctx.fillColor(nisaba::gpu::Color::rgba(16, 185, 129, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 16, 185, 129));
			p.setBlendMode(SkBlendMode::kSrcOut);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 87: Composite Op: Atop (Source Over Target Bounds Only)
	// =========================================================================
	benchmark_op("87. Composite Op: Atop (Target Bounds)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::Atop);
			ctx.fillColor(nisaba::gpu::Color::rgba(168, 85, 247, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 168, 85, 247));
			p.setBlendMode(SkBlendMode::kSrcATop);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 88: Composite Op: Destination-Over (Under-Drawing Layer)
	// =========================================================================
	benchmark_op("88. Composite Op: Dest-Over (Under-Drawing)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::DestinationOver);
			ctx.fillColor(nisaba::gpu::Color::rgba(236, 72, 153, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 236, 72, 153));
			p.setBlendMode(SkBlendMode::kDstOver);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 89: Composite Op: Destination-Out (Eraser / Subtractive Masking)
	// =========================================================================
	benchmark_op("89. Composite Op: Dest-Out (Eraser Mask)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::DestinationOut);
			ctx.fillColor(nisaba::gpu::Color::rgba(255, 255, 255, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 255, 255, 255));
			p.setBlendMode(SkBlendMode::kDstOut);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 90: Composite Op: Destination-Atop (Inverse Compositing)
	// =========================================================================
	benchmark_op("90. Composite Op: Dest-Atop (Inverse)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::DestinationAtop);
			ctx.fillColor(nisaba::gpu::Color::rgba(20, 184, 166, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 20, 184, 166));
			p.setBlendMode(SkBlendMode::kDstATop);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 91: Composite Op: Xor (Mutually Exclusive Alpha Blend)
	// =========================================================================
	benchmark_op("91. Composite Op: Xor (Exclusive Blend)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::Xor);
			ctx.fillColor(nisaba::gpu::Color::rgba(251, 191, 36, 200));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.circle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(200, 251, 191, 36));
			p.setBlendMode(SkBlendMode::kXor);
			for (int i = 0; i < 25; ++i) {
				canvas->drawCircle(120.0f + (i % 5) * 190.0f, 120.0f + (i / 5) * 120.0f, 55.0f, p);
			}
		}
	);

	// =========================================================================
	// Suite 92: Composite Op: Copy (Direct Surface Overwrite)
	// =========================================================================
	benchmark_op("92. Composite Op: Copy (Direct Overwrite)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::Copy);
			ctx.fillColor(nisaba::gpu::Color::rgba(239, 68, 68, 255));
			for (int i = 0; i < 25; ++i) {
				ctx.beginPath();
				ctx.rect(100.0f + (i % 5) * 190.0f, 100.0f + (i / 5) * 120.0f, 90.0f, 60.0f);
				ctx.fill();
			}
			ctx.globalCompositeOperation(nisaba::gpu::CompositeOperation::SourceOver);
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			p.setColor(SkColorSetARGB(255, 239, 68, 68));
			p.setBlendMode(SkBlendMode::kSrc);
			for (int i = 0; i < 25; ++i) {
				canvas->drawRect(SkRect::MakeXYWH(100.0f + (i % 5) * 190.0f, 100.0f + (i / 5) * 120.0f, 90.0f, 60.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 93: UI Dashboard Gauge (Semi-Circular Arc Track + Value Needle)
	// =========================================================================
	benchmark_op("93. UI Dashboard Gauge (Arc Track + Needle)",
		[&](nisaba::gpu::Context& ctx) {
			float cx = W * 0.5f, cy = 480.0f;
			// Background Track
			ctx.beginPath();
			ctx.arc(cx, cy, 220.0f, static_cast<float>(M_PI), static_cast<float>(2.0 * M_PI), nisaba::gpu::Winding::CounterClockwise);
			ctx.strokeWidth(28.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(30, 41, 59, 240));
			ctx.stroke();
			// Active Value Track
			ctx.beginPath();
			ctx.arc(cx, cy, 220.0f, static_cast<float>(M_PI), static_cast<float>(M_PI + 2.2), nisaba::gpu::Winding::CounterClockwise);
			ctx.strokeWidth(28.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(14, 165, 233, 240));
			ctx.stroke();
			// Needle Pointer
			ctx.save();
			ctx.translate(cx, cy);
			ctx.rotate(2.2f);
			ctx.beginPath();
			ctx.moveTo(-8.0f, 0.0f);
			ctx.lineTo(0.0f, -190.0f);
			ctx.lineTo(8.0f, 0.0f);
			ctx.closePath();
			ctx.fillColor(nisaba::gpu::Color::rgba(239, 68, 68, 255));
			ctx.fill();
			ctx.restore();
		},
		[&](SkCanvas* canvas) {
			float cx = W * 0.5f, cy = 480.0f;
			SkPaint pTrack, pVal, pNeedle;
			pTrack.setAntiAlias(true); pTrack.setStyle(SkPaint::kStroke_Style); pTrack.setStrokeWidth(28.0f); pTrack.setColor(SkColorSetARGB(240, 30, 41, 59));
			pVal.setAntiAlias(true); pVal.setStyle(SkPaint::kStroke_Style); pVal.setStrokeWidth(28.0f); pVal.setColor(SkColorSetARGB(240, 14, 165, 233));
			pNeedle.setAntiAlias(true); pNeedle.setColor(SkColorSetARGB(255, 239, 68, 68));
			// Track
			SkPath pathTrack;
			pathTrack.addArc(SkRect::MakeXYWH(cx - 220.0f, cy - 220.0f, 440.0f, 440.0f), 180.0f, 180.0f);
			canvas->drawPath(pathTrack, pTrack);
			// Val
			SkPath pathVal;
			pathVal.addArc(SkRect::MakeXYWH(cx - 220.0f, cy - 220.0f, 440.0f, 440.0f), 180.0f, 2.2f * 180.0f / static_cast<float>(M_PI));
			canvas->drawPath(pathVal, pVal);
			// Needle
			canvas->save();
			canvas->translate(cx, cy);
			canvas->rotate(2.2f * 180.0f / static_cast<float>(M_PI));
			SkPath needle;
			needle.moveTo(-8.0f, 0.0f);
			needle.lineTo(0.0f, -190.0f);
			needle.lineTo(8.0f, 0.0f);
			needle.close();
			canvas->drawPath(needle, pNeedle);
			canvas->restore();
		}
	);

	// =========================================================================
	// Suite 94: Audio Spectrum Visualizer (64 Frequency Bars with Gradients)
	// =========================================================================
	benchmark_op("94. Audio Spectrum Visualizer (64 Bars)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 64; ++i) {
				float x = 70.0f + i * 14.5f;
				float h = 40.0f + std::abs(std::sin(i * 0.15f)) * 380.0f;
				float y = 560.0f - h;
				auto grad = ctx.linearGradient(x, y, x, y + h,
					nisaba::gpu::Color::rgba(236, 72, 153, 240),
					nisaba::gpu::Color::rgba(59, 130, 246, 240));
				ctx.beginPath();
				ctx.roundedRect(x, y, 10.0f, h, 4.0f);
				ctx.fillPaint(grad);
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true);
			SkColor colors[2] = { SkColorSetARGB(240, 236, 72, 153), SkColorSetARGB(240, 59, 130, 246) };
			for (int i = 0; i < 64; ++i) {
				float x = 70.0f + i * 14.5f;
				float h = 40.0f + std::abs(std::sin(i * 0.15f)) * 380.0f;
				float y = 560.0f - h;
				SkPoint pts[2] = { {x, y}, {x, y + h} };
				p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 10.0f, h), 4.0f, 4.0f), p);
			}
		}
	);

	// =========================================================================
	// Suite 95: Circular Progress Rings (8 Donut Percent Ring Meters)
	// =========================================================================
	benchmark_op("95. Circular Progress Rings (8 Meters)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.lineCap(nisaba::gpu::LineCap::Round);
			for (int i = 0; i < 8; ++i) {
				float cx = 140.0f + (i % 4) * 260.0f;
				float cy = 200.0f + (i / 4) * 300.0f;
				// Track
				ctx.beginPath();
				ctx.circle(cx, cy, 70.0f);
				ctx.strokeWidth(14.0f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(51, 65, 85, 200));
				ctx.stroke();
				// Progress
				ctx.beginPath();
				ctx.arc(cx, cy, 70.0f, -static_cast<float>(M_PI)*0.5f, -static_cast<float>(M_PI)*0.5f + (i + 1) * 0.65f, nisaba::gpu::Winding::CounterClockwise);
				ctx.strokeWidth(14.0f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(16, 185, 129, 240));
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint pTrack, pProg;
			pTrack.setAntiAlias(true); pTrack.setStyle(SkPaint::kStroke_Style); pTrack.setStrokeWidth(14.0f); pTrack.setColor(SkColorSetARGB(200, 51, 65, 85));
			pProg.setAntiAlias(true); pProg.setStyle(SkPaint::kStroke_Style); pProg.setStrokeWidth(14.0f); pProg.setStrokeCap(SkPaint::kRound_Cap); pProg.setColor(SkColorSetARGB(240, 16, 185, 129));
			SkPath path;
			for (int i = 0; i < 8; ++i) {
				float cx = 140.0f + (i % 4) * 260.0f;
				float cy = 200.0f + (i / 4) * 300.0f;
				canvas->drawCircle(cx, cy, 70.0f, pTrack);
				path.rewind();
				path.addArc(SkRect::MakeXYWH(cx - 70.0f, cy - 70.0f, 140.0f, 140.0f), -90.0f, (i + 1) * 0.65f * 180.0f / static_cast<float>(M_PI));
				canvas->drawPath(path, pProg);
			}
		}
	);

	// =========================================================================
	// Suite 96: Modern Card Stack (5 Layered Elevated Cards with Shadow)
	// =========================================================================
	benchmark_op("96. Modern Card Stack (5 Elevated Cards)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 5; ++i) {
				float x = 240.0f + i * 40.0f;
				float y = 140.0f + i * 35.0f;
				// Shadow
				auto shadow = ctx.boxGradient(x, y + 6.0f, 440.0f, 220.0f, 16.0f, 18.0f,
					nisaba::gpu::Color::rgba(0, 0, 0, 160),
					nisaba::gpu::Color::rgba(0, 0, 0, 0));
				ctx.beginPath();
				ctx.rect(x - 18.0f, y - 18.0f, 440.0f + 36.0f, 220.0f + 36.0f + 6.0f);
				ctx.fillPaint(shadow);
				ctx.fill();
				// Card Surface
				ctx.beginPath();
				ctx.roundedRect(x, y, 440.0f, 220.0f, 16.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(30, 41, 59, 245));
				ctx.fill();
				// Card Border
				ctx.strokeWidth(1.5f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 40));
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint pShadow, pFill, pStroke;
			pShadow.setAntiAlias(true); pShadow.setColor(SkColorSetARGB(160, 0, 0, 0)); pShadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 18.0f * 0.45f));
			pFill.setAntiAlias(true); pFill.setColor(SkColorSetARGB(245, 30, 41, 59));
			pStroke.setAntiAlias(true); pStroke.setStyle(SkPaint::kStroke_Style); pStroke.setStrokeWidth(1.5f); pStroke.setColor(SkColorSetARGB(40, 255, 255, 255));
			for (int i = 0; i < 5; ++i) {
				float x = 240.0f + i * 40.0f;
				float y = 140.0f + i * 35.0f;
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y + 6.0f, 440.0f, 220.0f), 16.0f, 16.0f), pShadow);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 440.0f, 220.0f), 16.0f, 16.0f), pFill);
				canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 440.0f, 220.0f), 16.0f, 16.0f), pStroke);
			}
		}
	);

	// =========================================================================
	// Suite 97: CAD Cross-Hatch Pattern (Dense 45° Angle Cross-Hatching)
	// =========================================================================
	benchmark_op("97. CAD Cross-Hatch Pattern (80 Angled Lines)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.strokeWidth(1.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(148, 163, 184, 180));
			for (int i = 0; i < 40; ++i) {
				float offset = i * 25.0f;
				ctx.beginPath();
				ctx.moveTo(offset, 0.0f); ctx.lineTo(offset + static_cast<float>(H), static_cast<float>(H));
				ctx.stroke();
				ctx.beginPath();
				ctx.moveTo(offset + static_cast<float>(H), 0.0f); ctx.lineTo(offset, static_cast<float>(H));
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(1.0f); p.setColor(SkColorSetARGB(180, 148, 163, 184));
			SkPath path;
			for (int i = 0; i < 40; ++i) {
				float offset = i * 25.0f;
				path.moveTo(offset, 0.0f); path.lineTo(offset + static_cast<float>(H), static_cast<float>(H));
				path.moveTo(offset + static_cast<float>(H), 0.0f); path.lineTo(offset, static_cast<float>(H));
			}
			canvas->drawPath(path, p);
		}
	);

	// =========================================================================
	// Suite 98: Floating Action Button (FAB with Shadow, Fill & Plus Icon)
	// =========================================================================
	benchmark_op("98. Floating Action Button (FAB 10 Buttons)",
		[&](nisaba::gpu::Context& ctx) {
			for (int i = 0; i < 10; ++i) {
				float cx = 110.0f + (i % 5) * 200.0f;
				float cy = 200.0f + (i / 5) * 280.0f;
				// Shadow
				auto shadow = ctx.radialGradient(cx, cy + 5.0f, 25.0f, 50.0f,
					nisaba::gpu::Color::rgba(0, 0, 0, 160),
					nisaba::gpu::Color::rgba(0, 0, 0, 0));
				ctx.beginPath();
				ctx.circle(cx, cy + 5.0f, 50.0f);
				ctx.fillPaint(shadow);
				ctx.fill();
				// Button
				ctx.beginPath();
				ctx.circle(cx, cy, 35.0f);
				ctx.fillColor(nisaba::gpu::Color::rgba(244, 63, 94, 255));
				ctx.fill();
				// Plus Icon
				ctx.strokeWidth(4.0f);
				ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 255));
				ctx.beginPath();
				ctx.moveTo(cx - 15.0f, cy); ctx.lineTo(cx + 15.0f, cy);
				ctx.moveTo(cx, cy - 15.0f); ctx.lineTo(cx, cy + 15.0f);
				ctx.stroke();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint pShadow, pBtn, pIcon;
			pShadow.setAntiAlias(true);
			pBtn.setAntiAlias(true); pBtn.setColor(SkColorSetARGB(255, 244, 63, 94));
			pIcon.setAntiAlias(true); pIcon.setStyle(SkPaint::kStroke_Style); pIcon.setStrokeWidth(4.0f); pIcon.setColor(SkColorSetARGB(255, 255, 255, 255));
			SkColor shadowColors[2] = { SkColorSetARGB(160, 0, 0, 0), SkColorSetARGB(0, 0, 0, 0) };
			for (int i = 0; i < 10; ++i) {
				float cx = 110.0f + (i % 5) * 200.0f;
				float cy = 200.0f + (i / 5) * 280.0f;
				pShadow.setShader(SkGradientShader::MakeRadial(SkPoint::Make(cx, cy + 5.0f), 50.0f, shadowColors, nullptr, 2, SkTileMode::kClamp));
				canvas->drawCircle(cx, cy + 5.0f, 50.0f, pShadow);
				canvas->drawCircle(cx, cy, 35.0f, pBtn);
				canvas->drawLine(cx - 15.0f, cy, cx + 15.0f, cy, pIcon);
				canvas->drawLine(cx, cy - 15.0f, cx, cy + 15.0f, pIcon);
			}
		}
	);

	// =========================================================================
	// Suite 99: Anti-Aliasing Stress: AA ON vs AA OFF Geometry Grid
	// =========================================================================
	benchmark_op("99. Anti-Aliasing Geometry Grid (50 Diamonds)",
		[&](nisaba::gpu::Context& ctx) {
			ctx.shapeAntiAlias(true);
			ctx.fillColor(nisaba::gpu::Color::rgba(251, 191, 36, 210));
			for (int i = 0; i < 50; ++i) {
				float cx = 60.0f + (i * 71) % (W - 120);
				float cy = 40.0f + (i * 43) % (H - 90);
				ctx.beginPath();
				ctx.moveTo(cx, cy - 30.0f);
				ctx.lineTo(cx + 35.0f, cy);
				ctx.lineTo(cx, cy + 30.0f);
				ctx.lineTo(cx - 35.0f, cy);
				ctx.closePath();
				ctx.fill();
			}
		},
		[&](SkCanvas* canvas) {
			SkPaint p;
			p.setAntiAlias(true); p.setColor(SkColorSetARGB(210, 251, 191, 36));
			SkPath path;
			for (int i = 0; i < 50; ++i) {
				float cx = 60.0f + (i * 71) % (W - 120);
				float cy = 40.0f + (i * 43) % (H - 90);
				path.rewind();
				path.moveTo(cx, cy - 30.0f);
				path.lineTo(cx + 35.0f, cy);
				path.lineTo(cx, cy + 30.0f);
				path.lineTo(cx - 35.0f, cy);
				path.close();
				canvas->drawPath(path, p);
			}
		}
	);

	// =========================================================================
	// Suite 100: Master Vector Stress: Mixed Geometry Mega-Scene
	// =========================================================================
	benchmark_op("100. Master Vector Stress: Mixed Mega-Scene",
		[&](nisaba::gpu::Context& ctx) {
			// 1. Background gradient
			auto grad = ctx.linearGradient(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H),
				nisaba::gpu::Color::rgba(15, 23, 42, 255),
				nisaba::gpu::Color::rgba(30, 41, 59, 255));
			ctx.beginPath();
			ctx.rect(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H));
			ctx.fillPaint(grad);
			ctx.fill();

			// 2. Transformed rotating disks
			float cx = W * 0.5f, cy = H * 0.5f;
			ctx.fillColor(nisaba::gpu::Color::rgba(56, 189, 248, 160));
			for (int i = 0; i < 20; ++i) {
				ctx.save();
				ctx.translate(cx, cy);
				ctx.rotate(i * 0.314f);
				ctx.beginPath();
				ctx.ellipse(150.0f, 0.0f, 50.0f, 20.0f);
				ctx.fill();
				ctx.restore();
			}

			// 3. Central glowing star
			auto starGlow = ctx.radialGradient(cx, cy, 30.0f, 150.0f,
				nisaba::gpu::Color::rgba(244, 63, 94, 220),
				nisaba::gpu::Color::rgba(244, 63, 94, 0));
			ctx.beginPath();
			ctx.circle(cx, cy, 150.0f);
			ctx.fillPaint(starGlow);
			ctx.fill();

			// 4. Overlapping card
			ctx.beginPath();
			ctx.roundedRect(cx - 160.0f, cy - 90.0f, 320.0f, 180.0f, 16.0f);
			ctx.fillColor(nisaba::gpu::Color::rgba(255, 255, 255, 30));
			ctx.fill();
			ctx.strokeWidth(2.0f);
			ctx.strokeColor(nisaba::gpu::Color::rgba(255, 255, 255, 120));
			ctx.stroke();
		},
		[&](SkCanvas* canvas) {
			// 1. Background gradient
			SkPaint pBg;
			pBg.setAntiAlias(true);
			SkColor bgColors[2] = { SkColorSetARGB(255, 15, 23, 42), SkColorSetARGB(255, 30, 41, 59) };
			SkPoint bgPts[2] = { {0.0f, 0.0f}, {static_cast<float>(W), static_cast<float>(H)} };
			pBg.setShader(SkGradientShader::MakeLinear(bgPts, bgColors, nullptr, 2, SkTileMode::kClamp));
			canvas->drawRect(SkRect::MakeWH(W, H), pBg);

			// 2. Transformed rotating disks
			float cx = W * 0.5f, cy = H * 0.5f;
			SkPaint pDisk;
			pDisk.setAntiAlias(true); pDisk.setColor(SkColorSetARGB(160, 56, 189, 248));
			for (int i = 0; i < 20; ++i) {
				canvas->save();
				canvas->translate(cx, cy);
				canvas->rotate(i * 0.314f * 180.0f / static_cast<float>(M_PI));
				canvas->drawOval(SkRect::MakeXYWH(100.0f, -20.0f, 100.0f, 40.0f), pDisk);
				canvas->restore();
			}

			// 3. Central glowing star
			SkPaint pGlow;
			pGlow.setAntiAlias(true);
			SkColor glowColors[2] = { SkColorSetARGB(220, 244, 63, 94), SkColorSetARGB(0, 244, 63, 94) };
			pGlow.setShader(SkGradientShader::MakeRadial(SkPoint::Make(cx, cy), 150.0f, glowColors, nullptr, 2, SkTileMode::kClamp));
			canvas->drawCircle(cx, cy, 150.0f, pGlow);

			// 4. Overlapping card
			SkPaint pCardFill, pCardStroke;
			pCardFill.setAntiAlias(true); pCardFill.setColor(SkColorSetARGB(30, 255, 255, 255));
			pCardStroke.setAntiAlias(true); pCardStroke.setStyle(SkPaint::kStroke_Style); pCardStroke.setStrokeWidth(2.0f); pCardStroke.setColor(SkColorSetARGB(120, 255, 255, 255));
			canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - 160.0f, cy - 90.0f, 320.0f, 180.0f), 16.0f, 16.0f), pCardFill);
			canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(cx - 160.0f, cy - 90.0f, 320.0f, 180.0f), 16.0f, 16.0f), pCardStroke);
		}
	);

	// =========================================================================
	// EXTENDED HIGH-LEVEL GPU SUITES (101–134):
	// Lottie, SVG, Backdrop Blur/Glass, Shadows, ColorFilters, Advanced Gradients,
	// Programmable BlendModes, Path Clips/EvenOdd, Rich/RTL Text, Mipmaps/3D/Vertices,
	// Dashed Strokes & DRRect.
	// =========================================================================

	// Shared backdrop helper for glass/blend/layer tests
	auto draw_rich_backdrop_nisaba = [](nisaba::gpu::GpuCanvas& c) {
		const nisaba::Color cols[6] = {
			nisaba::Color::from_rgba8(230, 57, 70, 255),
			nisaba::Color::from_rgba8(241, 162, 8, 255),
			nisaba::Color::from_rgba8(42, 157, 143, 255),
			nisaba::Color::from_rgba8(38, 70, 83, 255),
			nisaba::Color::from_rgba8(131, 56, 236, 255),
			nisaba::Color::from_rgba8(58, 134, 255, 255),
		};
		for (int i = 0; i < 6; ++i) {
			auto r = nisaba::Rect::from_xywh(i * 180.0f, 0.0f, 180.0f, 720.0f).value();
			c.fill_rect(r, nisaba::Paint(cols[i]));
		}
		for (int i = 0; i < 4; ++i) {
			c.fill_circle(160.0f + i * 250.0f, 360.0f + ((i % 2) ? 80.0f : -80.0f), 110.0f,
				nisaba::Paint(nisaba::Color::from_rgba8(255, 255, 255, 110)));
		}
	};
	auto draw_rich_backdrop_skia = [](SkCanvas* c) {
		const SkColor cols[6] = {
			SkColorSetARGB(255, 230, 57, 70),
			SkColorSetARGB(255, 241, 162, 8),
			SkColorSetARGB(255, 42, 157, 143),
			SkColorSetARGB(255, 38, 70, 83),
			SkColorSetARGB(255, 131, 56, 236),
			SkColorSetARGB(255, 58, 134, 255),
		};
		SkPaint p; p.setAntiAlias(true);
		for (int i = 0; i < 6; ++i) {
			p.setColor(cols[i]);
			c->drawRect(SkRect::MakeXYWH(i * 180.0f, 0.0f, 180.0f, 720.0f), p);
		}
		p.setColor(SkColorSetARGB(110, 255, 255, 255));
		for (int i = 0; i < 4; ++i) {
			c->drawCircle(160.0f + i * 250.0f, 360.0f + ((i % 2) ? 80.0f : -80.0f), 110.0f, p);
		}
	};

	// -------------------------------------------------------------------------
	// Category 1: Lottie GPU (Suites 101–103) — Nisaba Lottie vs Skia Skottie
	// -------------------------------------------------------------------------
	const char* kLottieSimpleJson = R"JSON({
		"v":"5.7.4","fr":60,"ip":0,"op":60,"w":240,"h":240,"nm":"Spinner","ddd":0,"assets":[],
		"layers":[
			{"ddd":0,"ind":1,"ty":4,"nm":"Ring","sr":1,
			 "ks":{"o":{"a":0,"k":100},"r":{"a":1,"k":[{"t":0,"s":[0],"e":[360]},{"t":60,"s":[360]}]},
			       "p":{"a":0,"k":[120,120,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},
			 "shapes":[
				{"ty":"gr","nm":"Arc","it":[
					{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[160,160]}},
					{"ty":"tm","s":{"a":1,"k":[{"t":0,"s":[0],"e":[65]},{"t":30,"s":[65],"e":[0]},{"t":60,"s":[0]}]},
					           "e":{"a":1,"k":[{"t":0,"s":[30],"e":[95]},{"t":30,"s":[95],"e":[30]},{"t":60,"s":[30]}]},
					           "o":{"a":0,"k":0}},
					{"ty":"st","c":{"a":0,"k":[0.22,0.58,1.0,1.0]},"o":{"a":0,"k":100},"w":{"a":0,"k":14},"lc":2,"lj":2},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]}
			 ],"ip":0,"op":60,"st":0},
			{"ddd":0,"ind":2,"ty":4,"nm":"Core","sr":1,
			 "ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[120,120,0]},"a":{"a":0,"k":[0,0,0]},
			       "s":{"a":1,"k":[{"t":0,"s":[80,80,100],"e":[115,115,100]},{"t":30,"s":[115,115,100],"e":[80,80,100]},{"t":60,"s":[80,80,100]}]}},
			 "shapes":[
				{"ty":"gr","nm":"Dot","it":[
					{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[48,48]}},
					{"ty":"fl","c":{"a":0,"k":[0.95,0.35,0.45,1.0]},"o":{"a":0,"k":100}},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]}
			 ],"ip":0,"op":60,"st":0}
		]
	})JSON";

	const char* kLottieComplexJson = R"JSON({
		"v":"5.7.4","fr":60,"ip":0,"op":60,"w":400,"h":400,"nm":"ComplexScene","ddd":0,"assets":[],
		"layers":[
			{"ddd":0,"ind":1,"ty":4,"nm":"BG","sr":1,
			 "ks":{"o":{"a":0,"k":100},"r":{"a":1,"k":[{"t":0,"s":[0],"e":[90]},{"t":60,"s":[90]}]},
			       "p":{"a":0,"k":[200,200,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},
			 "shapes":[
				{"ty":"gr","nm":"Card","it":[
					{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[280,280]},"r":{"a":0,"k":36}},
					{"ty":"fl","c":{"a":0,"k":[0.14,0.18,0.28,1.0]},"o":{"a":0,"k":100}},
					{"ty":"st","c":{"a":0,"k":[0.35,0.75,1.0,1.0]},"o":{"a":0,"k":85},"w":{"a":0,"k":4},"lc":2,"lj":2},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]}
			 ],"ip":0,"op":60,"st":0},
			{"ddd":0,"ind":2,"ty":4,"nm":"Star","sr":1,
			 "ks":{"o":{"a":1,"k":[{"t":0,"s":[100],"e":[40]},{"t":30,"s":[40],"e":[100]},{"t":60,"s":[100]}]},
			       "r":{"a":1,"k":[{"t":0,"s":[0],"e":[360]},{"t":60,"s":[360]}]},
			       "p":{"a":0,"k":[200,200,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},
			 "shapes":[
				{"ty":"gr","nm":"Poly","it":[
					{"ty":"sr","sy":1,"pt":{"a":0,"k":6},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"ir":{"a":0,"k":55},"or":{"a":0,"k":115},"is":{"a":0,"k":0},"os":{"a":0,"k":0}},
					{"ty":"tm","s":{"a":0,"k":0},"e":{"a":1,"k":[{"t":0,"s":[20],"e":[100]},{"t":30,"s":[100],"e":[20]},{"t":60,"s":[20]}]},"o":{"a":0,"k":0}},
					{"ty":"fl","c":{"a":0,"k":[0.98,0.62,0.15,1.0]},"o":{"a":0,"k":80}},
					{"ty":"st","c":{"a":0,"k":[1.0,0.92,0.45,1.0]},"o":{"a":0,"k":100},"w":{"a":0,"k":6},"lc":2,"lj":2},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]}
			 ],"ip":0,"op":60,"st":0},
			{"ddd":0,"ind":3,"ty":4,"nm":"Orbit1","sr":1,
			 "ks":{"o":{"a":0,"k":100},"r":{"a":1,"k":[{"t":0,"s":[0],"e":[-360]},{"t":60,"s":[-360]}]},
			       "p":{"a":0,"k":[200,200,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},
			 "shapes":[
				{"ty":"gr","nm":"OrbA","it":[
					{"ty":"el","p":{"a":0,"k":[130,0]},"s":{"a":0,"k":[44,44]}},
					{"ty":"fl","c":{"a":0,"k":[0.2,0.9,0.65,1.0]},"o":{"a":0,"k":100}},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]},
				{"ty":"gr","nm":"OrbB","it":[
					{"ty":"el","p":{"a":0,"k":[-130,0]},"s":{"a":0,"k":[44,44]}},
					{"ty":"fl","c":{"a":0,"k":[0.92,0.28,0.58,1.0]},"o":{"a":0,"k":100}},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]},
				{"ty":"gr","nm":"OrbC","it":[
					{"ty":"el","p":{"a":0,"k":[0,130]},"s":{"a":0,"k":[36,36]}},
					{"ty":"fl","c":{"a":0,"k":[0.55,0.42,1.0,1.0]},"o":{"a":0,"k":100}},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]},
				{"ty":"gr","nm":"OrbD","it":[
					{"ty":"el","p":{"a":0,"k":[0,-130]},"s":{"a":0,"k":[36,36]}},
					{"ty":"fl","c":{"a":0,"k":[0.25,0.75,1.0,1.0]},"o":{"a":0,"k":100}},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]}
			 ],"ip":0,"op":60,"st":0},
			{"ddd":0,"ind":4,"ty":4,"nm":"Wave","sr":1,
			 "ks":{"o":{"a":0,"k":90},"r":{"a":0,"k":0},"p":{"a":0,"k":[200,200,0]},"a":{"a":0,"k":[0,0,0]},
			       "s":{"a":1,"k":[{"t":0,"s":[90,90,100],"e":[110,110,100]},{"t":30,"s":[110,110,100],"e":[90,90,100]},{"t":60,"s":[90,90,100]}]}},
			 "shapes":[
				{"ty":"gr","nm":"RingOut","it":[
					{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[320,320]}},
					{"ty":"tm","s":{"a":1,"k":[{"t":0,"s":[0],"e":[40]},{"t":60,"s":[40]}]},
					           "e":{"a":1,"k":[{"t":0,"s":[55],"e":[95]},{"t":60,"s":[95]}]},"o":{"a":0,"k":0}},
					{"ty":"st","c":{"a":0,"k":[0.3,0.85,0.95,1.0]},"o":{"a":0,"k":100},"w":{"a":0,"k":8},"lc":2,"lj":2},
					{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}
				]}
			 ],"ip":0,"op":60,"st":0}
		]
	})JSON";

	auto nisabaLottieSimple = nisaba::lottie::Animation::load_from_data(kLottieSimpleJson);
	auto nisabaLottieComplex = nisaba::lottie::Animation::load_from_data(kLottieComplexJson);
#ifdef HAVE_SKOTTIE
	auto skiaLottieSimple = skottie::Animation::Make(kLottieSimpleJson, std::strlen(kLottieSimpleJson));
	auto skiaLottieComplex = skottie::Animation::Make(kLottieComplexJson, std::strlen(kLottieComplexJson));
	if (nisabaLottieSimple && skiaLottieSimple) {
		float nFrame = 0.0f, sFrame = 0.0f;
		const auto nDst = nisaba::Rect::from_xywh(420.0f, 240.0f, 240.0f, 240.0f).value();
		const SkRect sDst = SkRect::MakeXYWH(420.0f, 240.0f, 240.0f, 240.0f);
		benchmark_canvas_op("101. Lottie Simple Spinner (GPU Vector)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisabaLottieSimple->render(c, nFrame, nDst);
				nFrame = std::fmod(nFrame + 1.0f, 60.0f);
			},
			[&](SkCanvas* c) {
				skiaLottieSimple->seekFrame(sFrame);
				skiaLottieSimple->render(c, &sDst);
				sFrame = std::fmod(sFrame + 1.0f, 60.0f);
			}
		);
	}
	if (nisabaLottieComplex && skiaLottieComplex) {
		float nFrame = 0.0f, sFrame = 0.0f;
		const auto nDst = nisaba::Rect::from_xywh(340.0f, 160.0f, 400.0f, 400.0f).value();
		const SkRect sDst = SkRect::MakeXYWH(340.0f, 160.0f, 400.0f, 400.0f);
		benchmark_canvas_op("102. Lottie Complex Multi-Layer (GPU)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisabaLottieComplex->render(c, nFrame, nDst);
				nFrame = std::fmod(nFrame + 1.0f, 60.0f);
			},
			[&](SkCanvas* c) {
				skiaLottieComplex->seekFrame(sFrame);
				skiaLottieComplex->render(c, &sDst);
				sFrame = std::fmod(sFrame + 1.0f, 60.0f);
			}
		);
		benchmark_canvas_op("103. Lottie 4x Simultaneous Instances",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 4; ++i) {
					float x = 140.0f + (i % 2) * 440.0f;
					float y = 40.0f + (i / 2) * 340.0f;
					auto dst = nisaba::Rect::from_xywh(x, y, 300.0f, 300.0f).value();
					nisabaLottieComplex->render(c, std::fmod(nFrame + i * 15.0f, 60.0f), dst);
				}
				nFrame = std::fmod(nFrame + 1.0f, 60.0f);
			},
			[&](SkCanvas* c) {
				for (int i = 0; i < 4; ++i) {
					float x = 140.0f + (i % 2) * 440.0f;
					float y = 40.0f + (i / 2) * 340.0f;
					SkRect dst = SkRect::MakeXYWH(x, y, 300.0f, 300.0f);
					skiaLottieComplex->seekFrame(std::fmod(sFrame + i * 15.0f, 60.0f));
					skiaLottieComplex->render(c, &dst);
				}
				sFrame = std::fmod(sFrame + 1.0f, 60.0f);
			}
		);
	}
#endif

	// -------------------------------------------------------------------------
	// Category 2: SVG GPU (Suites 104–106) — SvgDocument & SvgCache vs Skia
	// -------------------------------------------------------------------------
	const char* kSvgIconXml = R"SVG(
		<svg viewBox="0 0 64 64" width="64" height="64">
			<rect x="6" y="6" width="52" height="52" rx="12" fill="#2563eb" stroke="#93c5fd" stroke-width="3"/>
			<circle cx="32" cy="28" r="10" fill="#ffffff"/>
			<path d="M16 50 C16 40 48 40 48 50" fill="none" stroke="#ffffff" stroke-width="5" stroke-linecap="round"/>
		</svg>
	)SVG";
	const char* kSvgComplexXml = R"SVG(
		<svg viewBox="0 0 400 300" width="400" height="300">
			<defs>
				<linearGradient id="bgGrad" x1="0" y1="0" x2="1" y2="1">
					<stop offset="0%" stop-color="#0f172a"/>
					<stop offset="50%" stop-color="#1e3a8a"/>
					<stop offset="100%" stop-color="#7c3aed"/>
				</linearGradient>
				<radialGradient id="orbGrad" cx="0.5" cy="0.5" r="0.5">
					<stop offset="0%" stop-color="#fef08a"/>
					<stop offset="100%" stop-color="#ef4444"/>
				</radialGradient>
				<clipPath id="cardClip">
					<rect x="20" y="20" width="360" height="260" rx="24"/>
				</clipPath>
			</defs>
			<g clip-path="url(#cardClip)">
				<rect x="20" y="20" width="360" height="260" rx="24" fill="url(#bgGrad)"/>
				<circle cx="290" cy="110" r="70" fill="url(#orbGrad)" opacity="0.85"/>
				<g opacity="0.75">
					<path d="M20 220 C110 140 210 270 380 180 L380 280 L20 280 Z" fill="#38bdf8"/>
					<path d="M20 245 C140 190 250 290 380 215 L380 280 L20 280 Z" fill="#0284c7"/>
				</g>
				<rect x="44" y="48" width="140" height="24" rx="8" fill="#ffffff" opacity="0.9"/>
				<rect x="44" y="84" width="100" height="14" rx="6" fill="#cbd5e1" opacity="0.7"/>
			</g>
		</svg>
	)SVG";

	auto svgIconDoc = nisaba::svg::SvgDocument::parse(kSvgIconXml);
	auto svgComplexDoc = nisaba::svg::SvgDocument::parse(kSvgComplexXml);
	nisaba::svg::SvgCache svgCache(64);

	// Build Skia equivalent icon path + pre-rasterized cached SkImage for Suite 106
	SkPath skSvgIconArc;
	skSvgIconArc.moveTo(16.0f, 50.0f);
	skSvgIconArc.cubicTo(16.0f, 40.0f, 48.0f, 40.0f, 48.0f, 50.0f);
	auto draw_skia_svg_icon = [&](SkCanvas* c, float x, float y, float sz) {
		c->save();
		c->translate(x, y);
		c->scale(sz / 64.0f, sz / 64.0f);
		SkPaint pFill; pFill.setAntiAlias(true); pFill.setColor(SkColorSetARGB(255, 37, 99, 235));
		SkRRect rr = SkRRect::MakeRectXY(SkRect::MakeXYWH(6, 6, 52, 52), 12, 12);
		c->drawRRect(rr, pFill);
		SkPaint pStroke; pStroke.setAntiAlias(true); pStroke.setStyle(SkPaint::kStroke_Style);
		pStroke.setStrokeWidth(3.0f); pStroke.setColor(SkColorSetARGB(255, 147, 197, 253));
		c->drawRRect(rr, pStroke);
		pFill.setColor(SK_ColorWHITE);
		c->drawCircle(32, 28, 10, pFill);
		pStroke.setColor(SK_ColorWHITE); pStroke.setStrokeWidth(5.0f); pStroke.setStrokeCap(SkPaint::kRound_Cap);
		c->drawPath(skSvgIconArc, pStroke);
		c->restore();
	};

	sk_sp<SkSurface> skIconOff = SkSurface::MakeRasterN32Premul(96, 96);
	draw_skia_svg_icon(skIconOff->getCanvas(), 0, 0, 96.0f);
	sk_sp<SkImage> skCachedSvgIcon = skIconOff->makeImageSnapshot()->makeTextureImage(dContext.get());

	if (svgIconDoc) {
		benchmark_canvas_op("104. SVG Icons Grid 25x (Vector GPU)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int r = 0; r < 5; ++r) {
					for (int col = 0; col < 5; ++col) {
						auto dst = nisaba::Rect::from_xywh(140.0f + col * 160.0f, 60.0f + r * 120.0f, 96.0f, 96.0f).value();
						svgIconDoc->render_gpu(c, &dst);
					}
				}
			},
			[&](SkCanvas* c) {
				for (int r = 0; r < 5; ++r) {
					for (int col = 0; col < 5; ++col) {
						draw_skia_svg_icon(c, 140.0f + col * 160.0f, 60.0f + r * 120.0f, 96.0f);
					}
				}
			}
		);
	}

	if (svgComplexDoc) {
		SkPath skWave1, skWave2;
		skWave1.moveTo(20, 220); skWave1.cubicTo(110, 140, 210, 270, 380, 180); skWave1.lineTo(380, 280); skWave1.lineTo(20, 280); skWave1.close();
		skWave2.moveTo(20, 245); skWave2.cubicTo(140, 190, 250, 290, 380, 215); skWave2.lineTo(380, 280); skWave2.lineTo(20, 280); skWave2.close();
		SkPoint skGradPts[2] = {{20, 20}, {380, 280}};
		SkColor skBgCols[3] = {SkColorSetARGB(255, 15, 23, 42), SkColorSetARGB(255, 30, 58, 138), SkColorSetARGB(255, 124, 58, 237)};
		SkScalar skBgPos[3] = {0.0f, 0.5f, 1.0f};
		SkColor skOrbCols[2] = {SkColorSetARGB(255, 254, 240, 138), SkColorSetARGB(255, 239, 68, 68)};
		auto skBgShader = SkGradientShader::MakeLinear(skGradPts, skBgCols, skBgPos, 3, SkTileMode::kClamp);
		auto skOrbShader = SkGradientShader::MakeRadial({290, 110}, 70, skOrbCols, nullptr, 2, SkTileMode::kClamp);
		const auto nSceneDst = nisaba::Rect::from_xywh(140.0f, 60.0f, 800.0f, 600.0f).value();

		benchmark_canvas_op("105. SVG Complex Scene (Gradients+Clip)",
			[&](nisaba::gpu::GpuCanvas& c) {
				svgComplexDoc->render_gpu(c, &nSceneDst);
			},
			[&](SkCanvas* c) {
				c->save();
				c->translate(140.0f, 60.0f);
				c->scale(2.0f, 2.0f);
				SkRRect clipRR = SkRRect::MakeRectXY(SkRect::MakeXYWH(20, 20, 360, 260), 24, 24);
				c->clipRRect(clipRR, true);
				SkPaint pBg; pBg.setAntiAlias(true); pBg.setShader(skBgShader);
				c->drawRRect(clipRR, pBg);
				SkPaint pOrb; pOrb.setAntiAlias(true); pOrb.setShader(skOrbShader); pOrb.setAlphaf(0.85f);
				c->drawCircle(290, 110, 70, pOrb);
				SkPaint pLayer; pLayer.setAlphaf(0.75f);
				c->saveLayer(nullptr, &pLayer);
				SkPaint pW; pW.setAntiAlias(true);
				pW.setColor(SkColorSetARGB(255, 56, 189, 248)); c->drawPath(skWave1, pW);
				pW.setColor(SkColorSetARGB(255, 2, 132, 199)); c->drawPath(skWave2, pW);
				c->restore();
				SkPaint pBar; pBar.setAntiAlias(true);
				pBar.setColor(SkColorSetARGB(230, 255, 255, 255));
				c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(44, 48, 140, 24), 8, 8), pBar);
				pBar.setColor(SkColorSetARGB(179, 203, 213, 225));
				c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(44, 84, 100, 14), 6, 6), pBar);
				c->restore();
			}
		);
	}

	if (svgIconDoc && skCachedSvgIcon) {
		benchmark_canvas_op("106. SVG Cached Icons 25x (SvgCache)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int r = 0; r < 5; ++r) {
					for (int col = 0; col < 5; ++col) {
						auto dst = nisaba::Rect::from_xywh(140.0f + col * 160.0f, 60.0f + r * 120.0f, 96.0f, 96.0f).value();
						svgCache.draw(c, *svgIconDoc, dst);
					}
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				SkSamplingOptions samp(SkFilterMode::kLinear);
				for (int r = 0; r < 5; ++r) {
					for (int col = 0; col < 5; ++col) {
						SkRect dst = SkRect::MakeXYWH(140.0f + col * 160.0f, 60.0f + r * 120.0f, 96.0f, 96.0f);
						c->drawImageRect(skCachedSvgIcon, dst, samp, &p);
					}
				}
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 3: Backdrop Blur, Frosted Glass & Layers (Suites 107–111)
	// -------------------------------------------------------------------------
	{
		auto skBlur12 = SkImageFilters::Blur(12.0f, 12.0f, SkTileMode::kClamp, nullptr);
		auto skBlur14 = SkImageFilters::Blur(14.0f, 14.0f, SkTileMode::kClamp, nullptr);
		auto skBlur10 = SkImageFilters::Blur(10.0f, 10.0f, SkTileMode::kClamp, nullptr);
		auto skBlur8  = SkImageFilters::Blur(8.0f, 8.0f, SkTileMode::kClamp, nullptr);
		const auto cardRect = nisaba::Rect::from_xywh(240.0f, 160.0f, 600.0f, 400.0f).value();
		const auto cardRRect = nisaba::RRect::from_rect_and_radius(cardRect, 24.0f);
		const SkRect skCardRect = SkRect::MakeXYWH(240.0f, 160.0f, 600.0f, 400.0f);
		const SkRRect skCardRRect = SkRRect::MakeRectXY(skCardRect, 24.0f, 24.0f);

		benchmark_canvas_op("107. Backdrop Blur Single Card (r=12)",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				c.draw_backdrop_blur(cardRRect, 12.0f);
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				c->save();
				c->clipRRect(skCardRRect, true);
				SkCanvas::SaveLayerRec rec(&skCardRect, nullptr, skBlur12.get(), 0);
				c->saveLayer(rec);
				c->restore();
				c->restore();
			}
		);

		auto glassDark = nisaba::effects::GlassParams::dark();
		auto draw_skia_glass_card = [&](SkCanvas* c, const SkRect& r, float radius, const sk_sp<SkImageFilter>& blurF) {
			SkRRect rr = SkRRect::MakeRectXY(r, radius, radius);
			SkPaint pShad; pShad.setAntiAlias(true); pShad.setColor(SkColorSetARGB(160, 0, 0, 0));
			pShad.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 16.0f));
			c->drawRRect(rr.makeOffset(0, 10), pShad);
			c->save();
			c->clipRRect(rr, true);
			SkCanvas::SaveLayerRec rec(&r, nullptr, blurF.get(), 0);
			c->saveLayer(rec);
			SkPaint pTint; pTint.setAntiAlias(true); pTint.setColor(SkColorSetARGB(140, 20, 24, 33));
			c->drawRect(r, pTint);
			c->restore();
			c->restore();
			SkPaint pBdr; pBdr.setAntiAlias(true); pBdr.setStyle(SkPaint::kStroke_Style);
			pBdr.setStrokeWidth(1.0f); pBdr.setColor(SkColorSetARGB(40, 255, 255, 255));
			c->drawRRect(rr, pBdr);
		};

		benchmark_canvas_op("108. Frosted Glass Panel 1x (Full)",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				c.draw_glass_panel(cardRect, 24.0f, 24.0f, glassDark);
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				draw_skia_glass_card(c, skCardRect, 24.0f, skBlur14);
			}
		);

		benchmark_canvas_op("109. Frosted Glass 4x Sibling Cards",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				for (int i = 0; i < 4; ++i) {
					float x = 80.0f + (i % 2) * 480.0f;
					float y = 70.0f + (i / 2) * 300.0f;
					auto r = nisaba::Rect::from_xywh(x, y, 440.0f, 260.0f).value();
					c.draw_glass_panel(r, 20.0f, 20.0f, glassDark);
				}
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				for (int i = 0; i < 4; ++i) {
					float x = 80.0f + (i % 2) * 480.0f;
					float y = 70.0f + (i / 2) * 300.0f;
					draw_skia_glass_card(c, SkRect::MakeXYWH(x, y, 440.0f, 260.0f), 20.0f, skBlur14);
				}
			}
		);

		benchmark_canvas_op("110. SaveLayer + Blur ImageFilter (r=8)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisaba::Paint lp(nisaba::Color::WHITE);
				lp.set_image_filter(nisaba::effects::ImageFilter::blur(8.0f));
				c.save_layer_bounds(cardRect, lp);
				for (int i = 0; i < 8; ++i) {
					auto r = nisaba::Rect::from_xywh(270.0f + i * 65.0f, 220.0f + (i % 3) * 60.0f, 90.0f, 90.0f).value();
					c.fill_round_rect(r, 14.0f, 14.0f, nisaba::Paint(nisaba::Color::from_rgba8(60 + i * 24, 180 - i * 12, 240, 220)));
				}
				c.restore();
			},
			[&](SkCanvas* c) {
				SkPaint lp; lp.setImageFilter(skBlur8);
				c->saveLayer(&skCardRect, &lp);
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 8; ++i) {
					p.setColor(SkColorSetARGB(220, 60 + i * 24, 180 - i * 12, 240));
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(270.0f + i * 65.0f, 220.0f + (i % 3) * 60.0f, 90.0f, 90.0f), 14.0f, 14.0f), p);
				}
				c->restore();
			}
		);

		benchmark_canvas_op("111. SaveLayer Backdrop (r=10) + UI",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				c.save_layer_backdrop(cardRect, nisaba::Paint(nisaba::Color::WHITE), 10.0f);
				c.fill_round_rect(nisaba::Rect::from_xywh(280.0f, 200.0f, 520.0f, 60.0f).value(), 12.0f, 12.0f,
					nisaba::Paint(nisaba::Color::from_rgba8(255, 255, 255, 200)));
				c.fill_round_rect(nisaba::Rect::from_xywh(280.0f, 290.0f, 360.0f, 40.0f).value(), 8.0f, 8.0f,
					nisaba::Paint(nisaba::Color::from_rgba8(255, 255, 255, 140)));
				c.restore();
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				SkCanvas::SaveLayerRec rec(&skCardRect, nullptr, skBlur10.get(), 0);
				c->saveLayer(rec);
				SkPaint p; p.setAntiAlias(true);
				p.setColor(SkColorSetARGB(200, 255, 255, 255));
				c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(280.0f, 200.0f, 520.0f, 60.0f), 12.0f, 12.0f), p);
				p.setColor(SkColorSetARGB(140, 255, 255, 255));
				c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(280.0f, 290.0f, 360.0f, 40.0f), 8.0f, 8.0f), p);
				c->restore();
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 4: Analytical Shadows & Color Filters (Suites 112–115)
	// -------------------------------------------------------------------------
	{
		auto shadowCfg = nisaba::effects::DropShadow(0.0f, 10.0f, 14.0f, nisaba::Color::from_rgba8(0, 0, 0, 140));
		auto skBlurMask = SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 14.0f);
		benchmark_canvas_op("112. Elevated RRect Shadows 10x Cards",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisaba::Paint cardP(nisaba::Color::from_rgba8(245, 247, 252, 255));
				for (int i = 0; i < 10; ++i) {
					float x = 50.0f + (i % 5) * 200.0f;
					float y = 80.0f + (i / 5) * 300.0f;
					auto rr = nisaba::RRect::from_rect_and_radius(nisaba::Rect::from_xywh(x, y, 170.0f, 240.0f).value(), 18.0f);
					c.draw_rrect_with_shadow(rr, cardP, shadowCfg);
				}
			},
			[&](SkCanvas* c) {
				SkPaint pShad; pShad.setAntiAlias(true); pShad.setColor(SkColorSetARGB(140, 0, 0, 0)); pShad.setMaskFilter(skBlurMask);
				SkPaint pCard; pCard.setAntiAlias(true); pCard.setColor(SkColorSetARGB(255, 245, 247, 252));
				for (int i = 0; i < 10; ++i) {
					float x = 50.0f + (i % 5) * 200.0f;
					float y = 80.0f + (i / 5) * 300.0f;
					SkRRect rr = SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 170.0f, 240.0f), 18.0f, 18.0f);
					c->drawRRect(rr.makeOffset(0, 10), pShad);
					c->drawRRect(rr, pCard);
				}
			}
		);

		auto make_star_nisaba = [](float cx, float cy, float rOut, float rIn) {
			nisaba::PathBuilder pb;
			for (int k = 0; k < 10; ++k) {
				float ang = static_cast<float>(k) * static_cast<float>(M_PI) / 5.0f - static_cast<float>(M_PI) * 0.5f;
				float rad = (k % 2 == 0) ? rOut : rIn;
				float px = cx + std::cos(ang) * rad;
				float py = cy + std::sin(ang) * rad;
				if (k == 0) pb.move_to(px, py); else pb.line_to(px, py);
			}
			pb.close();
			return pb.finish().value();
		};
		auto make_star_skia = [](float cx, float cy, float rOut, float rIn) {
			SkPath p;
			for (int k = 0; k < 10; ++k) {
				float ang = static_cast<float>(k) * static_cast<float>(M_PI) / 5.0f - static_cast<float>(M_PI) * 0.5f;
				float rad = (k % 2 == 0) ? rOut : rIn;
				float px = cx + std::cos(ang) * rad;
				float py = cy + std::sin(ang) * rad;
				if (k == 0) p.moveTo(px, py); else p.lineTo(px, py);
			}
			p.close();
			return p;
		};
		std::vector<nisaba::Path> nStars;
		std::vector<SkPath> sStars;
		for (int i = 0; i < 5; ++i) {
			nStars.push_back(make_star_nisaba(140.0f + i * 200.0f, 360.0f, 75.0f, 32.0f));
			sStars.push_back(make_star_skia(140.0f + i * 200.0f, 360.0f, 75.0f, 32.0f));
		}
		auto skDropShadow = SkImageFilters::DropShadow(4.0f, 6.0f, 8.0f, 8.0f, SkColorSetARGB(180, 0, 0, 0), nullptr);
		benchmark_canvas_op("113. Path DropShadow Filter (5x Stars)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisaba::Paint p(nisaba::Color::from_rgba8(255, 195, 40, 255));
				p.set_image_filter(nisaba::effects::ImageFilter::drop_shadow(4.0f, 6.0f, 8.0f, nisaba::Color::from_rgba8(0, 0, 0, 180)));
				for (const auto& st : nStars) c.fill_path(st, p);
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true); p.setColor(SkColorSetARGB(255, 255, 195, 40));
				p.setImageFilter(skDropShadow);
				for (const auto& st : sStars) c->drawPath(st, p);
			}
		);

		const float sepiaMat[20] = {
			0.393f, 0.769f, 0.189f, 0.0f, 0.0f,
			0.349f, 0.686f, 0.168f, 0.0f, 0.0f,
			0.272f, 0.534f, 0.131f, 0.0f, 0.0f,
			0.000f, 0.000f, 0.000f, 1.0f, 0.0f
		};
		auto skSepiaCF = SkColorFilters::Matrix(sepiaMat);
		benchmark_canvas_op("114. ColorMatrix Filter (20x Sepia)",
			[&](nisaba::gpu::GpuCanvas& c) {
				auto sepia = nisaba::effects::ColorMatrix::sepia(1.0f);
				for (int i = 0; i < 20; ++i) {
					float x = 40.0f + (i % 5) * 205.0f;
					float y = 40.0f + (i / 5) * 165.0f;
					nisaba::Paint p(nisaba::Color::from_rgba8(60 + (i * 19) % 190, 90 + (i * 31) % 160, 220 - i * 8, 255));
					p.set_color_filter(sepia);
					c.fill_round_rect(nisaba::Rect::from_xywh(x, y, 180.0f, 140.0f).value(), 14.0f, 14.0f, p);
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true); p.setColorFilter(skSepiaCF);
				for (int i = 0; i < 20; ++i) {
					float x = 40.0f + (i % 5) * 205.0f;
					float y = 40.0f + (i / 5) * 165.0f;
					p.setColor(SkColorSetARGB(255, 60 + (i * 19) % 190, 90 + (i * 31) % 160, 220 - i * 8));
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 180.0f, 140.0f), 14.0f, 14.0f), p);
				}
			}
		);

		auto skBlendCF = SkColorFilters::Blend(SkColorSetARGB(128, 60, 180, 255), SkBlendMode::kSrcATop);
		benchmark_canvas_op("115. ColorBlend Filter (20x SrcATop)",
			[&](nisaba::gpu::GpuCanvas& c) {
				auto cf = nisaba::effects::ColorEffect::mode(nisaba::Color::from_rgba8(60, 180, 255, 128), nisaba::BlendMode::SourceAtop);
				for (int i = 0; i < 20; ++i) {
					float x = 40.0f + (i % 5) * 205.0f;
					float y = 40.0f + (i / 5) * 165.0f;
					nisaba::Paint p(nisaba::Color::from_rgba8(240 - i * 8, 70 + (i * 23) % 170, 90 + (i * 17) % 150, 255));
					p.set_color_filter(cf);
					c.fill_round_rect(nisaba::Rect::from_xywh(x, y, 180.0f, 140.0f).value(), 14.0f, 14.0f, p);
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true); p.setColorFilter(skBlendCF);
				for (int i = 0; i < 20; ++i) {
					float x = 40.0f + (i % 5) * 205.0f;
					float y = 40.0f + (i / 5) * 165.0f;
					p.setColor(SkColorSetARGB(255, 240 - i * 8, 70 + (i * 23) % 170, 90 + (i * 17) % 150));
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 180.0f, 140.0f), 14.0f, 14.0f), p);
				}
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 5: Advanced Gradients (Suites 116–120)
	// -------------------------------------------------------------------------
	{
		std::vector<nisaba::GradientStop> stops6 = {
			nisaba::GradientStop::create(0.00f, nisaba::Color::from_rgba8(239, 68, 68, 255)),
			nisaba::GradientStop::create(0.20f, nisaba::Color::from_rgba8(245, 158, 11, 255)),
			nisaba::GradientStop::create(0.40f, nisaba::Color::from_rgba8(16, 185, 129, 255)),
			nisaba::GradientStop::create(0.60f, nisaba::Color::from_rgba8(59, 130, 246, 255)),
			nisaba::GradientStop::create(0.80f, nisaba::Color::from_rgba8(139, 92, 246, 255)),
			nisaba::GradientStop::create(1.00f, nisaba::Color::from_rgba8(236, 72, 153, 255)),
		};
		SkColor skCols6[6] = {
			SkColorSetARGB(255, 239, 68, 68), SkColorSetARGB(255, 245, 158, 11),
			SkColorSetARGB(255, 16, 185, 129), SkColorSetARGB(255, 59, 130, 246),
			SkColorSetARGB(255, 139, 92, 246), SkColorSetARGB(255, 236, 72, 153)
		};
		SkScalar skPos6[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

		benchmark_canvas_op("116. Multi-Stop Linear Grad (6 Stops)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 16; ++i) {
					float x = 40.0f + (i % 4) * 255.0f;
					float y = 40.0f + (i / 4) * 165.0f;
					auto g = nisaba::LinearGradient::create(nisaba::Point::from_xy(x, y), nisaba::Point::from_xy(x + 230.0f, y + 140.0f), stops6).value();
					c.fill_round_rect(nisaba::Rect::from_xywh(x, y, 230.0f, 140.0f).value(), 16.0f, 16.0f, nisaba::Paint(nisaba::Shader(g)));
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 16; ++i) {
					float x = 40.0f + (i % 4) * 255.0f;
					float y = 40.0f + (i / 4) * 165.0f;
					SkPoint pts[2] = {{x, y}, {x + 230.0f, y + 140.0f}};
					p.setShader(SkGradientShader::MakeLinear(pts, skCols6, skPos6, 6, SkTileMode::kClamp));
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 230.0f, 140.0f), 16.0f, 16.0f), p);
				}
			}
		);

		std::vector<nisaba::GradientStop> stops20;
		SkColor skCols20[20];
		SkScalar skPos20[20];
		for (int k = 0; k < 20; ++k) {
			float t = static_cast<float>(k) / 19.0f;
			uint8_t r = static_cast<uint8_t>(127.0f + 127.0f * std::sin(t * 6.28318f));
			uint8_t g = static_cast<uint8_t>(127.0f + 127.0f * std::sin(t * 6.28318f + 2.094f));
			uint8_t b = static_cast<uint8_t>(127.0f + 127.0f * std::sin(t * 6.28318f + 4.188f));
			stops20.push_back(nisaba::GradientStop::create(t, nisaba::Color::from_rgba8(r, g, b, 255)));
			skCols20[k] = SkColorSetARGB(255, r, g, b);
			skPos20[k] = t;
		}
		benchmark_canvas_op("117. High-Stop LUT Linear Grad (20 St)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 8; ++i) {
					float y = 40.0f + i * 82.0f;
					auto g = nisaba::LinearGradient::create(nisaba::Point::from_xy(40.0f, y), nisaba::Point::from_xy(1040.0f, y), stops20).value();
					c.fill_round_rect(nisaba::Rect::from_xywh(40.0f, y, 1000.0f, 68.0f).value(), 10.0f, 10.0f, nisaba::Paint(nisaba::Shader(g)));
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 8; ++i) {
					float y = 40.0f + i * 82.0f;
					SkPoint pts[2] = {{40.0f, y}, {1040.0f, y}};
					p.setShader(SkGradientShader::MakeLinear(pts, skCols20, skPos20, 20, SkTileMode::kClamp));
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(40.0f, y, 1000.0f, 68.0f), 10.0f, 10.0f), p);
				}
			}
		);

		benchmark_canvas_op("118. Sweep (Conic) Gradient (12x Discs)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 12; ++i) {
					float cx = 140.0f + (i % 4) * 265.0f;
					float cy = 130.0f + (i / 4) * 230.0f;
					auto g = nisaba::SweepGradient::create(nisaba::Point::from_xy(cx, cy), 0.0f, 360.0f, stops6).value();
					c.fill_circle(cx, cy, 92.0f, nisaba::Paint(nisaba::Shader(g)));
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 12; ++i) {
					float cx = 140.0f + (i % 4) * 265.0f;
					float cy = 130.0f + (i / 4) * 230.0f;
					p.setShader(SkGradientShader::MakeSweep(cx, cy, skCols6, skPos6, 6));
					c->drawCircle(cx, cy, 92.0f, p);
				}
			}
		);

		benchmark_canvas_op("119. Two-Point Conical Grad (12x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 12; ++i) {
					float cx = 140.0f + (i % 4) * 265.0f;
					float cy = 130.0f + (i / 4) * 230.0f;
					auto g = nisaba::TwoPointConicalGradient::create(
						nisaba::Point::from_xy(cx - 25.0f, cy - 25.0f), 12.0f,
						nisaba::Point::from_xy(cx, cy), 95.0f, stops6).value();
					c.fill_circle(cx, cy, 92.0f, nisaba::Paint(nisaba::Shader(g)));
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 12; ++i) {
					float cx = 140.0f + (i % 4) * 265.0f;
					float cy = 130.0f + (i / 4) * 230.0f;
					p.setShader(SkGradientShader::MakeTwoPointConical(
						{cx - 25.0f, cy - 25.0f}, 12.0f, {cx, cy}, 95.0f, skCols6, skPos6, 6, SkTileMode::kClamp));
					c->drawCircle(cx, cy, 92.0f, p);
				}
			}
		);

		benchmark_canvas_op("120. Repeat & Reflect Gradients (16x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 16; ++i) {
					float x = 40.0f + (i % 4) * 255.0f;
					float y = 40.0f + (i / 4) * 165.0f;
					auto mode = (i % 2 == 0) ? nisaba::SpreadMode::Repeat : nisaba::SpreadMode::Reflect;
					auto g = nisaba::LinearGradient::create(nisaba::Point::from_xy(x, y), nisaba::Point::from_xy(x + 45.0f, y + 35.0f), stops6, mode).value();
					c.fill_rect(nisaba::Rect::from_xywh(x, y, 230.0f, 140.0f).value(), nisaba::Paint(nisaba::Shader(g)));
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 16; ++i) {
					float x = 40.0f + (i % 4) * 255.0f;
					float y = 40.0f + (i / 4) * 165.0f;
					SkPoint pts[2] = {{x, y}, {x + 45.0f, y + 35.0f}};
					SkTileMode tm = (i % 2 == 0) ? SkTileMode::kRepeat : SkTileMode::kMirror;
					p.setShader(SkGradientShader::MakeLinear(pts, skCols6, skPos6, 6, tm));
					c->drawRect(SkRect::MakeXYWH(x, y, 230.0f, 140.0f), p);
				}
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 6: Programmable BlendModes (Suites 121–123)
	// -------------------------------------------------------------------------
	{
		benchmark_canvas_op("121. BlendModes: Multiply+Screen (20x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				for (int i = 0; i < 20; ++i) {
					float cx = 100.0f + (i % 5) * 220.0f;
					float cy = 110.0f + (i / 5) * 165.0f;
					nisaba::Paint p(nisaba::Color::from_rgba8(220, 120 + (i * 7) % 120, 90 + (i * 11) % 150, 210));
					p.blend_mode = (i % 2 == 0) ? nisaba::BlendMode::Multiply : nisaba::BlendMode::Screen;
					c.fill_circle(cx, cy, 80.0f, p);
				}
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 20; ++i) {
					float cx = 100.0f + (i % 5) * 220.0f;
					float cy = 110.0f + (i / 5) * 165.0f;
					p.setColor(SkColorSetARGB(210, 220, 120 + (i * 7) % 120, 90 + (i * 11) % 150));
					p.setBlendMode((i % 2 == 0) ? SkBlendMode::kMultiply : SkBlendMode::kScreen);
					c->drawCircle(cx, cy, 80.0f, p);
				}
			}
		);

		benchmark_canvas_op("122. BlendModes: Overlay+HardLight",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				for (int i = 0; i < 20; ++i) {
					float x = 40.0f + (i % 5) * 205.0f;
					float y = 40.0f + (i / 5) * 165.0f;
					nisaba::Paint p(nisaba::Color::from_rgba8(90 + (i * 13) % 160, 200, 140 + (i * 9) % 100, 220));
					p.blend_mode = (i % 2 == 0) ? nisaba::BlendMode::Overlay : nisaba::BlendMode::HardLight;
					c.fill_round_rect(nisaba::Rect::from_xywh(x, y, 180.0f, 140.0f).value(), 16.0f, 16.0f, p);
				}
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 20; ++i) {
					float x = 40.0f + (i % 5) * 205.0f;
					float y = 40.0f + (i / 5) * 165.0f;
					p.setColor(SkColorSetARGB(220, 90 + (i * 13) % 160, 200, 140 + (i * 9) % 100));
					p.setBlendMode((i % 2 == 0) ? SkBlendMode::kOverlay : SkBlendMode::kHardLight);
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 180.0f, 140.0f), 16.0f, 16.0f), p);
				}
			}
		);

		benchmark_canvas_op("123. BlendModes: Dodge+Diff+Lum (24x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				draw_rich_backdrop_nisaba(c);
				const nisaba::BlendMode modes[3] = {nisaba::BlendMode::ColorDodge, nisaba::BlendMode::Difference, nisaba::BlendMode::Luminosity};
				for (int i = 0; i < 24; ++i) {
					float cx = 90.0f + (i % 6) * 180.0f;
					float cy = 100.0f + (i / 6) * 170.0f;
					nisaba::Paint p(nisaba::Color::from_rgba8(180, 140, 240, 200));
					p.blend_mode = modes[i % 3];
					c.fill_circle(cx, cy, 68.0f, p);
				}
			},
			[&](SkCanvas* c) {
				draw_rich_backdrop_skia(c);
				const SkBlendMode modes[3] = {SkBlendMode::kColorDodge, SkBlendMode::kDifference, SkBlendMode::kLuminosity};
				SkPaint p; p.setAntiAlias(true); p.setColor(SkColorSetARGB(200, 180, 140, 240));
				for (int i = 0; i < 24; ++i) {
					float cx = 90.0f + (i % 6) * 180.0f;
					float cy = 100.0f + (i / 6) * 170.0f;
					p.setBlendMode(modes[i % 3]);
					c->drawCircle(cx, cy, 68.0f, p);
				}
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 7: Path / Stencil Clipping & EvenOdd Fill (Suites 124–126)
	// -------------------------------------------------------------------------
	{
		nisaba::PathBuilder pbClip;
		SkPath skClipStar;
		for (int k = 0; k < 20; ++k) {
			float ang = static_cast<float>(k) * static_cast<float>(M_PI) / 10.0f;
			float rad = (k % 2 == 0) ? 310.0f : 170.0f;
			float px = 540.0f + std::cos(ang) * rad;
			float py = 360.0f + std::sin(ang) * rad;
			if (k == 0) { pbClip.move_to(px, py); skClipStar.moveTo(px, py); }
			else { pbClip.line_to(px, py); skClipStar.lineTo(px, py); }
		}
		pbClip.close(); skClipStar.close();
		auto nClipStar = pbClip.finish().value();

		benchmark_canvas_op("124. Path Stencil Clip + 20 Shapes",
			[&](nisaba::gpu::GpuCanvas& c) {
				c.save();
				c.clip_path(nClipStar);
				for (int i = 0; i < 20; ++i) {
					c.fill_circle(260.0f + (i % 5) * 140.0f, 120.0f + (i / 5) * 150.0f, 95.0f,
						nisaba::Paint(nisaba::Color::from_rgba8(50 + i * 10, 140 + (i * 7) % 100, 255 - i * 8, 220)));
				}
				c.restore();
			},
			[&](SkCanvas* c) {
				c->save();
				c->clipPath(skClipStar, true);
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 20; ++i) {
					p.setColor(SkColorSetARGB(220, 50 + i * 10, 140 + (i * 7) % 100, 255 - i * 8));
					c->drawCircle(260.0f + (i % 5) * 140.0f, 120.0f + (i / 5) * 150.0f, 95.0f, p);
				}
				c->restore();
			}
		);

		auto nClipRR = nisaba::RRect::from_rect_and_radius(nisaba::Rect::from_xywh(140.0f, 80.0f, 800.0f, 560.0f).value(), 48.0f);
		auto nClipOval = nisaba::Rect::from_xywh(220.0f, 60.0f, 640.0f, 600.0f).value();
		SkRRect sClipRR = SkRRect::MakeRectXY(SkRect::MakeXYWH(140.0f, 80.0f, 800.0f, 560.0f), 48.0f, 48.0f);
		SkPath sClipOval; sClipOval.addOval(SkRect::MakeXYWH(220.0f, 60.0f, 640.0f, 600.0f));
		benchmark_canvas_op("125. Nested RRect+Oval Clip + 15 Rects",
			[&](nisaba::gpu::GpuCanvas& c) {
				c.save();
				c.clip_rrect(nClipRR);
				c.clip_oval(nClipOval);
				for (int i = 0; i < 15; ++i) {
					auto r = nisaba::Rect::from_xywh(160.0f + i * 50.0f, 90.0f, 42.0f, 540.0f).value();
					c.fill_rect(r, nisaba::Paint(nisaba::Color::from_rgba8(240 - i * 12, 90 + i * 10, 210, 255)));
				}
				c.restore();
			},
			[&](SkCanvas* c) {
				c->save();
				c->clipRRect(sClipRR, true);
				c->clipPath(sClipOval, true);
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 15; ++i) {
					p.setColor(SkColorSetARGB(255, 240 - i * 12, 90 + i * 10, 210));
					c->drawRect(SkRect::MakeXYWH(160.0f + i * 50.0f, 90.0f, 42.0f, 540.0f), p);
				}
				c->restore();
			}
		);

		std::vector<nisaba::Path> nEvenOdd;
		std::vector<SkPath> sEvenOdd;
		for (int i = 0; i < 10; ++i) {
			float cx = 130.0f + (i % 5) * 205.0f;
			float cy = 190.0f + (i / 5) * 340.0f;
			nisaba::PathBuilder pb;
			SkPath sp;
			sp.setFillType(SkPathFillType::kEvenOdd);
			// Self-intersecting 5-point pentagram + concentric rings
			const int order[5] = {0, 2, 4, 1, 3};
			for (int k = 0; k < 5; ++k) {
				float ang = static_cast<float>(order[k]) * 2.0f * static_cast<float>(M_PI) / 5.0f - static_cast<float>(M_PI) * 0.5f;
				float px = cx + std::cos(ang) * 80.0f;
				float py = cy + std::sin(ang) * 80.0f;
				if (k == 0) { pb.move_to(px, py); sp.moveTo(px, py); }
				else { pb.line_to(px, py); sp.lineTo(px, py); }
			}
			pb.close(); sp.close();
			pb.push_circle(cx, cy, 85.0f); sp.addCircle(cx, cy, 85.0f);
			pb.push_circle(cx, cy, 45.0f); sp.addCircle(cx, cy, 45.0f);
			nEvenOdd.push_back(pb.finish().value());
			sEvenOdd.push_back(sp);
		}
		benchmark_canvas_op("126. EvenOdd Complex Star+Rings (10x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisaba::Paint p(nisaba::Color::from_rgba8(139, 92, 246, 255));
				for (const auto& pt : nEvenOdd) c.fill_path(pt, p, nisaba::FillRule::EvenOdd);
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true); p.setColor(SkColorSetARGB(255, 139, 92, 246));
				for (const auto& pt : sEvenOdd) c->drawPath(pt, p);
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 8: Advanced Text & Arabic/RTL (Suites 127–129)
	// -------------------------------------------------------------------------
	{
		auto* fontSys = gpuCanvas->font_system();
		const char* arabicFontCandidates[] = {
			"/usr/share/fonts/truetype/noto/NotoSansArabic-Bold.ttf",
			"/usr/share/fonts/truetype/noto/NotoNaskhArabic-Bold.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
		};
		const char* arabicFontPath = nullptr;
		for (const char* cand : arabicFontCandidates) {
			if (FILE* f = std::fopen(cand, "rb")) { std::fclose(f); arabicFontPath = cand; break; }
		}
		if (fontSys && arabicFontPath) {
			fontSys->load_font_file(arabicFontPath);
		}
		sk_sp<SkTypeface> skArabicTf = arabicFontPath ? SkTypeface::MakeFromFile(arabicFontPath) : skiaTypeface;

		nisaba::text::Buffer paraBuf(nisaba::text::Metrics(20.0f, 30.0f));
		paraBuf.set_size(960.0f, std::nullopt);
		const char* paraLines[6] = {
			"Nisaba is a sovereign C++20 hardware-accelerated 2D graphics engine engineered for desktop shells.",
			"Zero-copy GPU batching, analytical SDF rounded rectangles, and Dual Kawase backdrop blur pipelines.",
			"Direct comparison against Google Skia Ganesh OpenGL with identical framebuffers and glFinish sync.",
			"Typography shaping supports kerning, multi-line word wrapping, underline/strikethrough decorations.",
			"Vector SVG documents and Lottie animations compile cleanly into hardware draw calls per frame.",
			"Benchmarked continuously across 134 micro-suites covering primitives, layers, blends, and shaders."
		};
		std::string paraFull;
		for (int i = 0; i < 6; ++i) { if (i) paraFull += '\n'; paraFull += paraLines[i]; }
		paraBuf.set_text(paraFull);
		if (fontSys) paraBuf.shape_until_scroll(*fontSys);

		benchmark_canvas_op("127. Multi-Line Text Buffer (6 Lines)",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (fontSys) c.draw_text_buffer(paraBuf, *fontSys, nisaba::Point::from_xy(60.0f, 80.0f), nisaba::Color::WHITE);
			},
			[&](SkCanvas* c) {
				SkFont f(skiaTypeface, 20.0f);
				SkPaint p; p.setAntiAlias(true); p.setColor(SK_ColorWHITE);
				for (int i = 0; i < 6; ++i) {
					c->drawString(paraLines[i], 60.0f, 100.0f + i * 30.0f, f, p);
				}
			}
		);

		nisaba::text::Buffer rtlBuf(nisaba::text::Metrics(26.0f, 44.0f));
		rtlBuf.set_size(960.0f, std::nullopt);
		const char* rtlLines[4] = {
			"مرحبا بكم في محرك الرسوميات نيسابا للعرض السريع على البطاقة الرسومية",
			"يدعم المحرك النصوص العربية واتجاه القراءة من اليمين إلى اليسار بالكامل",
			"مقارنة الأداء الفعلي مع محرك Skia Ganesh عبر اختبارات دقيقة ومتكافئة",
			"Nisaba GPU Engine 2026 — نص عربي مختلط مع English BiDi 12345"
		};
		std::string rtlFull;
		for (int i = 0; i < 4; ++i) { if (i) rtlFull += '\n'; rtlFull += rtlLines[i]; }
		nisaba::text::Attrs rtlAttrs;
		rtlAttrs.set_color(nisaba::text::TextColor::rgb(245, 248, 255));
		rtlBuf.set_text(rtlFull, rtlAttrs);
		if (fontSys) rtlBuf.shape_until_scroll(*fontSys);

		benchmark_canvas_op("128. Arabic / RTL BiDi Text (4x Lines)",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (fontSys) c.draw_text_buffer(rtlBuf, *fontSys, nisaba::Point::from_xy(60.0f, 140.0f), nisaba::Color::WHITE);
			},
			[&](SkCanvas* c) {
				SkFont f(skArabicTf ? skArabicTf : skiaTypeface, 26.0f);
				SkPaint p; p.setAntiAlias(true); p.setColor(SkColorSetARGB(255, 245, 248, 255));
				for (int i = 0; i < 4; ++i) {
					c->drawString(rtlLines[i], 60.0f, 170.0f + i * 44.0f, f, p);
				}
			}
		);

		std::vector<nisaba::text::Buffer> styledBufs;
		const float fontSizes[8] = {12.0f, 16.0f, 20.0f, 24.0f, 28.0f, 32.0f, 36.0f, 42.0f};
		for (int i = 0; i < 8; ++i) {
			nisaba::text::Buffer b(nisaba::text::Metrics(fontSizes[i], fontSizes[i] * 1.3f));
			nisaba::text::Attrs a;
			a.underline = (i % 2 == 0);
			a.strikethrough = (i % 3 == 0);
			a.set_color(nisaba::text::TextColor::rgb(120 + i * 16, 220 - i * 10, 255));
			b.set_text("Styled GPU GlyphAtlas Telemetry Sample 0123456789", a);
			if (fontSys) b.shape_until_scroll(*fontSys);
			styledBufs.push_back(std::move(b));
		}
		benchmark_canvas_op("129. Styled Text + Multi-Size Atlas",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (!fontSys) return;
				float y = 40.0f;
				for (int i = 0; i < 8; ++i) {
					c.draw_text_buffer(styledBufs[i], *fontSys, nisaba::Point::from_xy(60.0f, y), nisaba::Color::WHITE);
					y += fontSizes[i] * 1.8f;
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				SkPaint pLine; pLine.setAntiAlias(true); pLine.setStrokeWidth(1.5f);
				float y = 40.0f;
				for (int i = 0; i < 8; ++i) {
					SkColor col = SkColorSetARGB(255, 120 + i * 16, 220 - i * 10, 255);
					p.setColor(col); pLine.setColor(col);
					SkFont f(skiaTypeface, fontSizes[i]);
					float base = y + fontSizes[i];
					c->drawString("Styled GPU GlyphAtlas Telemetry Sample 0123456789", 60.0f, base, f, p);
					if (i % 2 == 0) c->drawLine(60.0f, base + 3.0f, 700.0f, base + 3.0f, pLine);
					if (i % 3 == 0) c->drawLine(60.0f, base - fontSizes[i] * 0.35f, 700.0f, base - fontSizes[i] * 0.35f, pLine);
					y += fontSizes[i] * 1.8f;
				}
			}
		);

		nisaba::text::Buffer ellRtlBuf(nisaba::text::Metrics(22.0f, 34.0f));
		ellRtlBuf.set_size(420.0f, std::nullopt);
		ellRtlBuf.set_wrap(nisaba::text::Wrap::Word);
		ellRtlBuf.set_align(nisaba::text::Align::Start);
		ellRtlBuf.set_text_direction(nisaba::text::TextDirection::RTL);
		ellRtlBuf.set_overflow(nisaba::text::Overflow::Ellipsis);
		ellRtlBuf.set_max_lines(2);
		ellRtlBuf.set_ellipsis("\xE2\x80\xA6 \xD8\xA7\xD9\x84\xD9\x85\xD8\xB2\xD9\x8A\xD8\xAF");
		ellRtlBuf.set_text("مرحبا بكم في محرك نيسابا للرسوميات والنصوص العربية المتكاملة مع دعم الاتجاه الصريح وعلامة القطع المخصصة English 2026");
		if (fontSys) ellRtlBuf.shape_until_scroll(*fontSys);

		benchmark_canvas_op("129b. Custom RTL Ellipsis + Explicit Direction",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (fontSys) c.draw_text_buffer(ellRtlBuf, *fontSys, nisaba::Point::from_xy(60.0f, 120.0f), nisaba::Color::WHITE);
			},
			[&](SkCanvas* c) {
				SkFont f(skArabicTf ? skArabicTf : skiaTypeface, 22.0f);
				SkPaint p; p.setAntiAlias(true); p.setColor(SK_ColorWHITE);
				c->drawString("مرحبا بكم في محرك نيسابا للرسوميات والنصوص", 60.0f, 146.0f, f, p);
				c->drawString("… المزيد", 60.0f, 180.0f, f, p);
			}
		);

		nisaba::text::Buffer phase2Buf(nisaba::text::Metrics(28.0f, 40.0f));
		auto p2Grad = nisaba::LinearGradient::create(
			nisaba::Point::from_xy(60.0f, 0.0f), nisaba::Point::from_xy(420.0f, 0.0f),
			{nisaba::GradientStop::create(0.0f, nisaba::Color::from_rgba8(255, 80, 120, 255)),
			 nisaba::GradientStop::create(1.0f, nisaba::Color::from_rgba8(60, 210, 255, 255))}
		);
		nisaba::Paint p2Fg{nisaba::Shader{*p2Grad}};
		nisaba::text::Attrs p2Attrs;
		p2Attrs.set_foreground_paint(p2Fg)
		       .set_underline(true)
		       .set_decoration_mode(nisaba::text::DecorationMode::Gaps)
		       .set_decoration_color(nisaba::text::TextColor::rgb(255, 190, 60));
		phase2Buf.add_text("Typography ", p2Attrs);
		phase2Buf.add_placeholder(24.0f, 24.0f, nisaba::text::PlaceholderAlignment::Middle);
		phase2Buf.add_text(" gypq يرجى التوقيع", p2Attrs);
		if (fontSys) phase2Buf.shape_until_scroll(*fontSys);

		benchmark_canvas_op("129c. Gradient Text + Gaps Underline + Placeholder",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (fontSys) c.draw_text_buffer(phase2Buf, *fontSys, nisaba::Point::from_xy(60.0f, 140.0f), nisaba::Color::WHITE);
			},
			[&](SkCanvas* c) {
				SkFont f(skiaTypeface, 28.0f);
				SkPaint p; p.setAntiAlias(true); p.setColor(SK_ColorWHITE);
				c->drawString("Typography gypq", 60.0f, 168.0f, f, p);
			}
		);

		nisaba::text::Buffer phase3Buf(nisaba::text::Metrics(26.0f, 38.0f));
		nisaba::text::StrutStyle p3Strut;
		p3Strut.enabled = true;
		p3Strut.font_size = 26.0f;
		p3Strut.height_multiplier = 1.35f;
		p3Strut.force_strut_height = true;
		p3Strut.half_leading = true;
		phase3Buf.set_strut_style(p3Strut);
		nisaba::text::Attrs p3Attrs;
		p3Attrs.add_font_feature("tnum", 1)
		       .add_font_feature("zero", 1)
		       .add_font_variation("wght", 750.0f)
		       .set_color(nisaba::text::TextColor::rgb(235, 245, 255));
		phase3Buf.set_text("09:42:15 Variable Typography مرحبا", p3Attrs);
		if (fontSys) phase3Buf.shape_until_scroll(*fontSys);

		benchmark_canvas_op("129d. Variable Font + OT Features + StrutStyle",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (fontSys) c.draw_text_buffer(phase3Buf, *fontSys, nisaba::Point::from_xy(60.0f, 140.0f), nisaba::Color::WHITE);
			},
			[&](SkCanvas* c) {
				SkFont f(skiaTypeface, 26.0f);
				SkPaint p; p.setAntiAlias(true); p.setColor(SK_ColorWHITE);
				c->drawString("09:42:15 Variable Typography", 60.0f, 166.0f, f, p);
			}
		);

		nisaba::text::ParagraphCache phase4Cache(32);
		nisaba::text::Attrs p4Attrs;
		p4Attrs.set_color(nisaba::text::TextColor::rgb(235, 245, 255));
		std::shared_ptr<nisaba::text::Buffer> phase4CachedBuf;
		if (fontSys) {
			phase4CachedBuf = phase4Cache.get_or_layout(
				*fontSys, "Cached Interactive Paragraph مرحبا 2026",
				p4Attrs, nisaba::text::Metrics(24.0f, 34.0f), 420.0f,
				nisaba::text::Wrap::Word, nisaba::text::Align::Left
			);
		}
		nisaba::Paint p4Bg(nisaba::Color::from_rgba8(55, 75, 130, 255));
		int p4Frame = 0;

		benchmark_canvas_op("129e. Incremental Updates + ParagraphCache",
			[&](nisaba::gpu::GpuCanvas& c) {
				if (fontSys) {
					auto hit = phase4Cache.get_or_layout(
						*fontSys, "Cached Interactive Paragraph مرحبا 2026",
						p4Attrs, nisaba::text::Metrics(24.0f, 34.0f), 420.0f,
						nisaba::text::Wrap::Word, nisaba::text::Align::Left
					);
					hit->update_text_align((p4Frame++ & 1) ? nisaba::text::Align::Center : nisaba::text::Align::Left);
					hit->update_background_paint(0, 6, p4Bg);
					c.draw_text_buffer(*hit, *fontSys, nisaba::Point::from_xy(60.0f, 140.0f), nisaba::Color::WHITE);
				}
			},
			[&](SkCanvas* c) {
				SkFont f(skiaTypeface, 24.0f);
				SkPaint p; p.setAntiAlias(true); p.setColor(SK_ColorWHITE);
				c->drawString("Cached Interactive Paragraph", 60.0f, 164.0f, f, p);
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 9: Advanced Images, Mipmaps, 3D Perspective & Vertices (130–132)
	// -------------------------------------------------------------------------
	{
		auto pix512 = nisaba::Pixmap::create(512, 512).value();
		uint8_t* pdata = pix512.data_mut();
		for (int y = 0; y < 512; ++y) {
			for (int x = 0; x < 512; ++x) {
				bool chk = ((x / 8) + (y / 8)) % 2 == 0;
				size_t idx = (static_cast<size_t>(y) * 512 + x) * 4;
				pdata[idx + 0] = chk ? static_cast<uint8_t>((x * 255) / 512) : 30;
				pdata[idx + 1] = chk ? static_cast<uint8_t>((y * 255) / 512) : 200;
				pdata[idx + 2] = chk ? 240 : 120;
				pdata[idx + 3] = 255;
			}
		}
		auto nTex512 = nisaba::gpu::GpuTexture::create_from_pixmap(gpuDevice, pix512.as_ref());
		SkImageInfo info512 = SkImageInfo::Make(512, 512, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
		SkPixmap skPm512(info512, pix512.data(), 512 * 4);
		sk_sp<SkImage> skImg512 = SkImage::MakeRasterCopy(skPm512)->makeTextureImage(dContext.get(), GrMipmapped::kYes);

		const auto src512 = nisaba::Rect::from_xywh(0.0f, 0.0f, 512.0f, 512.0f).value();
		benchmark_canvas_op("130. Heavy Downscale Mipmaps (16x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisaba::PixmapPaint pp;
				pp.quality = nisaba::FilterQuality::Bilinear;
				for (int i = 0; i < 16; ++i) {
					float x = 80.0f + (i % 8) * 115.0f;
					float y = 200.0f + (i / 8) * 180.0f;
					auto dst = nisaba::Rect::from_xywh(x, y, 32.0f, 32.0f).value();
					c.draw_pixmap_rect(pix512.as_ref(), src512, dst, pp);
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				SkSamplingOptions samp(SkFilterMode::kLinear, SkMipmapMode::kLinear);
				for (int i = 0; i < 16; ++i) {
					float x = 80.0f + (i % 8) * 115.0f;
					float y = 200.0f + (i / 8) * 180.0f;
					c->drawImageRect(skImg512, SkRect::MakeXYWH(x, y, 32.0f, 32.0f), samp, &p);
				}
			}
		);

		benchmark_canvas_op("131. 3D Perspective Quads (8x Cards)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 8; ++i) {
					float px = 160.0f + (i % 4) * 240.0f;
					float py = 200.0f + (i / 4) * 300.0f;
					auto t3d = nisaba::Transform4x4::from_camera_orbit(px, py, 22.0f, -28.0f + i * 8.0f, 4.0f, 650.0f)
						* nisaba::Transform4x4::from_translate(px - 80.0f, py - 80.0f, 0.0f)
						* nisaba::Transform4x4::from_scale(160.0f / 512.0f, 160.0f / 512.0f, 1.0f);
					c.draw_pixmap_3d(pix512.as_ref(), t3d, 1.0f, nisaba::BlendMode::SourceOver);
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				SkSamplingOptions samp(SkFilterMode::kLinear, SkMipmapMode::kLinear);
				for (int i = 0; i < 8; ++i) {
					float px = 160.0f + (i % 4) * 240.0f;
					float py = 200.0f + (i / 4) * 300.0f;
					auto t3d = nisaba::Transform4x4::from_camera_orbit(px, py, 22.0f, -28.0f + i * 8.0f, 4.0f, 650.0f)
						* nisaba::Transform4x4::from_translate(px - 80.0f, py - 80.0f, 0.0f)
						* nisaba::Transform4x4::from_scale(160.0f / 512.0f, 160.0f / 512.0f, 1.0f);
					SkM44 m44 = SkM44::RowMajor(t3d.m.data());
					c->save();
					c->concat(m44);
					c->drawImageRect(skImg512, SkRect::MakeXYWH(0, 0, 512, 512), samp, &p);
					c->restore();
				}
			}
		);

		// Build 16x8 grid (256 triangles) Gouraud + textured mesh
		const uint32_t gCols = 17, gRows = 9;
		std::vector<nisaba::Point> nPts, nUvs;
		std::vector<nisaba::Color> nCols;
		std::vector<SkPoint> sPts, sUvs;
		std::vector<SkColor> sCols;
		for (uint32_t r = 0; r < gRows; ++r) {
			for (uint32_t col = 0; col < gCols; ++col) {
				float u = static_cast<float>(col) / (gCols - 1);
				float v = static_cast<float>(r) / (gRows - 1);
				float x = 80.0f + u * 920.0f;
				float y = 80.0f + v * 560.0f + std::sin(u * 6.28318f + v * 3.14f) * 18.0f;
				nPts.push_back(nisaba::Point::from_xy(x, y));
				nUvs.push_back(nisaba::Point::from_xy(u, v));
				uint8_t cr = static_cast<uint8_t>(u * 255.0f);
				uint8_t cg = static_cast<uint8_t>(v * 255.0f);
				uint8_t cb = static_cast<uint8_t>((1.0f - u) * 255.0f);
				nCols.push_back(nisaba::Color::from_rgba8(cr, cg, cb, 255));
				sPts.push_back({x, y});
				sUvs.push_back({u * 512.0f, v * 512.0f});
				sCols.push_back(SkColorSetARGB(255, cr, cg, cb));
			}
		}
		auto nVertsColor = nisaba::Vertices::create_grid(gCols, gRows, nPts, &nCols, nullptr);
		auto nVertsTex = nisaba::Vertices::create_grid(gCols, gRows, nPts, nullptr, &nUvs);
		std::vector<uint16_t> sIdx;
		sIdx.reserve(nVertsColor.indices.size());
		for (uint32_t idx : nVertsColor.indices) sIdx.push_back(static_cast<uint16_t>(idx));
		auto skVertsColor = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode, static_cast<int>(sPts.size()),
			sPts.data(), nullptr, sCols.data(), static_cast<int>(sIdx.size()), sIdx.data());
		auto skVertsTex = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode, static_cast<int>(sPts.size()),
			sPts.data(), sUvs.data(), nullptr, static_cast<int>(sIdx.size()), sIdx.data());
		auto skTexShader = skImg512->makeShader(SkSamplingOptions(SkFilterMode::kLinear));

		benchmark_canvas_op("132. Vertex Mesh Gouraud+Tex (256 Tri)",
			[&](nisaba::gpu::GpuCanvas& c) {
				c.draw_vertices(nVertsColor);
				c.draw_textured_vertices(nVertsTex, nTex512, nisaba::BlendMode::SourceOver, 0.5f);
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				c->drawVertices(skVertsColor, SkBlendMode::kDst, p);
				p.setShader(skTexShader); p.setAlphaf(0.5f);
				c->drawVertices(skVertsTex, SkBlendMode::kModulate, p);
			}
		);
	}

	// -------------------------------------------------------------------------
	// Category 10: Dashed Strokes & DRRect (Suites 133–134)
	// -------------------------------------------------------------------------
	{
		std::vector<nisaba::Path> nDashPaths;
		std::vector<SkPath> sDashPaths;
		for (int i = 0; i < 8; ++i) {
			float y = 60.0f + i * 80.0f;
			nisaba::PathBuilder pb;
			SkPath sp;
			pb.move_to(60.0f, y); sp.moveTo(60.0f, y);
			pb.cubic_to(320.0f, y - 55.0f, 740.0f, y + 55.0f, 1020.0f, y);
			sp.cubicTo(320.0f, y - 55.0f, 740.0f, y + 55.0f, 1020.0f, y);
			nDashPaths.push_back(pb.finish().value());
			sDashPaths.push_back(sp);
		}
		nisaba::Stroke nDashStroke(4.0f);
		nDashStroke.line_cap = nisaba::LineCap::Round;
		nDashStroke.dash = nisaba::StrokeDash::create({14.0f, 8.0f, 4.0f, 8.0f}, 0.0f);
		const SkScalar skDashIntervals[4] = {14.0f, 8.0f, 4.0f, 8.0f};
		auto skDashEff = SkDashPathEffect::Make(skDashIntervals, 4, 0.0f);

		benchmark_canvas_op("133. Dashed Strokes (8x Cubic+8x Rect)",
			[&](nisaba::gpu::GpuCanvas& c) {
				nisaba::Paint p(nisaba::Color::from_rgba8(56, 189, 248, 255));
				for (const auto& pt : nDashPaths) c.stroke_path(pt, p, nDashStroke);
				for (int i = 0; i < 8; ++i) {
					auto r = nisaba::Rect::from_xywh(80.0f + i * 118.0f, 260.0f, 95.0f, 200.0f).value();
					c.stroke_round_rect(r, 12.0f, 12.0f, p, nDashStroke);
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true); p.setStyle(SkPaint::kStroke_Style);
				p.setStrokeWidth(4.0f); p.setStrokeCap(SkPaint::kRound_Cap);
				p.setColor(SkColorSetARGB(255, 56, 189, 248));
				p.setPathEffect(skDashEff);
				for (const auto& pt : sDashPaths) c->drawPath(pt, p);
				for (int i = 0; i < 8; ++i) {
					c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(80.0f + i * 118.0f, 260.0f, 95.0f, 200.0f), 12.0f, 12.0f), p);
				}
			}
		);

		benchmark_canvas_op("134. Nested DRRect Frames (25x)",
			[&](nisaba::gpu::GpuCanvas& c) {
				for (int i = 0; i < 25; ++i) {
					float x = 50.0f + (i % 5) * 200.0f;
					float y = 30.0f + (i / 5) * 135.0f;
					auto outer = nisaba::RRect::from_rect_and_radius(nisaba::Rect::from_xywh(x, y, 175.0f, 115.0f).value(), 22.0f);
					auto inner = nisaba::RRect::from_rect_and_radius(nisaba::Rect::from_xywh(x + 14.0f, y + 14.0f, 147.0f, 87.0f).value(), 12.0f);
					c.fill_drrect(outer, inner, nisaba::Paint(nisaba::Color::from_rgba8(80 + i * 6, 180, 255 - i * 5, 255)));
				}
			},
			[&](SkCanvas* c) {
				SkPaint p; p.setAntiAlias(true);
				for (int i = 0; i < 25; ++i) {
					float x = 50.0f + (i % 5) * 200.0f;
					float y = 30.0f + (i / 5) * 135.0f;
					SkRRect outer = SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, 175.0f, 115.0f), 22.0f, 22.0f);
					SkRRect inner = SkRRect::MakeRectXY(SkRect::MakeXYWH(x + 14.0f, y + 14.0f, 147.0f, 87.0f), 12.0f, 12.0f);
					p.setColor(SkColorSetARGB(255, 80 + i * 6, 180, 255 - i * 5));
					c->drawDRRect(outer, inner, p);
				}
			}
		);
	}

	// Detailed Statistical Reporting & Speedup Analysis
	// =========================================================================
	std::cout << "\n\n";
	std::cout << "===================================================================================================================================================\n";
	std::cout << "                                NISABA GPU vs GOOGLE SKIA (GANESH GL) — GRANULAR BENCHMARK RESULTS                                \n";
	std::cout << "===================================================================================================================================================\n";
	std::cout << std::left << std::setw(42) << "Layer Operation Name"
	          << std::right << std::setw(11) << "Nisaba CPU"
	          << std::setw(11) << "Nisaba GPU"
	          << std::setw(13) << "Nisaba Total"
	          << std::setw(11) << "Skia CPU"
	          << std::setw(11) << "Skia GPU"
	          << std::setw(13) << "Skia Total"
	          << std::setw(18) << "CPU Winner"
	          << std::setw(20) << "Total Winner" << "\n";
	std::cout << "---------------------------------------------------------------------------------------------------------------------------------------------------\n";

	double total_n_cpu = 0, total_n_gpu = 0, total_n_all = 0;
	double total_s_cpu = 0, total_s_gpu = 0, total_s_all = 0;
	int nisaba_wins = 0;
	int skia_wins = 0;

	for (const auto& s : stats) {
		total_n_cpu += s.nisaba_cpu_us;
		total_n_gpu += s.nisaba_gpu_us;
		total_n_all += s.nisaba_total_us;
		total_s_cpu += s.skia_cpu_us;
		total_s_gpu += s.skia_gpu_us;
		total_s_all += s.skia_total_us;

		// Total verdict
		double total_ratio = (s.nisaba_total_us > 0) ? (s.skia_total_us / s.nisaba_total_us) : 1.0;
		std::string total_verdict;
		if (total_ratio > 1.05) {
			std::ostringstream ss;
			ss << std::fixed << std::setprecision(2) << total_ratio << "x Nisaba";
			total_verdict = ss.str();
			nisaba_wins++;
		} else if (total_ratio < 0.95) {
			std::ostringstream ss;
			ss << std::fixed << std::setprecision(2) << (1.0 / total_ratio) << "x Skia";
			total_verdict = ss.str();
			skia_wins++;
		} else {
			total_verdict = "Tie (~1.0x)";
		}

		// CPU verdict
		double cpu_ratio = (s.nisaba_cpu_us > 0) ? (s.skia_cpu_us / s.nisaba_cpu_us) : 1.0;
		std::string cpu_verdict;
		if (cpu_ratio > 1.05) {
			std::ostringstream ss;
			ss << std::fixed << std::setprecision(1) << cpu_ratio << "x Nisaba";
			cpu_verdict = ss.str();
		} else if (cpu_ratio < 0.95) {
			std::ostringstream ss;
			ss << std::fixed << std::setprecision(1) << (1.0 / cpu_ratio) << "x Skia";
			cpu_verdict = ss.str();
		} else {
			cpu_verdict = "Tie";
		}

		std::ostringstream sn_cpu, sn_gpu, sn_all, ss_cpu, ss_gpu, ss_all;
		sn_cpu << std::fixed << std::setprecision(1) << s.nisaba_cpu_us << " µs";
		sn_gpu << std::fixed << std::setprecision(1) << s.nisaba_gpu_us << " µs";
		sn_all << std::fixed << std::setprecision(1) << s.nisaba_total_us << " µs";
		ss_cpu << std::fixed << std::setprecision(1) << s.skia_cpu_us << " µs";
		ss_gpu << std::fixed << std::setprecision(1) << s.skia_gpu_us << " µs";
		ss_all << std::fixed << std::setprecision(1) << s.skia_total_us << " µs";

		std::cout << std::left << std::setw(42) << s.name
		          << std::right << std::setw(11) << sn_cpu.str()
		          << std::setw(11) << sn_gpu.str()
		          << std::setw(13) << sn_all.str()
		          << std::setw(11) << ss_cpu.str()
		          << std::setw(11) << ss_gpu.str()
		          << std::setw(13) << ss_all.str()
		          << std::setw(18) << cpu_verdict
		          << std::setw(20) << total_verdict << "\n";
	}

	std::cout << "---------------------------------------------------------------------------------------------------------------------------------------------------\n";
	std::ostringstream tot_n_cpu, tot_n_gpu, tot_n_all, tot_s_cpu, tot_s_gpu, tot_s_all;
	tot_n_cpu << std::fixed << std::setprecision(1) << total_n_cpu << " µs";
	tot_n_gpu << std::fixed << std::setprecision(1) << total_n_gpu << " µs";
	tot_n_all << std::fixed << std::setprecision(1) << total_n_all << " µs";
	tot_s_cpu << std::fixed << std::setprecision(1) << total_s_cpu << " µs";
	tot_s_gpu << std::fixed << std::setprecision(1) << total_s_gpu << " µs";
	tot_s_all << std::fixed << std::setprecision(1) << total_s_all << " µs";

	double tot_cpu_ratio = (total_n_cpu > 0) ? (total_s_cpu / total_n_cpu) : 1.0;
	std::ostringstream ss_tot_cpu;
	if (tot_cpu_ratio >= 1.0) ss_tot_cpu << std::fixed << std::setprecision(2) << tot_cpu_ratio << "x Nisaba";
	else ss_tot_cpu << std::fixed << std::setprecision(2) << (1.0 / tot_cpu_ratio) << "x Skia";

	double total_ratio = total_s_all / total_n_all;
	std::ostringstream ss_tot;
	if (total_ratio >= 1.0) ss_tot << std::fixed << std::setprecision(2) << total_ratio << "x Nisaba";
	else ss_tot << std::fixed << std::setprecision(2) << (1.0 / total_ratio) << "x Skia";

	std::cout << std::left << std::setw(42) << "TOTAL FRAME OVERHEAD (ALL SUITES)"
	          << std::right << std::setw(11) << tot_n_cpu.str()
	          << std::setw(11) << tot_n_gpu.str()
	          << std::setw(13) << tot_n_all.str()
	          << std::setw(11) << tot_s_cpu.str()
	          << std::setw(11) << tot_s_gpu.str()
	          << std::setw(13) << tot_s_all.str()
	          << std::setw(18) << ss_tot_cpu.str()
	          << std::setw(20) << ss_tot.str() << "\n";
	std::cout << "===================================================================================================================================================\n";
	std::cout << "SUMMARY: Total Frame Performance: Nisaba won in " << nisaba_wins << " suites | Skia won in " << skia_wins
	          << " suites | Ties: " << (static_cast<int>(stats.size()) - nisaba_wins - skia_wins) << "\n";
	std::cout << "         Overall Frame Throughput (End-to-End): " << ss_tot.str() << "\n";
	std::cout << "         Overall CPU Command Submission:       " << ss_tot_cpu.str() << "\n";
	std::cout << "===================================================================================================================================================\n\n";

	nisabaCtx->deleteImage(nisabaTex);
	gpuCanvas.reset();
	nisabaSurface.reset();
	gpuDevice.reset();
	nisabaCtx.reset();
	skiaSurface.reset();
	dContext.reset();
	window.reset();
	platform.reset();
	return 0;
}
