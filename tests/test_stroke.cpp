#include <iostream>
#include <cmath>
#include <cassert>
#include "nisaba/nisaba.hpp"

using namespace nisaba;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CASE(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "[RUN]  " << #name << "..." << std::flush; \
    int prev_failed = g_tests_failed; \
    name(); \
    if (g_tests_failed == prev_failed) { \
        std::cout << " [PASSED]" << std::endl; \
        g_tests_passed++; \
    } else { \
        std::cout << " [FAILED]" << std::endl; \
    } \
} while(0)

#define EXPECT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "\n  FAILED: " << #expr << " is false at " << __FILE__ << ":" << __LINE__ << std::endl; \
        g_tests_failed++; \
    } \
} while(0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::cerr << "\n  FAILED: " << #a << " == " << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        g_tests_failed++; \
    } \
} while(0)

#define EXPECT_NEAR(a, b, eps) do { \
    if (std::abs((a) - (b)) > (eps)) { \
        std::cerr << "\n  FAILED: |" << #a << " - " << #b << "| (" << std::abs((a) - (b)) << ") <= " << #eps << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        g_tests_failed++; \
    } \
} while(0)

TEST_CASE(test_stroke_dash_validation) {
    EXPECT_FALSE(StrokeDash::create({}, 0.0f).has_value());
    EXPECT_FALSE(StrokeDash::create({0.0f}, 0.0f).has_value());
    EXPECT_FALSE(StrokeDash::create({1.0f, -2.0f}, 0.0f).has_value());
    EXPECT_FALSE(StrokeDash::create({0.0f, 0.0f}, 0.0f).has_value());
    EXPECT_FALSE(StrokeDash::create({1.0f, 1.0f}, std::numeric_limits<float>::infinity()).has_value());

    // Odd-length dash arrays duplicate automatically (SVG/CSS/Canvas parity)
    auto single = StrokeDash::create({5.0f}, 0.0f);
    EXPECT_TRUE(single.has_value());
    EXPECT_EQ(single->array().size(), 2u);
    EXPECT_NEAR(single->interval_len().get(), 10.0f, 1e-5f);

    auto triple = StrokeDash::create({1.0f, 2.0f, 3.0f}, 0.0f);
    EXPECT_TRUE(triple.has_value());
    EXPECT_EQ(triple->array().size(), 6u);
    EXPECT_NEAR(triple->interval_len().get(), 12.0f, 1e-5f);

    auto d = StrokeDash::create({6.0f, 4.5f}, 0.0f);
    EXPECT_TRUE(d.has_value());
    EXPECT_EQ(d->array().size(), 2u);
    EXPECT_NEAR(d->interval_len().get(), 10.5f, 1e-5f);
}

TEST_CASE(test_stroke_triangle) {
    PathBuilder pb;
    pb.move_to(10.0f, 10.0f);
    pb.line_to(20.0f, 50.0f);
    pb.line_to(30.0f, 10.0f);
    pb.close();
    auto path = pb.finish();
    EXPECT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 1.0f;
    stroke.line_cap = LineCap::Butt;
    stroke.line_join = LineJoin::Miter;

    auto stroked = stroke_path(*path, stroke, 1.0f);
    EXPECT_TRUE(stroked.has_value());
    EXPECT_TRUE(stroked->len() > path->len());

    // Tight bounds of stroked triangle should be wider than original by roughly stroke radius (0.5)
    auto bounds = stroked->bounds();
    EXPECT_NEAR(bounds.left(), 9.35f, 0.5f);
    EXPECT_NEAR(bounds.top(), 9.5f, 0.5f);
    EXPECT_NEAR(bounds.right(), 30.65f, 0.5f);
    EXPECT_NEAR(bounds.bottom(), 50.12f, 0.5f);
}

TEST_CASE(test_stroke_with_dash) {
    PathBuilder pb;
    pb.move_to(0.0f, 0.0f);
    pb.line_to(100.0f, 0.0f);
    auto path = pb.finish();
    EXPECT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 2.0f;
    stroke.dash = StrokeDash::create({10.0f, 10.0f}, 0.0f);

    auto stroked = stroke_path(*path, stroke, 1.0f);
    EXPECT_TRUE(stroked.has_value());
    EXPECT_TRUE(stroked->len() > 0);
}

TEST_CASE(test_stroke_caps_and_joins) {
    PathBuilder pb;
    pb.move_to(0.0f, 0.0f);
    pb.line_to(50.0f, 50.0f);
    pb.line_to(100.0f, 0.0f);
    auto path = pb.finish().value();

    for (LineCap cap : {LineCap::Butt, LineCap::Round, LineCap::Square}) {
        for (LineJoin join : {LineJoin::Miter, LineJoin::Round, LineJoin::Bevel}) {
            Stroke s;
            s.width = 4.0f;
            s.line_cap = cap;
            s.line_join = join;
            auto res = stroke_path(path, s, 1.0f);
            EXPECT_TRUE(res.has_value());
            EXPECT_TRUE(!res->is_empty());
        }
    }
}

int main() {
    std::cout << "=== Running Nisaba Stroker & Dasher Unit Tests ===" << std::endl;
    RUN_TEST(test_stroke_dash_validation);
    RUN_TEST(test_stroke_triangle);
    RUN_TEST(test_stroke_with_dash);
    RUN_TEST(test_stroke_caps_and_joins);

    std::cout << "\nTest Summary: " << g_tests_passed << " passed, " << g_tests_failed << " failed." << std::endl;
    return g_tests_failed == 0 ? 0 : 1;
}
