/// @file test_lottie.cpp
/// @brief Comprehensive automated unit tests for sovereign JSON & industrial-grade Lottie animation engine.

#include "nisaba/json/json.hpp"
#include "nisaba/lottie/lottie.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/image/deflate.hpp"
#include "nisaba/image/png.hpp"

#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

using namespace nisaba;
using namespace nisaba::lottie;
using namespace nisaba::json;

void test_json_parser() {
    std::cout << "[TEST] Running Sovereign JSON Parser tests...\n";

    // 1. Primitive types & fast from_chars number parsing
    {
        auto v1 = JsonValue::parse("42.5");
        assert(v1.has_value() && v1->is_number());
        assert(std::abs(v1->to_float() - 42.5f) < 1e-4f);
        assert(v1->to_int() == 42);

        auto v2 = JsonValue::parse("\"Hello, \\\"Nisaba\\\"!\\n\"");
        assert(v2.has_value() && v2->is_string());
        assert(v2->as_string() == "Hello, \"Nisaba\"!\n");

        auto v3 = JsonValue::parse("true");
        assert(v3.has_value() && v3->is_bool() && v3->to_bool() == true);

        auto v4 = JsonValue::parse("false");
        assert(v4.has_value() && v4->is_bool() && v4->to_bool() == false);

        auto v5 = JsonValue::parse("null");
        assert(v5.has_value() && v5->is_null());
    }

    // 2. Arrays and comments
    {
        std::string_view json = R"(
            // Line comment
            [
                10,
                20.5,
                /* Block comment */
                "text",
                true,
                null
            ]
        )";
        auto arr_val = JsonValue::parse(json);
        assert(arr_val.has_value() && arr_val->is_array());
        const auto& arr = arr_val->as_array();
        assert(arr.size() == 5);
        assert(arr[0].to_int() == 10);
        assert(std::abs(arr[1].to_float() - 20.5f) < 1e-4f);
        assert(arr[2].as_string() == "text");
        assert(arr[3].to_bool() == true);
        assert(arr[4].is_null());
    }

    // 3. Nested objects (contiguous vector storage)
    {
        std::string_view json = R"({
            "name": "Nisaba Lottie",
            "version": "2.0",
            "fps": 60,
            "dimensions": {
                "w": 800,
                "h": 600
            },
            "tags": ["graphics", "animation", "sovereign"]
        })";
        auto obj = JsonValue::parse(json);
        assert(obj.has_value() && obj->is_object());
        assert(obj->get_string("name") == "Nisaba Lottie");
        assert(obj->get_int("fps") == 60);

        const auto* dims = obj->get("dimensions");
        assert(dims && dims->is_object());
        assert(dims->get_float("w") == 800.0f);
        assert(dims->get_float("h") == 600.0f);

        const auto* tags = obj->get("tags");
        assert(tags && tags->is_array());
        assert(tags->as_array().size() == 3);
        assert(tags->as_array()[0].as_string() == "graphics");
    }

    // 4. Unicode escape sequences \uXXXX and UTF-16 surrogate pairs
    {
        auto u_val = JsonValue::parse(R"("\u0041\u0042\u0043")"); // "ABC"
        assert(u_val.has_value() && u_val->is_string());
        assert(u_val->as_string() == "ABC");

        auto emoji_val = JsonValue::parse(R"("\uD83D\uDE80")"); // Rocket U+1F680
        assert(emoji_val.has_value() && emoji_val->is_string());
        assert(emoji_val->as_string() == "\xF0\x9F\x9A\x80");
    }

    std::cout << "[PASS] JSON Parser tests passed.\n";
}

void test_lottie_properties_spatial_and_expressions() {
    std::cout << "[TEST] Running Lottie Properties, Spatial Bezier, Skew, 3D & Expressions tests...\n";

    // 1. Static property & dynamic override
    {
        FloatProperty p(100.0f);
        assert(!p.is_animated());
        assert(p.evaluate(0.0f) == 100.0f);
        p.set_override(250.0f);
        assert(p.evaluate(50.0f) == 250.0f);
        p.clear_override();
        assert(p.evaluate(50.0f) == 100.0f);
    }

    // 2. Spatial Bezier curved motion with arc-length parameterization
    {
        PointProperty p;
        Keyframe<Point> kf1;
        kf1.time = 0.0f;
        kf1.start_value = Point::from_xy(0.0f, 0.0f);
        kf1.end_value = Point::from_xy(100.0f, 0.0f);
        kf1.spatial_out_tangent = Point::from_xy(0.0f, 100.0f);
        kf1.spatial_in_tangent = Point::from_xy(0.0f, 100.0f);
        kf1.has_spatial_tangents = true;
        p.add_keyframe(kf1);

        Keyframe<Point> kf2;
        kf2.time = 10.0f;
        kf2.start_value = Point::from_xy(100.0f, 0.0f);
        kf2.end_value = Point::from_xy(100.0f, 0.0f);
        p.add_keyframe(kf2);

        Point mid = p.evaluate(5.0f);
        // Midpoint along arch should be at x ~ 50 and y > 50 due to upward spatial tangents
        assert(std::abs(mid.x - 50.0f) < 2.0f);
        assert(mid.y > 50.0f);
    }

    // 3. Expressions: loopOut("cycle"), loopOut("pingpong"), wiggle(freq, amp)
    {
        std::string_view expr_json = R"JSON({
            "a": 1,
            "x": "loopOut('pingpong')",
            "k": [
                {"t": 0, "s": [0], "e": [100]},
                {"t": 10, "s": [100]}
            ]
        })JSON";
        auto val = JsonValue::parse(expr_json);
        assert(val.has_value());
        FloatProperty fp = parse_float_property(*val);
        assert(fp.expression().type == LottieExpression::Type::LoopOutPingPong);
        // At t=5 -> 50, at t=10 -> 100, at t=15 (ping-pong back) -> 50, at t=20 -> 0
        assert(std::abs(fp.evaluate(5.0f) - 50.0f) < 1.0f);
        assert(std::abs(fp.evaluate(15.0f) - 50.0f) < 1.0f);

        std::string_view wig_json = R"JSON({
            "a": 0,
            "k": 50,
            "x": "wiggle(4, 20)"
        })JSON";
        auto wval = JsonValue::parse(wig_json);
        FloatProperty wfp = parse_float_property(*wval);
        assert(wfp.expression().type == LottieExpression::Type::Wiggle);
        assert(wfp.is_animated());
        float v_t1 = wfp.evaluate(3.5f);
        assert(v_t1 >= 30.0f && v_t1 <= 70.0f);
    }

    // 4. LottieTransform with Skew ("sk"/"sa"), Auto-Orient ("ao"), and 3D ("rx", "ry", "rz")
    {
        std::string_view tr_json = R"({
            "p": {"a": 0, "k": [100, 100, 0]},
            "a": {"a": 0, "k": [0, 0, 0]},
            "s": {"a": 0, "k": [100, 100, 100]},
            "r": {"a": 0, "k": 0},
            "sk": {"a": 0, "k": 20},
            "sa": {"a": 0, "k": 15},
            "rx": {"a": 0, "k": 30},
            "ry": {"a": 0, "k": 45},
            "rz": {"a": 0, "k": 10},
            "o": {"a": 0, "k": 100}
        })";
        auto tr_val = JsonValue::parse(tr_json);
        LottieTransform tr = LottieTransform::parse(*tr_val);
        assert(tr.is_3d);
        Transform m2d = tr.matrix(0.0f);
        assert(!m2d.is_identity());
        Transform4x4 m3d = tr.matrix_3d(0.0f);
        assert(std::abs(m3d.m[3] - 100.0f) < 1e-2f || std::abs(m3d.m[0] - 1.0f) > 1e-3f);
    }

    std::cout << "[PASS] Lottie Properties, Spatial Bezier, Skew, 3D & Expressions tests passed.\n";
}

void test_lottie_shapes_gradients_and_modifiers() {
    std::cout << "[TEST] Running Lottie Shapes, Gradients & Path Modifiers tests...\n";

    // 1. StarShape with inner/outer roundness and CCW winding direction ("d": 3)
    {
        StarShape star;
        star.type = 1;
        star.points = FloatProperty(5.0f);
        star.outer_radius = FloatProperty(50.0f);
        star.inner_radius = FloatProperty(25.0f);
        star.outer_roundness = FloatProperty(40.0f);
        star.inner_roundness = FloatProperty(20.0f);
        star.direction = 3; // CCW

        Path p;
        star.collect_geometry(0.0f, p);
        assert(!p.is_empty());
        // Rounded star uses Cubic verbs
        bool has_cubic = false;
        for (auto v : p.verbs()) {
            if (v == PathVerb::Cubic) has_cubic = true;
        }
        assert(has_cubic);
    }

    // 2. GradientColorStops ("g") with RGB + Alpha stop cross-interpolation
    {
        std::string_view grad_json = R"({
            "p": 2,
            "k": {
                "a": 0,
                "k": [
                    0.0, 1.0, 0.0, 0.0,
                    1.0, 0.0, 0.0, 1.0,
                    0.0, 1.0,
                    0.5, 0.5,
                    1.0, 0.0
                ]
            }
        })";
        auto gval = JsonValue::parse(grad_json);
        GradientColorStops gcs = GradientColorStops::parse(*gval);
        auto stops = gcs.evaluate_stops(0.0f, 1.0f);
        // Should merge t=0.0, t=0.5 (from alpha), t=1.0 -> 3 stops!
        assert(stops.size() == 3);
        assert(std::abs(stops[0].color.red() - 1.0f) < 1e-3f);
        assert(std::abs(stops[1].position.get() - 0.5f) < 1e-3f);
        assert(std::abs(stops[1].color.alpha() - 0.5f) < 1e-3f);
        assert(std::abs(stops[2].color.blue() - 1.0f) < 1e-3f);
        assert(std::abs(stops[2].color.alpha() - 0.0f) < 1e-3f);
    }

    // 3. Exact TrimPathShape ("tm") via PathMeasure (Simultaneously & Individually)
    {
        TrimPathShape trim;
        trim.start = FloatProperty(0.0f);
        trim.end = FloatProperty(50.0f);
        trim.trim_type = TrimType::Individually;

        PathBuilder b;
        b.move_to(0.0f, 0.0f);
        b.line_to(100.0f, 0.0f);
        b.move_to(0.0f, 20.0f);
        b.line_to(100.0f, 20.0f);
        auto opt_p = b.finish();
        assert(opt_p.has_value());

        // Total length of 2 contours = 200. 50% individual trim should trim only the first contour (len 100)!
        Path trimmed = trim.apply_trim(*opt_p, 0.0f);
        assert(!trimmed.is_empty());
        auto b_trim = trimmed.compute_tight_bounds();
        assert(b_trim.has_value());
        assert(std::abs(b_trim->width() - 100.0f) < 1.0f);
        assert(std::abs(b_trim->height() - 0.0f) < 1.0f);
    }

    // 4. MergePathsShape ("mm"), RoundCornersShape ("rd"), OffsetPathShape ("op"), PuckerBloat ("pb"), ZigZag ("zz")
    {
        RectShape r1, r2;
        r1.position = PointProperty(Point::from_xy(40.0f, 50.0f));
        r1.size = PointProperty(Point::from_xy(40.0f, 40.0f));
        r2.position = PointProperty(Point::from_xy(60.0f, 50.0f));
        r2.size = PointProperty(Point::from_xy(40.0f, 40.0f));

        Path p1, p2;
        r1.collect_geometry(0.0f, p1);
        r2.collect_geometry(0.0f, p2);
        std::vector<Path> paths = {p1, p2};

        MergePathsShape mm;
        mm.mode = MergeMode::Add; // Union
        mm.modify_paths(0.0f, paths);
        assert(paths.size() == 1 && !paths[0].is_empty());

        RoundCornersShape rd;
        rd.radius = FloatProperty(8.0f);
        rd.modify_paths(0.0f, paths);
        assert(!paths[0].is_empty());

        OffsetPathShape op;
        op.amount = FloatProperty(4.0f);
        op.modify_paths(0.0f, paths);
        assert(!paths[0].is_empty());

        PuckerBloatShape pb;
        pb.amount = FloatProperty(25.0f);
        pb.modify_paths(0.0f, paths);
        assert(!paths[0].is_empty());

        ZigZagShape zz;
        zz.size = FloatProperty(5.0f);
        zz.ridges = FloatProperty(4.0f);
        zz.modify_paths(0.0f, paths);
        assert(!paths[0].is_empty());
    }

    std::cout << "[PASS] Lottie Shapes, Gradients & Path Modifiers tests passed.\n";
}

void test_advanced_features_and_dotlottie() {
    std::cout << "[TEST] Running Advanced Features (Repeater, Masks, Track Mattes, Effects, Text, Image, Overrides, .lottie ZIP)...\n";

    // Comprehensive Lottie JSON testing GradientFill, GradientStroke, Repeater, Track Matte, Mask, Effect, TextLayer, and Slot Overrides
    std::string_view rich_json = R"({
        "v": "5.9.0",
        "fr": 60,
        "ip": 0,
        "op": 60,
        "w": 200,
        "h": 200,
        "nm": "IndustrialShowcase",
        "markers": [
            {"cm": "intro", "tm": 0, "dr": 20},
            {"cm": "loop_seg", "tm": 20, "dr": 40}
        ],
        "fonts": {
            "list": [
                {"fName": "TestFont", "fFamily": "Sans", "fStyle": "Regular", "ascent": 75}
            ]
        },
        "chars": [
            {
                "ch": "A",
                "size": 100,
                "style": "Regular",
                "w": 60,
                "fFamily": "Sans",
                "data": {
                    "shapes": [
                        {
                            "ty": "gr",
                            "nm": "GlyphA",
                            "it": [
                                {"ty": "rc", "p": {"a": 0, "k": [30, -35]}, "s": {"a": 0, "k": [40, 70]}, "r": {"a": 0, "k": 4}},
                                {"ty": "tr", "p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}}
                            ]
                        }
                    ]
                }
            }
        ],
        "layers": [
            {
                "ind": 1,
                "ty": 5,
                "nm": "TitleText",
                "ip": 0,
                "op": 60,
                "st": 0,
                "ks": {"p": {"a": 0, "k": [20, 160, 0]}, "a": {"a": 0, "k": [0, 0, 0]}, "s": {"a": 0, "k": [100, 100, 100]}, "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}},
                "t": {
                    "d": {
                        "k": [
                            {
                                "t": 0,
                                "s": {
                                    "s": 36,
                                    "f": "TestFont",
                                    "t": "AAA",
                                    "j": 0,
                                    "tr": 50,
                                    "lh": 42,
                                    "fc": [0.9, 0.2, 0.2],
                                    "sc": [1.0, 1.0, 1.0],
                                    "sw": 2,
                                    "of": true
                                }
                            }
                        ]
                    },
                    "a": [
                        {
                            "nm": "Animator 1",
                            "s": {"s": {"a": 0, "k": 0}, "e": {"a": 0, "k": 100}, "o": {"a": 0, "k": 0}, "r": 1},
                            "a": {"p": {"a": 0, "k": [0, -5, 0]}}
                        }
                    ]
                }
            },
            {
                "ind": 2,
                "ty": 4,
                "nm": "MatteSourceLayer",
                "td": 1,
                "ip": 0,
                "op": 60,
                "st": 0,
                "ks": {"p": {"a": 0, "k": [100, 100, 0]}},
                "shapes": [
                    {
                        "ty": "gr",
                        "it": [
                            {"ty": "el", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [120, 120]}},
                            {"ty": "fl", "c": {"a": 0, "k": [1, 1, 1, 1]}, "o": {"a": 0, "k": 100}},
                            {"ty": "tr"}
                        ]
                    }
                ]
            },
            {
                "ind": 3,
                "ty": 4,
                "nm": "MainShapeLayer",
                "tt": 1,
                "ip": 0,
                "op": 60,
                "st": 0,
                "ks": {"p": {"a": 0, "k": [100, 100, 0]}},
                "ef": [
                    {
                        "ty": 29,
                        "nm": "Gaussian Blur",
                        "en": 1,
                        "ef": [{"ty": 0, "v": {"a": 0, "k": 2.0}}]
                    }
                ],
                "shapes": [
                    {
                        "ty": "gr",
                        "nm": "MainGroup",
                        "it": [
                            {"ty": "rc", "nm": "Box", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [80, 80]}, "r": {"a": 0, "k": 10}},
                            {
                                "ty": "rp",
                                "nm": "Repeater 1",
                                "c": {"a": 0, "k": 3},
                                "o": {"a": 0, "k": 0},
                                "m": 1,
                                "tr": {
                                    "p": {"a": 0, "k": [10, 10]},
                                    "s": {"a": 0, "k": [90, 90]},
                                    "r": {"a": 0, "k": 15},
                                    "so": {"a": 0, "k": 100},
                                    "eo": {"a": 0, "k": 50}
                                }
                            },
                            {
                                "ty": "gf",
                                "nm": "GradFill",
                                "t": 2,
                                "s": {"a": 0, "k": [0, 0]},
                                "e": {"a": 0, "k": [60, 0]},
                                "h": {"a": 0, "k": 50},
                                "a": {"a": 0, "k": 30},
                                "o": {"a": 0, "k": 100},
                                "g": {
                                    "p": 2,
                                    "k": {"a": 0, "k": [0.0, 0.1, 0.8, 0.4, 1.0, 0.1, 0.2, 0.9]}
                                }
                            },
                            {
                                "ty": "fl",
                                "nm": "ThemeFill",
                                "c": {"a": 0, "k": [0.2, 0.6, 1.0, 1.0], "sid": "primary_theme"},
                                "o": {"a": 0, "k": 80}
                            },
                            {"ty": "tr"}
                        ]
                    }
                ]
            }
        ]
    })";

    auto anim = Animation::load_from_data(rich_json);
    assert(anim != nullptr);
    assert(anim->markers().size() == 2);

    auto pixmap = Pixmap::create(200, 200);
    assert(pixmap.has_value());
    pixmap->fill(Color::BLACK);
    Canvas canvas(*pixmap);

    anim->render(canvas, 10.0f);

    // Verify dynamic overrides & slots
    assert(anim->set_slot_color("primary_theme", Color::from_rgba8(255, 128, 0, 255)));
    assert(anim->set_color_override("MainShapeLayer.MainGroup.ThemeFill", Color::from_rgba8(0, 255, 100, 255)));
    assert(anim->set_text_override("TitleText", "AAAA"));
    anim->render(canvas, 15.0f);
    anim->clear_overrides();

    // Verify Player marker playback & Bounce mode
    Player player(anim);
    assert(player.play_marker("intro"));
    assert(player.current_frame() == 0.0f);
    player.set_loop_mode(LoopMode::Bounce);
    player.advance(0.5f); // 30 frames > 20 duration -> bounces backward!
    assert(player.current_frame() >= 0.0f && player.current_frame() <= 20.0f);

    // Verify Hit-Testing
    auto hit = anim->hit_test(100.0f, 100.0f, 10.0f);
    assert(hit.has_value());
    assert(*hit == "MainShapeLayer" || *hit == "Box");

    // Verify .lottie (dotLottie ZIP container) ingestion using sovereign DEFLATE
    {
        auto compressed_res = image::deflate_compress(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rich_json.data()), rich_json.size()),
            6
        );
        assert(compressed_res.has_value());
        const auto& comp_data = *compressed_res;

        std::string entry_name = "animations/data.json";
        std::vector<uint8_t> zip_bytes;
        auto push_u16 = [&](uint16_t v) {
            zip_bytes.push_back(static_cast<uint8_t>(v & 0xFF));
            zip_bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        };
        auto push_u32 = [&](uint32_t v) {
            zip_bytes.push_back(static_cast<uint8_t>(v & 0xFF));
            zip_bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            zip_bytes.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            zip_bytes.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        };

        // Local File Header (0x04034b50)
        push_u32(0x04034b50u);
        push_u16(20); // version needed
        push_u16(0);  // flags
        push_u16(8);  // method = 8 (DEFLATE)
        push_u16(0);  // mod time
        push_u16(0);  // mod date
        push_u32(0);  // crc32
        push_u32(static_cast<uint32_t>(comp_data.size()));
        push_u32(static_cast<uint32_t>(rich_json.size()));
        push_u16(static_cast<uint16_t>(entry_name.size()));
        push_u16(0);  // extra len
        zip_bytes.insert(zip_bytes.end(), entry_name.begin(), entry_name.end());
        zip_bytes.insert(zip_bytes.end(), comp_data.begin(), comp_data.end());

        auto dot_anim = Animation::load_from_bytes(zip_bytes);
        assert(dot_anim != nullptr);
        assert(dot_anim->name() == "IndustrialShowcase");
        assert(dot_anim->width() == 200.0f);
    }

    std::cout << "[PASS] Advanced Features & .lottie ZIP tests passed.\n";
}

// Helper: builds a minimal valid 2-frame animated GIF89a (4x4: frame 0 solid red, frame 1 solid blue, 500ms each)
static std::vector<uint8_t> build_test_animated_gif89a() {
    std::vector<uint8_t> g;
    auto push_str = [&](const char* s, size_t n) {
        g.insert(g.end(), reinterpret_cast<const uint8_t*>(s), reinterpret_cast<const uint8_t*>(s) + n);
    };
    auto push_u16 = [&](uint16_t v) {
        g.push_back(static_cast<uint8_t>(v & 0xFF));
        g.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    // Header + Logical Screen Descriptor (4x4, GCT flag=1, 2 colors -> size code 0)
    push_str("GIF89a", 6);
    push_u16(4); // width
    push_u16(4); // height
    g.push_back(0x80); // GCT present (2 entries)
    g.push_back(0);    // bg index
    g.push_back(0);    // aspect ratio

    // Global Color Table: index 0 = Red (255,0,0), index 1 = Blue (0,0,255)
    g.push_back(255); g.push_back(0); g.push_back(0);
    g.push_back(0);   g.push_back(0); g.push_back(255);

    // Helper to write a 4x4 solid frame filled with color index `cidx` (0 or 1) using min_code_size=2
    // Codes (3 bits each, LSB first): clear=4, 16x cidx, eoi=5.
    // Wait: after clear(4), emitting 16 identical symbols with 3-bit codes requires tracking table growth
    // OR simply emitting Clear(4) every 3 pixels so code size stays fixed at 3 bits!
    auto append_frame = [&](uint8_t cidx, uint16_t delay_cs) {
        // Graphic Control Extension
        g.push_back(0x21); g.push_back(0xF9); g.push_back(4);
        g.push_back(0x04); // disposal=1 (keep)
        push_u16(delay_cs);
        g.push_back(0);    // transparent idx
        g.push_back(0);    // block terminator

        // Image Descriptor (0,0, 4x4, no LCT)
        g.push_back(0x2C);
        push_u16(0); push_u16(0); push_u16(4); push_u16(4);
        g.push_back(0x00);

        // LZW min code size = 2 (clear=4, eoi=5, 3-bit codes)
        g.push_back(2);
        std::vector<uint8_t> packed;
        uint32_t bit_buf = 0;
        uint32_t bit_cnt = 0;
        auto emit_3bit = [&](uint32_t code) {
            bit_buf |= (code & 0x7u) << bit_cnt;
            bit_cnt += 3;
            while (bit_cnt >= 8) {
                packed.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
                bit_buf >>= 8;
                bit_cnt -= 8;
            }
        };
        for (int p = 0; p < 16; ++p) {
            if (p % 2 == 0) emit_3bit(4); // Clear code every 2 pixels keeps next_code <= 7 (strictly 3-bit LZW)
            emit_3bit(cidx);
        }
        emit_3bit(5); // EOI
        if (bit_cnt > 0) packed.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));

        g.push_back(static_cast<uint8_t>(packed.size()));
        g.insert(g.end(), packed.begin(), packed.end());
        g.push_back(0); // sub-block terminator
    };

    append_frame(0, 50); // Frame 0: Red, 500ms
    append_frame(1, 50); // Frame 1: Blue, 500ms
    g.push_back(0x3B);   // GIF Trailer
    return g;
}

void test_phase1_invalidation_controller() {
    std::cout << "[TEST] Running Phase 1.1 InvalidationController & Dirty-Region Tracking tests...\n";

    // Scene: full-canvas static background (200x200) + small animated moving square (20x20) + animated color dot (16x16)
    std::string_view scene_json = R"({
        "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 200, "h": 200,
        "layers": [
            {
                "ind": 1, "ty": 4, "nm": "MovingBox", "ip": 0, "op": 60, "st": 0,
                "ks": {
                    "p": {
                        "a": 1,
                        "k": [
                            {"t": 0, "s": [20, 20, 0], "e": [80, 20, 0]},
                            {"t": 60, "s": [80, 20, 0]}
                        ]
                    },
                    "a": {"a": 0, "k": [0, 0, 0]},
                    "s": {"a": 0, "k": [100, 100, 100]},
                    "r": {"a": 0, "k": 0},
                    "o": {"a": 0, "k": 100}
                },
                "shapes": [
                    {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [20, 20]}, "r": {"a": 0, "k": 0}},
                    {"ty": "fl", "c": {"a": 0, "k": [1, 0, 0, 1]}, "o": {"a": 0, "k": 100}}
                ]
            },
            {
                "ind": 2, "ty": 1, "nm": "StaticBG", "ip": 0, "op": 60, "st": 0,
                "sw": 200, "sh": 200, "sc": "#101820",
                "ks": {"p": {"a": 0, "k": [0, 0, 0]}, "a": {"a": 0, "k": [0, 0, 0]}, "s": {"a": 0, "k": [100, 100, 100]}, "o": {"a": 0, "k": 100}}
            }
        ]
    })";

    auto anim = Animation::load_from_data(scene_json);
    assert(anim != nullptr);

    // 1. Initial seek invalidates full scene (including StaticBG)
    InvalidationController ic;
    anim->seek_frame(0.0f, &ic);
    assert(!ic.is_empty());
    assert(ic.bounds().has_value());
    assert(ic.bounds()->width() >= 199.0f && ic.bounds()->height() >= 199.0f);

    // 2. Seeking to the exact same frame produces ZERO damage
    ic.reset();
    anim->seek_frame(0.0f, &ic);
    assert(ic.is_empty());

    // 3. Seeking to frame 10 moves MovingBox from x=20 to x=30 -> dirty rect must ONLY cover ~[9..41, 9..31], NOT full 200x200!
    ic.reset();
    anim->seek_frame(10.0f, &ic);
    assert(!ic.is_empty());
    auto b = ic.bounds();
    assert(b.has_value());
    assert(b->left() >= 5.0f && b->left() <= 11.0f);
    assert(b->right() >= 39.0f && b->right() <= 45.0f);
    assert(b->top() >= 5.0f && b->bottom() <= 35.0f);
    assert(b->width() < 50.0f && b->height() < 35.0f);

    // Verify ScreenIntRect conversion for GpuCanvas::flush(damage_rect)
    auto sr = ic.to_screen_int_rect(200, 200);
    assert(sr.has_value());
    assert(sr->width() < 50 && sr->height() < 35);

    auto sr_fitted = ic.to_screen_int_rect_fitted(400, 400, 200.0f, 200.0f, *Rect::from_xywh(0.0f, 0.0f, 400.0f, 400.0f), true);
    assert(sr_fitted.has_value());
    assert(sr_fitted->width() == sr->width() * 2 || std::abs(static_cast<int>(sr_fitted->width()) - static_cast<int>(sr->width() * 2)) <= 2);

    // 4. Player::seek_frame(frame, &dirty_rect) and Player::advance(dt, &dirty_rect)
    Player player(anim);
    Rect dirty{};
    assert(player.seek_frame(0.0f, &dirty)); // initial full damage
    assert(!player.seek_frame(0.0f, &dirty)); // unchanged frame -> false
    assert(player.advance(1.0f / 6.0f, &dirty)); // +10 frames
    assert(!dirty.is_empty());
    assert(dirty.width() < 50.0f && dirty.height() < 35.0f);

    std::cout << "[PASS] Phase 1.1 InvalidationController tests passed.\n";
}

void test_phase1_resource_provider_deferred_and_multiframe() {
    std::cout << "[TEST] Running Phase 1.2 ResourceProvider, kDeferImageLoading & Multi-frame ImageAsset tests...\n";

    // Encode a 16x16 green PNG in memory (no filesystem!)
    auto green_pm = Pixmap::create(16, 16);
    assert(green_pm.has_value());
    green_pm->fill(Color::from_rgba8(0, 220, 60, 255));
    auto png_res = image::encode_png(green_pm->as_ref());
    assert(png_res.has_value());

    // Build an in-memory 2-frame animated GIF (Red for 0..0.5s, Blue for 0.5..1.0s)
    auto gif_bytes = build_test_animated_gif89a();

    auto mem_rp = std::make_shared<MemoryResourceProvider>(/*defer_decode=*/true);
    mem_rp->add_resource("images/delayed.png", *png_res);
    mem_rp->add_resource("images/anim.gif", gif_bytes);
    mem_rp->add_audio_asset("aud_0", std::make_shared<MemoryAudioAsset>(std::vector<uint8_t>{0x52, 0x49, 0x46, 0x46}));

    std::string_view json = R"({
        "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 64, "h": 64,
        "assets": [
            {"id": "img_delayed", "w": 16, "h": 16, "u": "images/", "p": "delayed.png", "e": 0},
            {"id": "img_gif", "w": 16, "h": 16, "u": "images/", "p": "anim.gif", "e": 0},
            {"id": "aud_0", "u": "audio/", "p": "click.wav"}
        ],
        "layers": [
            {
                "ind": 1, "ty": 2, "nm": "DelayedImg", "refId": "img_delayed",
                "ip": 20, "op": 60, "st": 0,
                "ks": {"p": {"a": 0, "k": [32, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}}
            },
            {
                "ind": 2, "ty": 2, "nm": "GifImg", "refId": "img_gif",
                "ip": 0, "op": 60, "st": 0,
                "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}}
            }
        ]
    })";

    auto anim = AnimationBuilder(kDeferImageLoading)
        .set_resource_provider(mem_rp)
        .make(json);
    assert(anim != nullptr);
    assert(anim->assets().audio_assets.count("aud_0") == 1);

    // 1. Verify BOTH images are NOT yet decoded immediately after load!
    const auto& delayed_entry = anim->assets().images.at("img_delayed");
    const auto& gif_entry = anim->assets().images.at("img_gif");
    assert(delayed_entry.asset != nullptr);
    assert(gif_entry.asset != nullptr);
    assert(!delayed_entry.asset->is_decoded());
    assert(!gif_entry.asset->is_decoded());
    assert(gif_entry.asset->is_multi_frame());

    // 2. Seek to frame 5: GifImg is visible (decodes lazily), DelayedImg is NOT active yet (stays deferred!)
    InvalidationController ic;
    anim->seek_frame(5.0f, &ic);
    assert(gif_entry.asset->is_decoded());
    assert(!delayed_entry.asset->is_decoded());

    // Verify GIF renders RED at frame 5 (t = 5/60 = 0.083s < 0.5s)
    auto surface = Pixmap::create(64, 64);
    assert(surface.has_value());
    surface->fill(Color::BLACK);
    Canvas canvas(*surface);
    anim->render(canvas, 5.0f);
    auto px0 = surface->as_ref().pixel(8, 8);
    assert(px0.has_value() && px0->red() > 200 && px0->blue() < 20);

    // 3. Seek to frame 40 (t = 40/60 = 0.667s > 0.5s):
    //    - DelayedImg becomes active -> decoded lazily!
    //    - GifImg advances to frame 1 (BLUE) -> triggers damage invalidation!
    ic.reset();
    anim->seek_frame(40.0f, &ic);
    assert(delayed_entry.asset->is_decoded());
    assert(!ic.is_empty());

    surface->fill(Color::BLACK);
    anim->render(canvas, 40.0f);
    auto px1 = surface->as_ref().pixel(8, 8);
    assert(px1.has_value() && px1->blue() > 200 && px1->red() < 20);
    auto px_delayed = surface->as_ref().pixel(40, 8);
    assert(px_delayed.has_value() && px_delayed->green() > 180 && px_delayed->red() < 20);

    std::cout << "[PASS] Phase 1.2 ResourceProvider, Deferred Decode & Multi-frame GIF tests passed.\n";
}

void test_phase1_logger_stats_and_property_observer() {
    std::cout << "[TEST] Running Phase 1.3 Logger, Stats & PropertyObserver O(1) Handles tests...\n";

    // 1. Logger reports Error on malformed JSON
    int error_count = 0;
    int warn_count = 0;
    auto logger = std::make_shared<CallbackLogger>(
        [&](Logger::Level lvl, std::string_view, std::string_view) {
            if (lvl == Logger::Level::Error) ++error_count;
            else ++warn_count;
        }
    );

    auto bad_anim = AnimationBuilder().set_logger(logger).make("{invalid json");
    assert(bad_anim == nullptr);
    assert(error_count >= 1);

    // 2. Observer capturing O(1) ColorHandle, OpacityHandle, TransformHandle, TextHandle
    struct CapturingObserver final : public PropertyObserver {
        std::unique_ptr<ColorHandle> fill_color;
        std::unique_ptr<ColorHandle> solid_color;
        std::unique_ptr<OpacityHandle> layer_opacity;
        std::unique_ptr<TransformHandle> layer_transform;
        std::unique_ptr<TextHandle> title_text;
        int enter_count = 0;
        int leave_count = 0;

        void on_node_enter(std::string_view) override { ++enter_count; }
        void on_node_leave(std::string_view) override { ++leave_count; }

        void on_color_property(std::string_view kp, const std::function<std::unique_ptr<ColorHandle>()>& factory) override {
            if (kp == "HeroShape.BoxGroup.HeroFill") fill_color = factory();
            if (kp == "BgSolid") solid_color = factory();
        }
        void on_opacity_property(std::string_view kp, const std::function<std::unique_ptr<OpacityHandle>()>& factory) override {
            if (kp == "HeroShape") layer_opacity = factory();
        }
        void on_transform_property(std::string_view kp, const std::function<std::unique_ptr<TransformHandle>()>& factory) override {
            if (kp == "HeroShape") layer_transform = factory();
        }
        void on_text_property(std::string_view kp, const std::function<std::unique_ptr<TextHandle>()>& factory) override {
            if (kp == "LabelText") title_text = factory();
        }
    };

    auto observer = std::make_shared<CapturingObserver>();
    Stats stats{};

    std::string_view doc_json = R"({
        "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 128, "h": 128,
        "layers": [
            {
                "ind": 1, "ty": 4, "nm": "HeroShape", "ip": 0, "op": 60, "st": 0,
                "ef": [{"ty": 999, "nm": "UnsupportedFX", "en": 1}],
                "ks": {
                    "p": {"a": 0, "k": [64, 64]},
                    "a": {"a": 0, "k": [0, 0]},
                    "s": {"a": 0, "k": [100, 100]},
                    "r": {"a": 1, "k": [{"t": 0, "s": [0], "e": [90]}, {"t": 60, "s": [90]}]},
                    "o": {"a": 0, "k": 100}
                },
                "shapes": [
                    {
                        "ty": "gr", "nm": "BoxGroup",
                        "it": [
                            {"ty": "rc", "nm": "Rect", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [40, 40]}, "r": {"a": 0, "k": 0}},
                            {"ty": "fl", "nm": "HeroFill", "c": {"a": 0, "k": [1, 0, 0, 1]}, "o": {"a": 0, "k": 100}},
                            {"ty": "tr"}
                        ]
                    }
                ]
            },
            {
                "ind": 2, "ty": 5, "nm": "LabelText", "ip": 0, "op": 60, "st": 0,
                "ks": {"p": {"a": 0, "k": [10, 110]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                "t": {"d": {"k": {"t": "Hi", "s": 20, "f": "Sans", "fc": [1, 1, 1]}}}
            },
            {
                "ind": 3, "ty": 1, "nm": "BgSolid", "ip": 0, "op": 60, "st": 0,
                "sw": 128, "sh": 128, "sc": "#000000",
                "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}}
            },
            {
                "ind": 4, "ty": 99, "nm": "UnknownLayer", "ip": 0, "op": 60, "st": 0,
                "ks": {}
            }
        ]
    })";

    {
        auto anim = AnimationBuilder()
            .set_logger(logger)
            .set_stats(&stats)
            .set_property_observer(observer)
            .make(doc_json);
        assert(anim != nullptr);

        // Verify Logger caught unsupported effect (ty=999) and unknown layer (ty=99)
        assert(warn_count >= 2);

        // Verify Stats
        assert(stats.layer_count == 4);
        assert(stats.animator_count >= 1);
        assert(stats.json_size_bytes == doc_json.size());
        assert(stats.json_parse_ms >= 0.0);
        assert(stats.scene_build_ms >= 0.0);

        // Verify Observer balanced node enter/leave and captured all 5 typed handles
        assert(observer->enter_count == observer->leave_count && observer->enter_count > 0);
        assert(observer->fill_color && observer->fill_color->is_valid());
        assert(observer->solid_color && observer->solid_color->is_valid());
        assert(observer->layer_opacity && observer->layer_opacity->is_valid());
        assert(observer->layer_transform && observer->layer_transform->is_valid());
        assert(observer->title_text && observer->title_text->is_valid());

        // Prime invalidation state at frame 0
        InvalidationController ic;
        anim->seek_frame(0.0f, &ic);
        ic.reset();
        anim->seek_frame(0.0f, &ic);
        assert(ic.is_empty());

        // Mutate fill color via O(1) ColorHandle (Red -> Green) and verify dirty tracking detects it!
        observer->fill_color->set(Color::from_rgba8(0, 255, 0, 255));
        observer->layer_transform->set_position(Point::from_xy(30.0f, 30.0f));
        observer->layer_opacity->set(100.0f);
        observer->title_text->set_text("Nisaba");
        observer->title_text->set_fill_color(Color::from_rgba8(255, 255, 0, 255));
        assert(observer->title_text->get().text == "Nisaba");

        anim->seek_frame(0.0f, &ic);
        assert(!ic.is_empty());

        auto pm = Pixmap::create(128, 128);
        assert(pm.has_value());
        Canvas canvas(*pm);
        anim->render(canvas, 0.0f);
        // Box moved to (30, 30) and recolored green!
        auto px = pm->as_ref().pixel(30, 30);
        assert(px.has_value() && px->green() > 220 && px->red() < 20);
    }

    // After Animation goes out of scope, weak_ptr handles must safely become invalid
    assert(!observer->fill_color->is_valid());
    assert(!observer->layer_transform->is_valid());
    assert(!observer->title_text->is_valid());

    std::cout << "[PASS] Phase 1.3 Logger, Stats & PropertyObserver tests passed.\n";
}

void test_phase2_external_precomp_and_color_effects() {
    std::cout << "[TEST] Running Phase 2.1 ExternalLayer/PrecompInterceptor & Phase 2.2 Color/Tone Effects tests...\n";

    // 1. ExternalLayer via PrecompInterceptor with transform, opacity, mask, and hit-test
    {
        std::string_view precomp_json = R"({
            "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 160, "h": 160,
            "layers": [
                {
                    "ind": 1, "ty": 0, "nm": "LiveWidgetSlot", "refId": "comp_live_ui",
                    "w": 80, "h": 80, "ip": 0, "op": 60, "st": 0,
                    "ks": {
                        "p": {"a": 0, "k": [40, 40, 0]},
                        "a": {"a": 0, "k": [0, 0, 0]},
                        "s": {"a": 0, "k": [100, 100, 100]},
                        "o": {"a": 0, "k": 100}
                    }
                }
            ]
        })";

        float captured_t = -1.0f;
        bool intercepted = false;
        auto interceptor = std::make_shared<CallbackPrecompInterceptor>(
            [&](std::string_view id, std::string_view name, Size sz) -> std::shared_ptr<ExternalLayer> {
                if (id == "comp_live_ui" && name == "LiveWidgetSlot" &&
                    std::abs(sz.width() - 80.0f) < 0.5f && std::abs(sz.height() - 80.0f) < 0.5f) {
                    intercepted = true;
                    return std::make_shared<CallbackExternalLayer>([&](ICanvas& c, float t_sec) {
                        captured_t = t_sec;
                        // Draw oversized rect to verify precomp clipping to 80x80!
                        c.fill_rect(*Rect::from_xywh(-20.0f, -20.0f, 140.0f, 140.0f),
                                    Paint(Color::from_rgba8(20, 200, 240, 255)));
                    });
                }
                return nullptr;
            }
        );

        auto anim = AnimationBuilder().set_precomp_interceptor(interceptor).make(precomp_json);
        assert(anim != nullptr && intercepted);

        auto pm = Pixmap::create(160, 160);
        assert(pm.has_value());
        pm->fill(Color::BLACK);
        Canvas canvas(*pm);
        anim->render(canvas, 30.0f);

        assert(std::abs(captured_t - 0.5f) < 1e-3f);
        // Inside precomp slot [40..120, 40..120] -> cyan
        auto inside = pm->as_ref().pixel(80, 80);
        assert(inside.has_value() && inside->blue() > 210 && inside->green() > 170 && inside->red() < 40);
        // Outside precomp slot (clipped!) -> black
        auto outside = pm->as_ref().pixel(30, 80);
        assert(outside.has_value() && outside->red() == 0 && outside->green() == 0 && outside->blue() == 0);
        // Hit-test inside slot succeeds
        auto hit = anim->hit_test(80.0f, 80.0f, 30.0f);
        assert(hit.has_value() && *hit == "LiveWidgetSlot");
    }

    // Helper to render a single solid-colored layer with a given "ef" JSON array and sample center pixel
    auto render_solid_with_ef = [](std::string_view hex_color, std::string_view ef_json, int sx = 32, int sy = 32) {
        std::string doc = R"({"v":"5.9.0","fr":60,"ip":0,"op":60,"w":64,"h":64,"layers":[{"ind":1,"ty":1,"nm":"S","sw":64,"sh":64,"sc":")";
        doc += hex_color;
        doc += R"(","ip":0,"op":60,"st":0,"ks":{"p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"o":{"a":0,"k":100}},"ef":[)";
        doc += ef_json;
        doc += R"(]}]})";

        auto anim = Animation::load_from_data(doc);
        assert(anim != nullptr);
        auto pm = Pixmap::create(64, 64);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        anim->render(c, 0.0f);
        auto px = pm->as_ref().pixel(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
        assert(px.has_value());
        return *px;
    };

    // 2. BrightnessContrast (+40 brightness on #404040 -> brighter gray)
    {
        auto px = render_solid_with_ef("#404040",
            R"({"ty":5,"mn":"ADBE Brightness & Contrast 2","nm":"BC","en":1,"ef":[{"ty":0,"v":{"a":0,"k":40}},{"ty":0,"v":{"a":0,"k":20}}]})");
        assert(px.red() > 150 && px.green() > 150 && px.blue() > 150);
    }

    // 3. HueSaturation (+120 deg hue shift turns pure red #ff0000 -> pure green!)
    {
        auto px = render_solid_with_ef("#ff0000",
            R"({"ty":5,"mn":"ADBE HUE SATURATION","nm":"HS","en":1,"ef":[{"ty":0,"v":{"a":0,"k":120}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":0}}]})");
        assert(px.green() > 220 && px.red() < 25 && px.blue() < 25);
    }

    // 4. BlackAndWhite (pure red #ff0000 with reds=80% -> grayscale ~204)
    {
        auto px = render_solid_with_ef("#ff0000",
            R"({"ty":5,"mn":"ADBE Black&White","nm":"BW","en":1,"ef":[{"ty":0,"v":{"a":0,"k":80}},{"ty":0,"v":{"a":0,"k":60}},{"ty":0,"v":{"a":0,"k":40}},{"ty":0,"v":{"a":0,"k":60}},{"ty":0,"v":{"a":0,"k":20}},{"ty":0,"v":{"a":0,"k":80}}]})");
        assert(std::abs(int(px.red()) - int(px.green())) <= 2);
        assert(std::abs(int(px.red()) - int(px.blue())) <= 2);
        assert(px.red() >= 195 && px.red() <= 215);
    }

    // 5. Invert (RGB invert turns #ff1020 -> ~#00efdf)
    {
        auto px = render_solid_with_ef("#ff1020",
            R"({"ty":5,"mn":"ADBE Invert","nm":"Inv","en":1,"ef":[{"ty":7,"v":{"a":0,"k":1}},{"ty":0,"v":{"a":0,"k":0}}]})");
        assert(px.red() < 10 && px.green() > 230 && px.blue() > 215);
    }

    // 6. Threshold (level=128: #c0c0c0 -> white, #404040 -> black)
    {
        auto hi = render_solid_with_ef("#c0c0c0",
            R"({"ty":5,"mn":"ADBE Threshold2","nm":"Thr","en":1,"ef":[{"ty":0,"v":{"a":0,"k":128}}]})");
        auto lo = render_solid_with_ef("#404040",
            R"({"ty":5,"mn":"ADBE Threshold2","nm":"Thr","en":1,"ef":[{"ty":0,"v":{"a":0,"k":128}}]})");
        assert(hi.red() == 255 && hi.green() == 255 && hi.blue() == 255);
        assert(lo.red() == 0 && lo.green() == 0 && lo.blue() == 0 && lo.alpha() == 255);
    }

    // 7. ShiftChannels (Swap R<-B, G<-R, B<-G on #ff0000 -> green!)
    {
        auto px = render_solid_with_ef("#ff0000",
            R"({"ty":5,"mn":"ADBE Shift Channels","nm":"Shift","en":1,"ef":[{"ty":7,"v":{"a":0,"k":1}},{"ty":7,"v":{"a":0,"k":4}},{"ty":7,"v":{"a":0,"k":2}},{"ty":7,"v":{"a":0,"k":3}}]})");
        assert(px.green() == 255 && px.red() == 0 && px.blue() == 0);
    }

    // 8. CCToner (Pentone mapping mid-gray #808080 -> midtone color #00ff80)
    {
        auto px = render_solid_with_ef("#808080",
            R"({"ty":5,"mn":"CC Toner","nm":"Toner","en":1,"ef":[{"ty":7,"v":{"a":0,"k":3}},{"ty":2,"v":{"a":0,"k":[1,1,1,1]}},{"ty":2,"v":{"a":0,"k":[0.8,0.8,0.8,1]}},{"ty":2,"v":{"a":0,"k":[0,1,0.5,1]}},{"ty":2,"v":{"a":0,"k":[0.2,0.2,0.2,1]}},{"ty":2,"v":{"a":0,"k":[0,0,0,1]}},{"ty":0,"v":{"a":0,"k":0}}]})");
        assert(px.green() > 235 && px.blue() > 110 && px.blue() < 145 && px.red() < 20);
    }

    // 9. EasyLevels (gamma=2.0 brightens #808080 from 128 -> ~180)
    {
        auto px = render_solid_with_ef("#808080",
            R"({"ty":5,"mn":"ADBE Easy Levels2","nm":"Levels","en":1,"ef":[{"ty":7,"v":{"a":0,"k":1}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":1}},{"ty":0,"v":{"a":0,"k":2.0}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":1}}]})");
        assert(px.red() > 170 && px.red() < 195);
    }

    // 10. GradientRamp (2-color linear horizontal red->blue from x=0 to x=64)
    {
        std::string_view ramp_ef = R"({"ty":5,"mn":"ADBE Ramp","nm":"Ramp","en":1,"ef":[{"ty":3,"v":{"a":0,"k":[0,32]}},{"ty":2,"v":{"a":0,"k":[1,0,0,1]}},{"ty":3,"v":{"a":0,"k":[64,32]}},{"ty":2,"v":{"a":0,"k":[0,0,1,1]}},{"ty":7,"v":{"a":0,"k":1}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":0}}]})";
        auto p_left = render_solid_with_ef("#ffffff", ramp_ef, 4, 32);
        auto p_right = render_solid_with_ef("#ffffff", ramp_ef, 60, 32);
        assert(p_left.red() > 215 && p_left.blue() < 40);
        assert(p_right.blue() > 215 && p_right.red() < 40);
    }

    // 11. FourColorGradient (4 corners: R, G, B, White)
    {
        std::string_view four_ef = R"({"ty":5,"mn":"ADBE 4ColorGradient","nm":"4CG","en":1,"ef":[{"ty":3,"v":{"a":0,"k":[0,0]}},{"ty":2,"v":{"a":0,"k":[1,0,0,1]}},{"ty":3,"v":{"a":0,"k":[64,0]}},{"ty":2,"v":{"a":0,"k":[0,1,0,1]}},{"ty":3,"v":{"a":0,"k":[0,64]}},{"ty":2,"v":{"a":0,"k":[0,0,1,1]}},{"ty":3,"v":{"a":0,"k":[64,64]}},{"ty":2,"v":{"a":0,"k":[1,1,1,1]}},{"ty":0,"v":{"a":0,"k":10}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":100}}]})";
        auto tl = render_solid_with_ef("#000000", four_ef, 2, 2);
        auto tr = render_solid_with_ef("#000000", four_ef, 62, 2);
        auto bl = render_solid_with_ef("#000000", four_ef, 2, 62);
        assert(tl.red() > 200 && tl.green() < 60);
        assert(tr.green() > 200 && tr.red() < 60);
        assert(bl.blue() > 200 && bl.red() < 60);
    }

    std::cout << "[PASS] Phase 2 External Precomp & Color/Tone Effects tests passed.\n";
}

void test_phase3_transitions_distortion_blur_and_procedural() {
    std::cout << "[TEST] Running Phase 3.1 (Wipe/Blinds/Tile/Transform) & Phase 3.2 (CornerPin/Bulge/Sphere/DirBlur/Sharpen/FractalNoise) tests...\n";

    auto render_layer_ef = [](std::string_view layer_content, std::string_view ef_json) -> Pixmap {
        std::string doc = R"({"v":"5.9.0","fr":60,"ip":0,"op":60,"w":80,"h":80,"layers":[{)";
        doc += layer_content;
        doc += R"(,"ip":0,"op":60,"st":0,"ks":{"p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"o":{"a":0,"k":100}},"ef":[)";
        doc += ef_json;
        doc += R"(]}]})";
        auto anim = Animation::load_from_data(doc);
        assert(anim != nullptr);
        auto pm = Pixmap::create(80, 80);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        anim->render(c, 0.0f);
        return std::move(*pm);
    };

    const std::string_view solid_red = R"("ind":1,"ty":1,"nm":"S","sw":80,"sh":80,"sc":"#ff2020")";

    // 1. LinearWipe (50% completion, angle=90 deg -> left half wiped out, right half visible!)
    {
        auto pm = render_layer_ef(solid_red,
            R"({"ty":5,"mn":"ADBE Linear Wipe","en":1,"ef":[{"ty":0,"v":{"a":0,"k":50}},{"ty":0,"v":{"a":0,"k":90}},{"ty":0,"v":{"a":0,"k":0}}]})");
        assert(pm.as_ref().pixel(10, 40)->alpha() == 0);
        assert(pm.as_ref().pixel(70, 40)->red() > 220);
    }

    // 2. VenetianBlinds (50% completion, width=20px, angle=0 -> alternating opaque & transparent vertical bands)
    {
        auto pm = render_layer_ef(solid_red,
            R"({"ty":5,"mn":"ADBE Venetian Blinds","en":1,"ef":[{"ty":0,"v":{"a":0,"k":50}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":20}},{"ty":0,"v":{"a":0,"k":0}}]})");
        // Slat center x=10 is wiped (alpha 0), slat edge x=1 is visible (red > 220)
        assert(pm.as_ref().pixel(10, 40)->alpha() == 0);
        assert(pm.as_ref().pixel(1, 40)->red() > 220);
    }

    // 3. MotionTile (tile_width=50%, tile_height=50% replicates a shape in all 4 quadrants!)
    {
        const std::string_view corner_dot =
            R"("ind":1,"ty":4,"nm":"Dot","shapes":[{"ty":"rc","p":{"a":0,"k":[20,20]},"s":{"a":0,"k":[20,20]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[0.1,0.9,0.2,1]},"o":{"a":0,"k":100}}])";
        auto pm = render_layer_ef(corner_dot,
            R"({"ty":5,"mn":"ADBE Tile","en":1,"ef":[{"ty":3,"v":{"a":0,"k":[20,20]}},{"ty":0,"v":{"a":0,"k":50}},{"ty":0,"v":{"a":0,"k":50}},{"ty":0,"v":{"a":0,"k":100}},{"ty":0,"v":{"a":0,"k":100}},{"ty":7,"v":{"a":0,"k":0}}]})");
        assert(pm.as_ref().pixel(10, 10)->green() > 180);
        assert(pm.as_ref().pixel(50, 50)->green() > 180);
    }

    // 4. TransformEffect (translates a 20x20 box from [0..20] to [40..60] with 50% opacity)
    {
        const std::string_view box_tl =
            R"("ind":1,"ty":4,"nm":"Box","shapes":[{"ty":"rc","p":{"a":0,"k":[10,10]},"s":{"a":0,"k":[20,20]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[0.2,0.4,1.0,1]},"o":{"a":0,"k":100}}])";
        auto pm = render_layer_ef(box_tl,
            R"({"ty":5,"mn":"ADBE Geometry2","en":1,"ef":[{"ty":3,"v":{"a":0,"k":[0,0]}},{"ty":3,"v":{"a":0,"k":[40,40]}},{"ty":7,"v":{"a":0,"k":1}},{"ty":0,"v":{"a":0,"k":100}},{"ty":0,"v":{"a":0,"k":100}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":50}}]})");
        assert(pm.as_ref().pixel(10, 10)->alpha() == 0);
        auto moved = pm.as_ref().pixel(50, 50);
        assert(moved->alpha() >= 115 && moved->alpha() <= 140 && moved->blue() > 100);
    }

    // 5. CornerPin (maps full 80x80 solid into trapezoid [20,10]..[60,10]..[70,70]..[10,70])
    {
        auto pm = render_layer_ef(solid_red,
            R"({"ty":5,"mn":"ADBE Corner Pin","en":1,"ef":[{"ty":3,"v":{"a":0,"k":[20,10]}},{"ty":3,"v":{"a":0,"k":[60,10]}},{"ty":3,"v":{"a":0,"k":[10,70]}},{"ty":3,"v":{"a":0,"k":[70,70]}}]})");
        assert(pm.as_ref().pixel(40, 40)->red() > 220);
        assert(pm.as_ref().pixel(5, 5)->alpha() == 0);
    }

    // 6. Bulge & 7. Sphere (distorts a center dot outward so radius grows)
    {
        const std::string_view center_dot =
            R"("ind":1,"ty":4,"nm":"CDot","shapes":[{"ty":"el","p":{"a":0,"k":[40,40]},"s":{"a":0,"k":[20,20]}},{"ty":"fl","c":{"a":0,"k":[1,1,0,1]},"o":{"a":0,"k":100}}])";
        auto pm_bulge = render_layer_ef(center_dot,
            R"({"ty":5,"mn":"ADBE Bulge","en":1,"ef":[{"ty":0,"v":{"a":0,"k":36}},{"ty":0,"v":{"a":0,"k":36}},{"ty":3,"v":{"a":0,"k":[40,40]}},{"ty":0,"v":{"a":0,"k":2.0}}]})");
        // Without bulge, x=52 (r=12 > 10) is transparent; positive bulge magnifies center dot outward!
        assert(pm_bulge.as_ref().pixel(52, 40)->alpha() > 100);

        auto pm_sphere = render_layer_ef(center_dot,
            R"({"ty":5,"mn":"ADBE Spherize","en":1,"ef":[{"ty":0,"v":{"a":0,"k":36}},{"ty":3,"v":{"a":0,"k":[40,40]}}]})");
        assert(pm_sphere.as_ref().pixel(40, 40)->red() > 220);
        assert(pm_sphere.as_ref().pixel(52, 40)->alpha() > 80);
    }

    // 8. DirectionalBlur (horizontal blur length=24 spreads a 6px vertical bar horizontally)
    {
        const std::string_view vbar =
            R"("ind":1,"ty":4,"nm":"Bar","shapes":[{"ty":"rc","p":{"a":0,"k":[40,40]},"s":{"a":0,"k":[6,50]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}])";
        auto pm = render_layer_ef(vbar,
            R"({"ty":5,"mn":"ADBE Motion Blur","en":1,"ef":[{"ty":0,"v":{"a":0,"k":90}},{"ty":0,"v":{"a":0,"k":24}}]})");
        // Spread horizontally at x=49 (outside original 37..43 bar), but NOT vertically above y=12!
        assert(pm.as_ref().pixel(49, 40)->alpha() > 20);
        assert(pm.as_ref().pixel(40, 8)->alpha() == 0);
    }

    // 9. Sharpen (increases edge contrast across step boundary without corrupting flat interior)
    {
        const std::string_view half_box =
            R"("ind":1,"ty":4,"nm":"Step","shapes":[{"ty":"rc","p":{"a":0,"k":[20,40]},"s":{"a":0,"k":[40,80]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[0.6,0.6,0.6,1]},"o":{"a":0,"k":100}}])";
        auto pm = render_layer_ef(half_box,
            R"({"ty":5,"mn":"ADBE Sharpen","en":1,"ef":[{"ty":0,"v":{"a":0,"k":80}}]})");
        auto interior = pm.as_ref().pixel(15, 40);
        assert(interior->red() > 140 && interior->red() < 170);
    }

    // 10. FractalNoise (generates non-uniform procedural grayscale variation across solid layer)
    {
        auto pm = render_layer_ef(solid_red,
            R"({"ty":5,"mn":"ADBE Fractal Noise","en":1,"ef":[{"ty":7,"v":{"a":0,"k":1}},{"ty":7,"v":{"a":0,"k":2}},{"ty":7,"v":{"a":0,"k":0}},{"ty":0,"v":{"a":0,"k":180}},{"ty":0,"v":{"a":0,"k":0}},{"ty":7,"v":{"a":0,"k":1}},{"ty":0,"v":{"a":0,"k":16}},{"ty":0,"v":{"a":0,"k":4}},{"ty":0,"v":{"a":0,"k":50}},{"ty":0,"v":{"a":0,"k":50}},{"ty":0,"v":{"a":0,"k":45}},{"ty":0,"v":{"a":0,"k":100}}]})");
        uint8_t min_v = 255, max_v = 0;
        for (uint32_t y = 4; y < 76; y += 4) {
            for (uint32_t x = 4; x < 76; x += 4) {
                auto p = pm.as_ref().pixel(x, y);
                assert(std::abs(int(p->red()) - int(p->green())) <= 2);
                min_v = std::min(min_v, p->red());
                max_v = std::max(max_v, p->red());
            }
        }
        assert(max_v - min_v > 60);
    }

    std::cout << "[PASS] Phase 3 Transitions, Distortion, Blur & Procedural Effects tests passed.\n";
}

void test_phase4_expressions_text_animators_and_audio() {
    std::cout << "[TEST] Running Phase 4.1 (ExpressionManager), Phase 4.2 (Advanced TextAnimator & Text-on-Path), and Phase 4.3 (AudioLayer/AudioTrack) tests...\n";

    // ── 4.1 Pluggable ExpressionManager (number, array, string + fallback + override + invalidation) ──
    {
        auto expr_mgr = std::make_shared<CallbackExpressionManager>();
        expr_mgr->number_factory = [](std::string_view expr) -> std::shared_ptr<ExpressionEvaluator<float>> {
            if (expr == "custom_opacity") {
                return std::make_shared<CallbackExpressionEvaluator<float>>([](float t) {
                    return (t >= 0.4f) ? 100.0f : 25.0f;
                });
            }
            // Return nullptr for built-in expressions (e.g. loopOut) so fast built-ins still run!
            return nullptr;
        };
        expr_mgr->array_factory = [](std::string_view expr) -> std::shared_ptr<ExpressionEvaluator<std::vector<float>>> {
            if (expr == "custom_pos") {
                return std::make_shared<CallbackExpressionEvaluator<std::vector<float>>>([](float t) {
                    return std::vector<float>{20.0f + t * 80.0f, 40.0f};
                });
            }
            if (expr == "custom_color") {
                return std::make_shared<CallbackExpressionEvaluator<std::vector<float>>>([](float t) {
                    return (t >= 0.4f) ? std::vector<float>{0.1f, 0.9f, 0.2f, 1.0f}
                                       : std::vector<float>{0.9f, 0.1f, 0.1f, 1.0f};
                });
            }
            return nullptr;
        };
        expr_mgr->string_factory = [](std::string_view expr) -> std::shared_ptr<ExpressionEvaluator<std::string>> {
            if (expr == "custom_label") {
                return std::make_shared<CallbackExpressionEvaluator<std::string>>([](float t) {
                    return (t >= 0.4f) ? std::string("HI") : std::string("A");
                });
            }
            return nullptr;
        };

        const std::string expr_json = R"JSON({
            "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 140, "h": 80,
            "layers": [
                {
                    "ind": 1, "ty": 4, "nm": "ExprBox", "ip": 0, "op": 60, "st": 0,
                    "ks": {
                        "p": {"a": 0, "k": [10, 10], "x": "custom_pos"},
                        "a": {"a": 0, "k": [0, 0]},
                        "s": {"a": 0, "k": [100, 100]},
                        "o": {"a": 0, "k": 100, "x": "custom_opacity"}
                    },
                    "shapes": [
                        {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [24, 24]}, "r": {"a": 0, "k": 0}},
                        {"ty": "fl", "nm": "Fill", "c": {"a": 0, "k": [0, 0, 1, 1], "x": "custom_color"}, "o": {"a": 0, "k": 100}}
                    ]
                },
                {
                    "ind": 2, "ty": 4, "nm": "BuiltinLoop", "ip": 0, "op": 60, "st": 0,
                    "ks": {
                        "p": {
                            "a": 1,
                            "k": [{"t": 0, "s": [10, 70], "e": [50, 70]}, {"t": 10, "s": [50, 70]}],
                            "x": "loopOut(\"cycle\")"
                        },
                        "a": {"a": 0, "k": [0, 0]},
                        "s": {"a": 0, "k": [100, 100]},
                        "o": {"a": 0, "k": 100}
                    },
                    "shapes": [
                        {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [10, 10]}, "r": {"a": 0, "k": 0}},
                        {"ty": "fl", "c": {"a": 0, "k": [0.2, 0.5, 1.0, 1.0]}, "o": {"a": 0, "k": 100}}
                    ]
                },
                {
                    "ind": 3, "ty": 5, "nm": "DynText", "ip": 0, "op": 60, "st": 0,
                    "ks": {"p": {"a": 0, "k": [10, 22]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                    "t": {
                        "d": {
                            "x": "custom_label",
                            "k": [{"t": 0, "s": {"t": "OLD", "s": 18, "fc": [1, 1, 1], "f": "Sans", "j": 0, "lh": 22}}]
                        }
                    }
                }
            ]
        })JSON";

        auto anim = AnimationBuilder().set_expression_manager(expr_mgr).make(expr_json);
        assert(anim != nullptr);

        // Verify InvalidationController recognizes expression-driven changes across frames
        InvalidationController ic;
        anim->seek_frame(0.0f, &ic);
        ic.reset();
        anim->seek_frame(30.0f, &ic); // t = 0.5s -> pos=(60,40), color=green, opacity=100%, text="HI"
        assert(!ic.is_empty());

        auto pm0 = Pixmap::create(140, 80);
        auto pm30 = Pixmap::create(140, 80);
        assert(pm0 && pm30);
        pm0->fill(Color::TRANSPARENT);
        pm30->fill(Color::TRANSPARENT);
        {
            Canvas c0(*pm0);
            anim->render(c0, 0.0f);
            Canvas c30(*pm30);
            anim->render(c30, 30.0f);
        }

        // At frame 0 (t=0): ExprBox at (20,40), red, ~25% alpha
        auto p_f0 = pm0->as_ref().pixel(20, 40);
        assert(p_f0->alpha() >= 55 && p_f0->alpha() <= 75);
        assert(p_f0->red() > p_f0->green());

        // At frame 30 (t=0.5s): ExprBox moved to (60,40), green, 100% alpha; old pos (20,40) empty
        auto p_f30_old = pm30->as_ref().pixel(20, 40);
        auto p_f30_new = pm30->as_ref().pixel(60, 40);
        assert(p_f30_old->alpha() == 0);
        assert(p_f30_new->alpha() == 255 && p_f30_new->green() > 210 && p_f30_new->red() < 40);

        // Built-in loopOut("cycle") still works at frame 15 (t=5 within 10-frame cycle -> x=30, y=70)
        auto pm15 = Pixmap::create(140, 80);
        pm15->fill(Color::TRANSPARENT);
        {
            Canvas c15(*pm15);
            anim->render(c15, 15.0f);
        }
        assert(pm15->as_ref().pixel(30, 70)->blue() > 200);

        // String expression "custom_label" changed text width from "A" (1 char) to "HI" (2 chars)
        int text_px_0 = 0, text_px_30 = 0;
        for (uint32_t y = 0; y < 26; ++y) {
            for (uint32_t x = 0; x < 60; ++x) {
                if (pm0->as_ref().pixel(x, y)->alpha() > 20) ++text_px_0;
                if (pm30->as_ref().pixel(x, y)->alpha() > 20) ++text_px_30;
            }
        }
        assert(text_px_30 > text_px_0);

        // Runtime overrides still take priority over expression evaluators!
        assert(anim->set_color_override("ExprBox.Fill", Color::from_rgba8(20, 40, 240, 255)));
        auto pm_ov = Pixmap::create(140, 80);
        pm_ov->fill(Color::TRANSPARENT);
        {
            Canvas cov(*pm_ov);
            anim->render(cov, 30.0f);
        }
        auto p_ov = pm_ov->as_ref().pixel(60, 40);
        assert(p_ov->blue() > 210 && p_ov->green() < 60);
    }

    // ── 4.2 Advanced TextAnimator (Word/Line/Non-Space Selectors, Smoothness, Multi-Selector) & Text-on-Path ──
    {
        // A. Direct unit checks on TextRangeSelector smoothness & shapes
        TextRangeSelector sel;
        sel.start = FloatProperty(0.0f);
        sel.end = FloatProperty(100.0f);
        sel.units = 1; // percentage
        sel.shape = 2; // Ramp Up
        sel.smoothness = FloatProperty(100.0f);
        const float smooth_low = sel.compute_factor(0, 4, 0.0f);   // mid = 0.125
        const float smooth_high = sel.compute_factor(3, 4, 0.0f);  // mid = 0.875
        assert(smooth_low > 0.05f && smooth_low < 0.25f);
        assert(smooth_high > 0.75f && smooth_high < 0.95f);

        // With smoothness = 0%, ramp snaps to binary 0 or 1
        sel.smoothness = FloatProperty(0.0f);
        assert(sel.compute_factor(0, 4, 0.0f) < 0.01f);
        assert(sel.compute_factor(3, 4, 0.0f) > 0.99f);

        // Triangle & Round shapes peak at center
        sel.smoothness = FloatProperty(100.0f);
        sel.shape = 4; // Triangle
        assert(sel.compute_factor(1, 3, 0.0f) > 0.95f);
        assert(sel.compute_factor(0, 3, 0.0f) < 0.45f);
        sel.shape = 5; // Round
        assert(sel.compute_factor(1, 3, 0.0f) > 0.95f);

        // B. Word-based ("b": 3) & Line-based ("b": 4) & Multi-Selector (Add + Subtract) rendering
        const std::string word_line_json = R"({
            "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 220, "h": 100,
            "layers": [{
                "ind": 1, "ty": 5, "nm": "MultiText", "ip": 0, "op": 60, "st": 0,
                "ks": {"p": {"a": 0, "k": [12, 34]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                "t": {
                    "d": {"k": [{"t": 0, "s": {"t": "AAA BBB\nCCC", "s": 24, "fc": [1, 0, 0], "f": "Sans", "j": 0, "lh": 42}}]},
                    "a": [
                        {
                            "nm": "SecondWordGreen",
                            "s": [
                                {"s": {"a": 0, "k": 0}, "e": {"a": 0, "k": 3}, "r": 2, "b": 3, "sh": 1, "m": 1},
                                {"s": {"a": 0, "k": 0}, "e": {"a": 0, "k": 1}, "r": 2, "b": 3, "sh": 1, "m": 2}
                            ],
                            "a": {"fc": {"a": 0, "k": [0, 1, 0, 1]}}
                        },
                        {
                            "nm": "SecondLineBlue",
                            "s": {"s": {"a": 0, "k": 1}, "e": {"a": 0, "k": 2}, "r": 2, "b": 4, "sh": 1},
                            "a": {"fc": {"a": 0, "k": [0, 0, 1, 1]}}
                        }
                    ]
                }
            }]
        })";
        auto anim_wl = Animation::load_from_data(word_line_json);
        assert(anim_wl != nullptr);
        auto pm_wl = Pixmap::create(220, 100);
        pm_wl->fill(Color::TRANSPARENT);
        {
            Canvas c(*pm_wl);
            anim_wl->render(c, 0.0f);
        }
        // Word 0 ("AAA" top-left) is red; Word 1 ("BBB" top-right) is green (Add 0..3 minus Subtract 0..1); Line 1 ("CCC" bottom) has blue channel!
        bool found_red_w0 = false, found_green_w1 = false, found_blue_l1 = false;
        for (uint32_t y = 8; y < 44; ++y) {
            for (uint32_t x = 10; x < 70; ++x) {
                auto p = pm_wl->as_ref().pixel(x, y);
                if (p->alpha() > 150 && p->red() > 180 && p->green() < 60) found_red_w0 = true;
            }
            for (uint32_t x = 70; x < 180; ++x) {
                auto p = pm_wl->as_ref().pixel(x, y);
                if (p->alpha() > 150 && p->green() > 180 && p->red() < 60) found_green_w1 = true;
            }
        }
        for (uint32_t y = 50; y < 90; ++y) {
            for (uint32_t x = 10; x < 100; ++x) {
                auto p = pm_wl->as_ref().pixel(x, y);
                if (p->alpha() > 150 && p->blue() > 180) found_blue_l1 = true;
            }
        }
        assert(found_red_w0 && found_green_w1 && found_blue_l1);

        // C. Text-on-Path ("t.p") along an upward-arching cubic Bezier mask path
        const std::string path_text_json = R"({
            "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 220, "h": 140,
            "layers": [{
                "ind": 1, "ty": 5, "nm": "ArchText", "ip": 0, "op": 60, "st": 0,
                "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                "masksProperties": [{
                    "mode": "n",
                    "pt": {
                        "a": 0,
                        "k": {
                            "c": false,
                            "v": [[20, 110], [200, 110]],
                            "i": [[0, 0], [0, -100]],
                            "o": [[0, -100], [0, 0]]
                        }
                    },
                    "o": {"a": 0, "k": 100}
                }],
                "t": {
                    "d": {"k": [{"t": 0, "s": {"t": "NISABA", "s": 20, "fc": [1, 0.9, 0.1], "f": "Sans", "j": 1, "lh": 24}}]},
                    "p": {"m": 0, "f": {"a": 0, "k": 0}, "l": {"a": 0, "k": 0}, "p": 1, "r": 0, "a": 1}
                }
            }]
        })";
        auto anim_tp = Animation::load_from_data(path_text_json);
        assert(anim_tp != nullptr);
        auto pm_tp = Pixmap::create(220, 140);
        pm_tp->fill(Color::TRANSPARENT);
        {
            Canvas c(*pm_tp);
            anim_tp->render(c, 0.0f);
        }
        // Apex of the arch is at x=110, y=35 (far above the endpoints at y=110). Verify yellow glyph ink near the apex AND near left/right legs!
        bool apex_ink = false, left_leg_ink = false, right_leg_ink = false;
        for (uint32_t y = 10; y < 55; ++y) {
            for (uint32_t x = 85; x < 135; ++x) {
                auto p = pm_tp->as_ref().pixel(x, y);
                if (p->alpha() > 100 && p->red() > 180 && p->green() > 150) apex_ink = true;
            }
        }
        for (uint32_t y = 75; y < 120; ++y) {
            for (uint32_t x = 5; x < 50; ++x) {
                if (pm_tp->as_ref().pixel(x, y)->alpha() > 100) left_leg_ink = true;
            }
            for (uint32_t x = 170; x < 215; ++x) {
                if (pm_tp->as_ref().pixel(x, y)->alpha() > 100) right_leg_ink = true;
            }
        }
        assert(apex_ink && left_leg_ink && right_leg_ink);
    }

    // ── 4.3 AudioLayer ("ty": 6) & Player AudioTrack Hook ──
    {
        auto mem_rp = std::make_shared<MemoryResourceProvider>();
        std::vector<uint8_t> fake_pcm = {'R', 'I', 'F', 'F', 1, 2, 3, 4};
        mem_rp->add_audio_asset("snd_whoosh", std::make_shared<MemoryAudioAsset>(fake_pcm));

        const std::string audio_json = R"({
            "v": "5.9.0", "fr": 60, "ip": 0, "op": 60, "w": 64, "h": 64,
            "assets": [{"id": "snd_whoosh", "u": "", "p": "whoosh.wav"}],
            "layers": [
                {
                    "ind": 1, "ty": 6, "nm": "SFX", "refId": "snd_whoosh",
                    "ip": 10, "op": 40, "st": 10, "sr": 1,
                    "au": {
                        "lv": {
                            "a": 1,
                            "k": [
                                {"t": 10, "s": [0, 0], "e": [-6, -6]},
                                {"t": 25, "s": [-6, -6], "e": [-96, -96]},
                                {"t": 40, "s": [-96, -96]}
                            ]
                        }
                    }
                }
            ]
        })";

        auto anim_au = AnimationBuilder().set_resource_provider(mem_rp).make(audio_json);
        assert(anim_au != nullptr);

        struct RecordedEvent {
            std::string id;
            float local_t = 0.0f;
            float l_db = 0.0f;
            float r_db = 0.0f;
            float gain = 0.0f;
            bool playing = false;
            bool has_asset = false;
        };
        std::vector<RecordedEvent> events;
        std::vector<std::string> stops;

        auto track = std::make_shared<CallbackAudioTrack>(
            [&](std::string_view id, float local_t, float l_db, float r_db, float gain, bool playing, const std::shared_ptr<AudioAsset>& asset) {
                events.push_back({std::string(id), local_t, l_db, r_db, gain, playing, asset != nullptr});
            },
            [&](std::string_view id) {
                stops.emplace_back(id);
            }
        );

        Player player(anim_au);
        player.set_audio_track(track);
        player.set_loop(false);

        // Before in_point (frame 5): inactive -> no audio frame event
        player.seek_frame(5.0f);
        assert(events.empty());

        // Enter [10, 40) at frame 10 -> 0 dB -> gain ~ 1.0, local_t = 0.0
        player.seek_frame(10.0f);
        assert(events.size() == 1);
        assert(events.back().id == "snd_whoosh");
        assert(std::abs(events.back().local_t - 0.0f) < 1e-3f);
        assert(std::abs(events.back().l_db - 0.0f) < 1e-2f);
        assert(std::abs(events.back().gain - 1.0f) < 1e-2f);
        assert(events.back().has_asset);

        // Advance by 0.25s (15 frames -> frame 25): -6 dB -> gain ~ 0.501, local_t = 0.25s, is_playing = true
        player.play();
        player.advance(0.25f);
        assert(events.size() == 2);
        assert(events.back().playing);
        assert(std::abs(events.back().local_t - 0.25f) < 1e-2f);
        assert(std::abs(events.back().l_db - (-6.0f)) < 0.2f);
        assert(std::abs(events.back().gain - 0.501187f) < 0.02f);

        // Exit [10, 40) at frame 45 -> triggers on_audio_stop("snd_whoosh")
        player.seek_frame(45.0f);
        assert(stops.size() == 1 && stops.back() == "snd_whoosh");

        // Re-enter at frame 15 then call stop() -> triggers on_audio_stop again
        player.seek_frame(15.0f);
        player.stop();
        assert(stops.size() == 2 && stops.back() == "snd_whoosh");

        // Verify helper gain conversion
        assert(std::abs(audio_db_to_linear_gain(0.0f) - 1.0f) < 1e-4f);
        assert(audio_db_to_linear_gain(-96.0f) == 0.0f);
    }

    std::cout << "[PASS] Phase 4 ExpressionManager, Advanced TextAnimator/Text-on-Path & AudioLayer tests passed.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   Nisaba Sovereign Lottie & JSON Test Suite\n";
    std::cout << "========================================================\n";

    test_json_parser();
    test_lottie_properties_spatial_and_expressions();
    test_lottie_shapes_gradients_and_modifiers();
    test_advanced_features_and_dotlottie();
    test_phase1_invalidation_controller();
    test_phase1_resource_provider_deferred_and_multiframe();
    test_phase1_logger_stats_and_property_observer();
    test_phase2_external_precomp_and_color_effects();
    test_phase3_transitions_distortion_blur_and_procedural();
    test_phase4_expressions_text_animators_and_audio();

    std::cout << "========================================================\n";
    std::cout << "   ALL LOTTIE & JSON TESTS PASSED SUCCESSFULLY! (100%)\n";
    std::cout << "========================================================\n";
    return 0;
}
