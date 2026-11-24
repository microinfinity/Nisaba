#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::text;

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string resolve_font(std::string_view filename) {
    const std::string candidates[3] = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename)
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return candidates[0];
}

double compute_mae(const Pixmap& a, const Pixmap& b) {
    const uint32_t w = std::min(a.width(), b.width());
    const uint32_t h = std::min(a.height(), b.height());
    if (w == 0 || h == 0) return 0.0;
    const auto ar = a.as_ref();
    const auto br = b.as_ref();
    double sum_abs = 0.0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            auto pa = ar.pixel(x, y);
            auto pb = br.pixel(x, y);
            if (!pa || !pb) continue;
            sum_abs += std::abs(int(pa->red())   - int(pb->red()))
                     + std::abs(int(pa->green()) - int(pb->green()))
                     + std::abs(int(pa->blue())  - int(pb->blue()))
                     + std::abs(int(pa->alpha()) - int(pb->alpha()));
        }
    }
    return sum_abs / (4.0 * static_cast<double>(w) * static_cast<double>(h));
}

Path make_star_path(float cx, float cy, float r_outer, float r_inner, int points = 5) {
    PathBuilder pb;
    for (int i = 0; i < points * 2; ++i) {
        const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / static_cast<float>(points);
        const float r = (i % 2 == 0) ? r_outer : r_inner;
        const float px = cx + std::cos(angle) * r;
        const float py = cy + std::sin(angle) * r;
        if (i == 0) pb.move_to(px, py);
        else pb.line_to(px, py);
    }
    pb.close();
    auto p = pb.finish();
    return p ? *p : Path{};
}

Pixmap make_ui_nine_slice_pixmap() {
    auto pm = Pixmap::create(32, 32).value();
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            const bool bx = (x < 8 || x >= 24);
            const bool by = (y < 8 || y >= 24);
            const bool edge = (x == 0 || y == 0 || x == 31 || y == 31 || x == 1 || y == 1 || x == 30 || y == 30);
            Color c = Color::from_rgba8(30, 52, 92, 255);
            if (edge) {
                c = Color::from_rgba8(125, 211, 252, 255);
            } else if (bx && by) {
                c = Color::from_rgba8(56, 189, 248, 255);
            } else if (bx || by) {
                c = Color::from_rgba8(37, 99, 235, 255);
            }
            pm.set_pixel(x, y, c.premultiply().to_color_u8());
        }
    }
    return pm;
}

Pixmap make_sprite_atlas_pixmap() {
    auto pm = Pixmap::create(64, 64).value();
    pm.fill(Color::TRANSPARENT);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            const uint32_t qx = x / 32;
            const uint32_t qy = y / 32;
            const float lx = static_cast<float>(x % 32) - 15.5f;
            const float ly = static_cast<float>(y % 32) - 15.5f;
            const float dist = std::hypot(lx, ly);

            Color c = Color::TRANSPARENT;
            if (qx == 0 && qy == 0) {
                // Glowing orb
                if (dist <= 12.5f) {
                    const float t = std::clamp(1.0f - dist / 12.5f, 0.0f, 1.0f);
                    c = Color::from_rgba8(255, static_cast<uint8_t>(110 + t * 145), static_cast<uint8_t>(120 + t * 100), 255);
                }
            } else if (qx == 1 && qy == 0) {
                // Diamond gem
                const float manh = std::abs(lx) + std::abs(ly);
                if (manh <= 13.0f) {
                    c = (manh <= 6.0f) ? Color::from_rgba8(224, 242, 254, 255) : Color::from_rgba8(56, 189, 248, 255);
                }
            } else if (qx == 0 && qy == 1) {
                // Ring badge
                if (dist >= 7.0f && dist <= 13.0f) {
                    c = Color::from_rgba8(52, 211, 153, 255);
                }
            } else {
                // Plus emblem
                if ((std::abs(lx) <= 4.0f && std::abs(ly) <= 12.5f) ||
                    (std::abs(ly) <= 4.0f && std::abs(lx) <= 12.5f)) {
                    c = Color::from_rgba8(250, 204, 21, 255);
                }
            }
            if (c.alpha() > 0.0f) {
                pm.set_pixel(x, y, c.premultiply().to_color_u8());
            }
        }
    }
    return pm;
}

class LiveEmblemDrawable final : public Drawable {
public:
    LiveEmblemDrawable(float r, Color fill, Color ring) : r_(r), fill_(fill), ring_(ring) {}

protected:
    Rect on_get_bounds() const override {
        return *Rect::from_xywh(-r_ - 4.0f, -r_ - 4.0f, 2.0f * (r_ + 4.0f), 2.0f * (r_ + 4.0f));
    }

    void on_draw(ICanvas& c) override {
        c.fill_oval(*Rect::from_xywh(-r_, -r_, 2.0f * r_, 2.0f * r_), Paint(fill_));
        Stroke st(3.0f);
        c.stroke_oval(*Rect::from_xywh(-r_ - 3.0f, -r_ - 3.0f, 2.0f * (r_ + 3.0f), 2.0f * (r_ + 3.0f)), Paint(ring_), st);
        c.fill_path(make_star_path(0.0f, 0.0f, r_ * 0.68f, r_ * 0.30f, 5), Paint(Color::WHITE));
    }

private:
    float r_;
    Color fill_;
    Color ring_;
};

void render_canvas_parity_showcase(
    ICanvas& canvas,
    const TtfFont& ui_font,
    const TtfFont& mono_font,
    const Pixmap& ui_nine_pm,
    const Pixmap& atlas_pm,
    uint32_t W,
    uint32_t H
) {
    auto draw_text_line = [&](std::string_view text, float x, float y, float sz, Color col, const TtfFont& f) {
        auto blob = TextBlob::from_text(text, f, sz);
        if (blob && !blob->is_empty()) {
            canvas.draw_text_blob(*blob, x, y, Paint(col));
        }
    };

    // -------------------------------------------------------------------------
    // 0. Full-Screen Gradient Backdrop using draw_paint()
    // -------------------------------------------------------------------------
    {
        auto bg_grad = LinearGradient::create(
            Point::from_xy(0.0f, 0.0f),
            Point::from_xy(static_cast<float>(W), static_cast<float>(H)),
            {
                GradientStop::create(0.0f, Color::from_rgba8(10, 14, 24, 255)),
                GradientStop::create(0.5f, Color::from_rgba8(15, 22, 38, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(12, 16, 30, 255))
            }
        );
        Paint bg_p = bg_grad ? Paint(Shader(*bg_grad)) : Paint(Color::from_rgba8(12, 16, 28, 255));
        canvas.draw_paint(bg_p);
    }

    // Header bar
    {
        Rect header_r = *Rect::from_xywh(24.0f, 18.0f, static_cast<float>(W) - 48.0f, 74.0f);
        canvas.fill_rrect(RRect::from_rect_and_radius(header_r, 14.0f), Paint(Color::from_rgba8(22, 31, 52, 235)));
        Stroke border(1.25f);
        canvas.stroke_rrect(RRect::from_rect_and_radius(header_r, 14.0f), Paint(Color::from_rgba8(56, 189, 248, 90)), border);

        auto title_blob = TextBlob::from_text("NISABA CANVAS PARITY SHOWCASE — PHASES 1 TO 4", ui_font, 26.0f);
        auto title_grad = LinearGradient::create(
            Point::from_xy(44.0f, 52.0f), Point::from_xy(680.0f, 52.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(248, 250, 252, 255)),
                GradientStop::create(0.55f, Color::from_rgba8(125, 211, 252, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(192, 132, 252, 255))
            }
        );
        Paint tp = title_grad ? Paint(Shader(*title_grad)) : Paint(Color::WHITE);
        canvas.draw_text_blob(title_blob, 44.0f, 52.0f, tp);

        draw_text_line(
            "1:1 CPU / OpenGL 3 / Vulkan / PDF Parity  |  Clip Queries, 9-Slice/Lattice, Diff & Shader Clip, Atlas, Coons, Picture, 4x4, Region, TextBlob & RSXform",
            44.0f, 78.0f, 12.5f, Color::from_rgba8(148, 163, 184, 255), ui_font
        );

        // Phase status pills on right
        const char* pills[4] = {"PHASE 1", "PHASE 2", "PHASE 3", "PHASE 4"};
        const Color pill_cols[4] = {
            Color::from_rgba8(56, 189, 248, 255),
            Color::from_rgba8(168, 85, 247, 255),
            Color::from_rgba8(52, 211, 153, 255),
            Color::from_rgba8(251, 191, 36, 255)
        };
        for (int i = 0; i < 4; ++i) {
            const float px = static_cast<float>(W) - 380.0f + static_cast<float>(i) * 88.0f;
            Rect pr = *Rect::from_xywh(px, 34.0f, 78.0f, 26.0f);
            canvas.fill_rrect(RRect::from_rect_and_radius(pr, 13.0f),
                              Paint(Color::from_rgba_unchecked(pill_cols[i].red(), pill_cols[i].green(), pill_cols[i].blue(), 0.16f)));
            canvas.stroke_rrect(RRect::from_rect_and_radius(pr, 13.0f), Paint(pill_cols[i]), Stroke(1.0f));
            draw_text_line(pills[i], px + 14.0f, 51.0f, 11.0f, pill_cols[i], mono_font);
        }
    }

    // 4x3 Bento Grid Layout (12 cards)
    constexpr float margin_x = 24.0f;
    constexpr float start_y = 110.0f;
    constexpr float gap = 16.0f;
    constexpr int cols = 4;
    constexpr int rows = 3;
    const float card_w = (static_cast<float>(W) - 2.0f * margin_x - (cols - 1) * gap) / cols;
    const float card_h = (static_cast<float>(H) - start_y - 24.0f - (rows - 1) * gap) / rows;

    auto card_rect = [&](int col, int row) -> Rect {
        const float x = margin_x + static_cast<float>(col) * (card_w + gap);
        const float y = start_y + static_cast<float>(row) * (card_h + gap);
        return *Rect::from_xywh(x, y, card_w, card_h);
    };

    auto begin_card = [&](int col, int row, std::string_view tag, std::string_view title, Color accent) -> Rect {
        Rect r = card_rect(col, row);
        canvas.fill_rrect(RRect::from_rect_and_radius(r, 12.0f), Paint(Color::from_rgba8(20, 28, 46, 240)));
        canvas.stroke_rrect(RRect::from_rect_and_radius(r, 12.0f), Paint(Color::from_rgba8(51, 65, 85, 220)), Stroke(1.0f));

        // Top accent strip
        Rect strip = *Rect::from_xywh(r.left() + 12.0f, r.top() + 10.0f, 4.0f, 22.0f);
        canvas.fill_rrect(RRect::from_rect_and_radius(strip, 2.0f), Paint(accent));

        draw_text_line(tag, r.left() + 24.0f, r.top() + 22.0f, 10.5f, accent, mono_font);
        draw_text_line(title, r.left() + 24.0f, r.top() + 38.0f, 13.5f, Color::from_rgba8(241, 245, 249, 255), ui_font);

        return *Rect::from_xywh(r.left() + 14.0f, r.top() + 48.0f, r.width() - 28.0f, r.height() - 62.0f);
    };

    // =========================================================================
    // CARD 1.1: Clip Queries & Frustum Culling (quick_reject + clip bounds)
    // =========================================================================
    {
        Rect body = begin_card(0, 0, "1.1 CLIP QUERIES", "quick_reject() Frustum Culling", Color::from_rgba8(56, 189, 248, 255));
        Rect viewport = *Rect::from_xywh(body.left() + 58.0f, body.top() + 34.0f, body.width() - 116.0f, body.height() - 84.0f);

        // Draw all candidate boxes in subtle outline first, then highlight visible vs culled
        int visible_cnt = 0;
        int culled_cnt = 0;
        const int base = canvas.save();
        canvas.clip_rect(viewport);
        for (int gy = 0; gy < 4; ++gy) {
            for (int gx = 0; gx < 6; ++gx) {
                const float bx = body.left() + 8.0f + static_cast<float>(gx) * ((body.width() - 28.0f) / 6.0f);
                const float by = body.top() + 8.0f + static_cast<float>(gy) * ((body.height() - 40.0f) / 4.0f);
                Rect box = *Rect::from_xywh(bx, by, 42.0f, 34.0f);
                if (!canvas.quick_reject(box)) {
                    ++visible_cnt;
                    canvas.fill_rrect(RRect::from_rect_and_radius(box, 6.0f), Paint(Color::from_rgba8(52, 211, 153, 210)));
                } else {
                    ++culled_cnt;
                }
            }
        }
        canvas.restore_to_count(base);

        // Draw culled boxes outside viewport in dimmed red outline
        for (int gy = 0; gy < 4; ++gy) {
            for (int gx = 0; gx < 6; ++gx) {
                const float bx = body.left() + 8.0f + static_cast<float>(gx) * ((body.width() - 28.0f) / 6.0f);
                const float by = body.top() + 8.0f + static_cast<float>(gy) * ((body.height() - 40.0f) / 4.0f);
                Rect box = *Rect::from_xywh(bx, by, 42.0f, 34.0f);
                const bool outside = box.right() <= viewport.left() || box.left() >= viewport.right() ||
                                     box.bottom() <= viewport.top() || box.top() >= viewport.bottom();
                if (outside) {
                    canvas.fill_rrect(RRect::from_rect_and_radius(box, 6.0f), Paint(Color::from_rgba8(244, 63, 94, 55)));
                    canvas.stroke_rrect(RRect::from_rect_and_radius(box, 6.0f), Paint(Color::from_rgba8(244, 63, 94, 180)), Stroke(1.0f));
                }
            }
        }
        canvas.stroke_rect(viewport, Paint(Color::from_rgba8(56, 189, 248, 255)), Stroke(2.0f));
        char stat[64];
        std::snprintf(stat, sizeof(stat), "Visible: %d  |  Culled: %d", visible_cnt, culled_cnt);
        draw_text_line(stat, body.left() + 8.0f, body.bottom() - 4.0f, 11.5f, Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 1.2: Save Stack Ergonomics (save_layer_alpha + rotate(deg, px, py))
    // =========================================================================
    {
        Rect body = begin_card(1, 0, "1.2 SAVE STACK", "save_layer_alpha & Pivot Rotate", Color::from_rgba8(56, 189, 248, 255));
        const float px = body.left() + body.width() * 0.5f;
        const float py = body.top() + body.height() * 0.50f;

        const int root = canvas.save();
        canvas.save_layer_alpha(body, 0.80f);
        const Color petal_cols[6] = {
            Color::from_rgba8(56, 189, 248, 255),
            Color::from_rgba8(99, 102, 241, 255),
            Color::from_rgba8(168, 85, 247, 255),
            Color::from_rgba8(236, 72, 153, 255),
            Color::from_rgba8(251, 146, 60, 255),
            Color::from_rgba8(52, 211, 153, 255)
        };
        for (int i = 0; i < 6; ++i) {
            const int s = canvas.save();
            canvas.rotate(static_cast<float>(i) * 60.0f, px, py);
            Rect petal = *Rect::from_xywh(px - 22.0f, py - 88.0f, 44.0f, 76.0f);
            canvas.fill_rrect(RRect::from_rect_and_radius(petal, 22.0f), Paint(petal_cols[i]));
            canvas.stroke_rrect(RRect::from_rect_and_radius(petal, 22.0f), Paint(Color::from_rgba8(255, 255, 255, 160)), Stroke(1.25f));
            canvas.restore_to_count(s);
        }
        canvas.restore_to_count(root);

        // Pivot center indicator
        canvas.fill_oval(*Rect::from_xywh(px - 8.0f, py - 8.0f, 16.0f, 16.0f), Paint(Color::WHITE));
        canvas.fill_oval(*Rect::from_xywh(px - 4.0f, py - 4.0f, 8.0f, 8.0f), Paint(Color::from_rgba8(15, 23, 42, 255)));
        draw_text_line("6 petals @ rotate(60*i, px, py) + alpha=0.80", body.left() + 4.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 1.3: draw_paint & draw_color Full-Clip Fills
    // =========================================================================
    {
        Rect body = begin_card(2, 0, "1.3 FULL-CLIP FILL", "draw_paint() & draw_color()", Color::from_rgba8(56, 189, 248, 255));
        const float half_w = (body.width() - 12.0f) * 0.5f;
        Rect left_r = *Rect::from_xywh(body.left(), body.top() + 8.0f, half_w, body.height() - 34.0f);
        Rect right_r = *Rect::from_xywh(body.left() + half_w + 12.0f, body.top() + 8.0f, half_w, body.height() - 34.0f);

        // Left: clip_rrect + draw_paint (radial gradient)
        canvas.save();
        canvas.clip_rrect(RRect::from_rect_and_radius(left_r, 14.0f));
        auto rg = RadialGradient::create(
            Point::from_xy(left_r.left() + left_r.width() * 0.5f, left_r.top() + left_r.height() * 0.5f),
            left_r.width() * 0.7f,
            {
                GradientStop::create(0.0f, Color::from_rgba8(250, 204, 21, 255)),
                GradientStop::create(0.55f, Color::from_rgba8(244, 63, 94, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(88, 28, 135, 255))
            }
        );
        if (rg) canvas.draw_paint(Paint(Shader(*rg)));
        canvas.restore();

        // Right: clip_rrect + draw_color + Screen blend overlay
        canvas.save();
        canvas.clip_rrect(RRect::from_rect_and_radius(right_r, 14.0f));
        canvas.draw_color(Color::from_rgba8(30, 64, 175, 255));
        canvas.clip_oval(*Rect::from_xywh(right_r.left() + 18.0f, right_r.top() + 26.0f, right_r.width() - 36.0f, right_r.height() - 52.0f));
        canvas.draw_color(Color::from_rgba8(52, 211, 153, 210), BlendMode::Screen);
        canvas.restore();

        draw_text_line("Radial draw_paint    |    Screen draw_color", body.left() + 6.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 1.4: 9-Slice & Lattice Grid Scaling
    // =========================================================================
    {
        Rect body = begin_card(3, 0, "1.4 9-SLICE / LATTICE", "draw_image_nine & Lattice", Color::from_rgba8(56, 189, 248, 255));
        PixmapPaint pp;
        pp.quality = FilterQuality::Bilinear;

        // Top: 9-slice stretched horizontally
        Rect nine_r = *Rect::from_xywh(body.left() + 4.0f, body.top() + 8.0f, body.width() - 8.0f, 76.0f);
        canvas.draw_image_nine(ui_nine_pm.as_ref(), *IntRect::from_ltrb(8, 8, 24, 24), nine_r, pp);
        draw_text_line("9-Slice UI Panel (Crisp Corners)", nine_r.left() + 20.0f, nine_r.top() + 44.0f, 13.0f, Color::WHITE, ui_font);

        // Bottom: Lattice with Transparent center & FixedColor bottom-right corner
        const std::array<int32_t, 2> x_divs{8, 24};
        const std::array<int32_t, 2> y_divs{8, 24};
        const std::array<Lattice::RectType, 9> rtypes{
            Lattice::RectType::Default, Lattice::RectType::Default,     Lattice::RectType::Default,
            Lattice::RectType::Default, Lattice::RectType::Transparent, Lattice::RectType::Default,
            Lattice::RectType::Default, Lattice::RectType::Default,     Lattice::RectType::FixedColor
        };
        std::array<Color, 9> cols{};
        cols[8] = Color::from_rgba8(251, 191, 36, 255);
        Lattice lat;
        lat.x_divs = x_divs;
        lat.y_divs = y_divs;
        lat.rect_types = rtypes;
        lat.colors = cols;

        Rect lat_r = *Rect::from_xywh(body.left() + 4.0f, body.top() + 98.0f, body.width() - 8.0f, 96.0f);
        canvas.fill_rect(lat_r, Paint(Color::from_rgba8(30, 41, 59, 255)));
        canvas.draw_image_lattice(ui_nine_pm.as_ref(), lat, lat_r, pp);
        draw_text_line("Transparent Hole + FixedColor", lat_r.left() + 24.0f, lat_r.top() + 54.0f, 12.0f,
                       Color::from_rgba8(251, 191, 36, 255), mono_font);
    }

    // =========================================================================
    // CARD 2.1: Difference Clipping (ClipOp::Difference)
    // =========================================================================
    {
        Rect body = begin_card(0, 1, "2.1 DIFF CLIP", "Nested ClipOp::Difference", Color::from_rgba8(168, 85, 247, 255));
        Rect plate = *Rect::from_xywh(body.left() + 10.0f, body.top() + 10.0f, body.width() - 20.0f, body.height() - 36.0f);

        // Checkerboard background visible through punched holes
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 14; ++x) {
                if (((x + y) & 1) == 0) {
                    const float cw = plate.width() / 14.0f;
                    const float ch = plate.height() / 8.0f;
                    canvas.fill_rect(*Rect::from_xywh(plate.left() + x * cw, plate.top() + y * ch, cw, ch),
                                     Paint(Color::from_rgba8(30, 41, 59, 255)));
                }
            }
        }

        canvas.save();
        canvas.clip_rrect(RRect::from_rect_and_radius(plate, 16.0f), ClipOp::Intersect);
        canvas.clip_oval(*Rect::from_xywh(plate.left() + 28.0f, plate.top() + 34.0f, 82.0f, 82.0f), ClipOp::Difference);
        canvas.clip_path(make_star_path(plate.left() + plate.width() * 0.56f, plate.top() + plate.height() * 0.52f, 52.0f, 22.0f, 5),
                         FillRule::Winding, ClipOp::Difference);
        canvas.clip_rect(*Rect::from_xywh(plate.right() - 78.0f, plate.top() + 42.0f, 52.0f, 70.0f), ClipOp::Difference);

        auto grad = LinearGradient::create(
            Point::from_xy(plate.left(), plate.top()), Point::from_xy(plate.right(), plate.bottom()),
            {
                GradientStop::create(0.0f, Color::from_rgba8(236, 72, 153, 255)),
                GradientStop::create(0.5f, Color::from_rgba8(139, 92, 246, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(56, 189, 248, 255))
            }
        );
        if (grad) canvas.draw_paint(Paint(Shader(*grad)));
        canvas.restore();

        draw_text_line("Oval + Star + Rect Difference Cutouts", body.left() + 10.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 2.2: Shader Clipping (clip_shader Intersect & Difference)
    // =========================================================================
    {
        Rect body = begin_card(1, 1, "2.2 SHADER CLIP", "clip_shader() Fade & Portal", Color::from_rgba8(168, 85, 247, 255));
        const float half_w = (body.width() - 12.0f) * 0.5f;
        Rect left_r = *Rect::from_xywh(body.left(), body.top() + 10.0f, half_w, body.height() - 36.0f);
        Rect right_r = *Rect::from_xywh(body.left() + half_w + 12.0f, body.top() + 10.0f, half_w, body.height() - 36.0f);

        // Left: Linear alpha fade Intersect clip_shader
        auto fade_g = LinearGradient::create(
            Point::from_xy(left_r.left(), left_r.top()), Point::from_xy(left_r.right(), left_r.bottom()),
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 255, 255, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(255, 255, 255, 0))
            }
        );
        canvas.save();
        canvas.clip_rrect(RRect::from_rect_and_radius(left_r, 12.0f));
        if (fade_g) canvas.clip_shader(Shader(*fade_g), ClipOp::Intersect);
        canvas.fill_rect(left_r, Paint(Color::from_rgba8(56, 189, 248, 255)));
        for (int i = 0; i < 6; ++i) {
            Rect bar = *Rect::from_xywh(left_r.left() + 10.0f, left_r.top() + 14.0f + i * 26.0f, left_r.width() - 20.0f, 12.0f);
            canvas.fill_rrect(RRect::from_rect_and_radius(bar, 6.0f), Paint(Color::from_rgba8(250, 204, 21, 255)));
        }
        canvas.restore();

        // Right: Radial alpha Difference clip_shader portal
        auto rad_g = RadialGradient::create(
            Point::from_xy(right_r.left() + right_r.width() * 0.5f, right_r.top() + right_r.height() * 0.5f),
            right_r.width() * 0.48f,
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 255, 255, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(255, 255, 255, 0))
            }
        );
        canvas.save();
        canvas.clip_rrect(RRect::from_rect_and_radius(right_r, 12.0f));
        if (rad_g) canvas.clip_shader(Shader(*rad_g), ClipOp::Difference);
        canvas.fill_rect(right_r, Paint(Color::from_rgba8(168, 85, 247, 255)));
        canvas.restore();

        draw_text_line("Intersect Fade     |     Difference Portal", body.left() + 4.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 2.3: Sprite Atlas Batching (RSXform + draw_atlas)
    // =========================================================================
    {
        Rect body = begin_card(2, 1, "2.3 SPRITE ATLAS", "Batched RSXform draw_atlas()", Color::from_rgba8(168, 85, 247, 255));
        const float cx = body.left() + body.width() * 0.5f;
        const float cy = body.top() + body.height() * 0.46f;

        constexpr size_t N = 28;
        std::vector<RSXform> xforms;
        std::vector<Rect> tex_rects;
        std::vector<Color> colors;
        xforms.reserve(N);
        tex_rects.reserve(N);
        colors.reserve(N);

        for (size_t i = 0; i < N; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(N);
            const float ang = t * 4.0f * kPi;
            const float rad = 18.0f + t * 78.0f;
            const float sc = 0.55f + 0.55f * t;
            const float px = cx + std::cos(ang) * rad;
            const float py = cy + std::sin(ang) * rad;
            xforms.push_back(RSXform::from_radians(sc, ang, px, py, 16.0f, 16.0f));
            tex_rects.push_back(*Rect::from_xywh(static_cast<float>((i & 1u) * 32u), static_cast<float>(((i >> 1u) & 1u) * 32u), 32.0f, 32.0f));
            colors.push_back(Color::from_rgba8(255, static_cast<uint8_t>(180 + 75 * t), 255, static_cast<uint8_t>(185 + 70 * t)));
        }
        PixmapPaint pp;
        pp.quality = FilterQuality::Bilinear;
        canvas.draw_atlas(atlas_pm.as_ref(), xforms, tex_rects, colors, BlendMode::Modulate, pp);

        draw_text_line("28 Spiral Sprites (1 Batched GPU Call)", body.left() + 8.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 2.4: Adaptive Coons Patch & Gradient Mesh
    // =========================================================================
    {
        Rect body = begin_card(3, 1, "2.4 GRADIENT MESH", "Adaptive CoonsPatch & Mesh", Color::from_rgba8(168, 85, 247, 255));
        const float ox = body.left() + 18.0f;
        const float oy = body.top() + 26.0f;
        const float pw = body.width() - 36.0f;
        const float ph = body.height() - 72.0f;

        CoonsPatch cp;
        cp.top[0]    = {ox, oy + 12.0f};
        cp.top[1]    = {ox + pw * 0.33f, oy - 24.0f};
        cp.top[2]    = {ox + pw * 0.66f, oy + 26.0f};
        cp.top[3]    = {ox + pw, oy + 4.0f};
        cp.right[0]  = cp.top[3];
        cp.right[1]  = {ox + pw + 18.0f, oy + ph * 0.35f};
        cp.right[2]  = {ox + pw - 16.0f, oy + ph * 0.70f};
        cp.right[3]  = {ox + pw - 4.0f, oy + ph};
        cp.bottom[0] = {ox + 6.0f, oy + ph - 6.0f};
        cp.bottom[1] = {ox + pw * 0.33f, oy + ph - 20.0f};
        cp.bottom[2] = {ox + pw * 0.66f, oy + ph + 24.0f};
        cp.bottom[3] = cp.right[3];
        cp.left[0]   = cp.top[0];
        cp.left[1]   = {ox + 18.0f, oy + ph * 0.35f};
        cp.left[2]   = {ox - 16.0f, oy + ph * 0.65f};
        cp.left[3]   = cp.bottom[0];

        cp.color_top_left     = Color::from_rgba8(244, 63, 94, 255);
        cp.color_top_right    = Color::from_rgba8(250, 204, 21, 255);
        cp.color_bottom_right = Color::from_rgba8(52, 211, 153, 255);
        cp.color_bottom_left  = Color::from_rgba8(56, 189, 248, 255);

        canvas.draw_coons_patch(cp, BlendMode::SourceOver, 1.0f);

        draw_text_line("4-Corner Bicubic Coons Patch (Adaptive)", body.left() + 8.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 3.1: Retained Picture & Live Drawable
    // =========================================================================
    {
        Rect body = begin_card(0, 2, "3.1 DISPLAY LISTS", "PictureRecorder & Drawable", Color::from_rgba8(52, 211, 153, 255));

        PictureRecorder rec;
        ICanvas& rc = rec.begin_recording(*Rect::from_xywh(0.0f, 0.0f, 110.0f, 110.0f));
        rc.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(6.0f, 6.0f, 98.0f, 98.0f), 18.0f),
                      Paint(Color::from_rgba8(30, 64, 175, 255)));
        rc.stroke_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(6.0f, 6.0f, 98.0f, 98.0f), 18.0f),
                        Paint(Color::from_rgba8(125, 211, 252, 255)), Stroke(2.0f));
        rc.fill_path(make_star_path(55.0f, 55.0f, 32.0f, 14.0f, 5), Paint(Color::from_rgba8(250, 204, 21, 255)));
        auto pic = rec.finish_recording();

        // Replay the same immutable Picture 2x with different transforms & alpha
        const Transform t1 = Transform::from_translate(body.left() + 14.0f, body.top() + 26.0f);
        canvas.draw_picture(pic, &t1);

        const Transform t2 = Transform::from_scale(0.82f, 0.82f)
                                 .post_concat(Transform::from_rotate(12.0f))
                                 .post_concat(Transform::from_translate(body.left() + 132.0f, body.top() + 30.0f));
        Paint tint(Color::from_rgba8(255, 255, 255, 200));
        canvas.draw_picture(pic, &t2, &tint);

        // Live Drawable on the right
        LiveEmblemDrawable emblem(34.0f, Color::from_rgba8(16, 185, 129, 255), Color::from_rgba8(167, 243, 208, 255));
        const Transform td = Transform::from_translate(body.right() - 52.0f, body.top() + 106.0f);
        canvas.draw_drawable(emblem, &td);

        draw_text_line("2x Picture Replay   |   Live Drawable", body.left() + 8.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 3.2: Full 4x4 Projective Matrix Stack
    // =========================================================================
    {
        Rect body = begin_card(1, 2, "3.2 4x4 PROJECTIVE", "concat(Transform4x4) 3D UI", Color::from_rgba8(52, 211, 153, 255));
        const float cx = body.left() + body.width() * 0.5f;
        const float cy = body.top() + body.height() * 0.45f;

        canvas.save();
        Transform4x4 orbit = Transform4x4::from_camera_orbit(cx, cy, 16.0f, -28.0f, 0.0f, 420.0f);
        canvas.concat(orbit);

        Rect card3d = *Rect::from_xywh(cx - 115.0f, cy - 68.0f, 230.0f, 136.0f);
        auto g3d = LinearGradient::create(
            Point::from_xy(card3d.left(), card3d.top()), Point::from_xy(card3d.right(), card3d.bottom()),
            {
                GradientStop::create(0.0f, Color::from_rgba8(56, 189, 248, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(99, 102, 241, 255))
            }
        );
        canvas.fill_rrect(RRect::from_rect_and_radius(card3d, 16.0f), g3d ? Paint(Shader(*g3d)) : Paint(Color::from_rgba8(99, 102, 241, 255)));
        canvas.stroke_rrect(RRect::from_rect_and_radius(card3d, 16.0f), Paint(Color::from_rgba8(255, 255, 255, 220)), Stroke(2.5f));
        // Inner UI bars projected in 3D
        canvas.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(card3d.left() + 20.0f, card3d.top() + 24.0f, 130.0f, 18.0f), 9.0f),
                          Paint(Color::from_rgba8(255, 255, 255, 220)));
        canvas.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(card3d.left() + 20.0f, card3d.top() + 56.0f, 185.0f, 14.0f), 7.0f),
                          Paint(Color::from_rgba8(15, 23, 42, 160)));
        canvas.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(card3d.left() + 20.0f, card3d.top() + 82.0f, 145.0f, 14.0f), 7.0f),
                          Paint(Color::from_rgba8(52, 211, 153, 240)));
        canvas.restore();

        draw_text_line("Camera Orbit 4x4 Matrix on Vector RRects", body.left() + 6.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 3.3: Integer Pixel Regions (Boolean Ops + clip_region + draw_region)
    // =========================================================================
    {
        Rect body = begin_card(2, 2, "3.3 PIXEL REGIONS", "Region Algebra + clip_region", Color::from_rgba8(52, 211, 153, 255));
        const int32_t bx = static_cast<int32_t>(body.left()) + 16;
        const int32_t by = static_cast<int32_t>(body.top()) + 18;

        // Left: XOR region rendered with draw_region
        Region xor_rgn(*IntRect::from_xywh(bx, by, 96, 86));
        xor_rgn.xor_with(*IntRect::from_xywh(bx + 42, by + 38, 96, 86));
        canvas.draw_region(xor_rgn, Paint(Color::from_rgba8(168, 85, 247, 235)));

        // Right: Difference region used as device-space clip_region (unaffected by CTM!)
        const int32_t rx = bx + 168;
        Region clip_rgn(*IntRect::from_xywh(rx, by, 134, 124));
        clip_rgn.difference_with(*IntRect::from_xywh(rx + 36, by + 34, 62, 56));
        canvas.save();
        canvas.translate(250.0f, 250.0f); // Must NOT shift device-space clip_region!
        canvas.clip_region(clip_rgn, ClipOp::Intersect);
        canvas.draw_color(Color::from_rgba8(52, 211, 153, 235));
        canvas.restore();

        draw_text_line("XOR draw_region    |   Device clip_region", body.left() + 6.0f, body.bottom() - 4.0f, 10.5f,
                       Color::from_rgba8(203, 213, 225, 255), mono_font);
    }

    // =========================================================================
    // CARD 4.1 & 4.2: TextBlob, RSXform Curved Glyphs, PDF Annots & Discard
    // =========================================================================
    {
        Rect body = begin_card(3, 2, "4.1 & 4.2 TYPOGRAPHY", "TextBlob, RSXform & Annots", Color::from_rgba8(251, 191, 36, 255));

        // 1. Immutable TextBlob with gradient
        auto blob = TextBlob::from_text("Immutable TextBlob", ui_font, 24.0f);
        auto tg = LinearGradient::create(
            Point::from_xy(body.left() + 12.0f, body.top() + 34.0f),
            Point::from_xy(body.right() - 12.0f, body.top() + 34.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(251, 191, 36, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(244, 63, 94, 255))
            }
        );
        canvas.draw_text_blob(blob, body.left() + 16.0f, body.top() + 38.0f, tg ? Paint(Shader(*tg)) : Paint(Color::WHITE));

        // 2. RSXform curved arc glyphs
        const std::string_view arc_txt = "SOVEREIGN ARC TEXT";
        std::vector<uint16_t> gids;
        std::vector<RSXform> xforms;
        gids.reserve(arc_txt.size());
        xforms.reserve(arc_txt.size());

        const float acx = body.left() + body.width() * 0.5f;
        const float acy = body.top() + 186.0f;
        const float arad = 92.0f;
        const float start_a = -2.35f;
        const float step_a = 0.095f;
        for (size_t i = 0; i < arc_txt.size(); ++i) {
            gids.push_back(ui_font.glyph_index(static_cast<char32_t>(arc_txt[i])));
            const float ang = start_a + static_cast<float>(i) * step_a;
            const float tangent = ang + kPi * 0.5f;
            xforms.push_back(RSXform::from_radians(
                1.0f, tangent,
                acx + std::cos(ang) * arad,
                acy + std::sin(ang) * arad,
                0.0f, 0.0f
            ));
        }
        canvas.draw_glyphs(gids, xforms, ui_font, 17.5f, Paint(Color::from_rgba8(56, 189, 248, 255)));

        // 3. Annotation badge (also emits real PDF /Annots URI link when rendered to PdfCanvas!)
        Rect link_r = *Rect::from_xywh(body.left() + 16.0f, body.bottom() - 44.0f, body.width() - 32.0f, 24.0f);
        canvas.fill_rrect(RRect::from_rect_and_radius(link_r, 6.0f), Paint(Color::from_rgba8(30, 58, 138, 180)));
        canvas.stroke_rrect(RRect::from_rect_and_radius(link_r, 6.0f), Paint(Color::from_rgba8(96, 165, 250, 220)), Stroke(1.0f));
        canvas.draw_annotation(link_r, "URL", "https://nisaba.engine/canvas-parity");
        draw_text_line("PDF /Annot Link + GPU discard() Ready", link_r.left() + 18.0f, link_r.top() + 16.0f, 11.0f,
                       Color::from_rgba8(191, 219, 254, 255), mono_font);
    }
}

} // namespace

int main() {
    constexpr uint32_t W = 1600;
    constexpr uint32_t H = 960;

    auto ui_font = TtfFont::from_file(resolve_font("Inter-Regular.ttf"));
    if (!ui_font || !ui_font->is_valid()) {
        ui_font = TtfFont::from_file(resolve_font("NotoSans-Regular.ttf"));
    }
    auto mono_font = TtfFont::from_file(resolve_font("FiraMono-Medium.ttf"));
    if (!ui_font || !ui_font->is_valid()) {
        std::cerr << "Failed to load bundled fonts from fonts/ directory." << std::endl;
        return 1;
    }
    const TtfFont& mono_ref = (mono_font && mono_font->is_valid()) ? *mono_font : *ui_font;

    const Pixmap ui_nine_pm = make_ui_nine_slice_pixmap();
    const Pixmap atlas_pm = make_sprite_atlas_pixmap();

    // 1. CPU Render
    auto cpu_pm = Pixmap::create(W, H);
    if (!cpu_pm) {
        std::cerr << "Failed to allocate CPU Pixmap." << std::endl;
        return 1;
    }
    {
        Canvas cpu_canvas(*cpu_pm);
        render_canvas_parity_showcase(cpu_canvas, *ui_font, mono_ref, ui_nine_pm, atlas_pm, W, H);
    }
    const std::string cpu_out = "nisaba_canvas_parity_showcase_cpu.png";
    cpu_pm->save_png(cpu_out);
    std::cout << "[CPU]    Saved " << cpu_out << " (" << W << "x" << H << ")" << std::endl;

#ifdef NISABA_HAS_GPU
    // 2. OpenGL 3 Render + CPU-vs-GL Parity MAE + discard() verification
    if (auto gl_dev = gpu::GpuDevice::create(gpu::GpuBackendType::OpenGL)) {
        if (auto surface = gpu::GpuSurface::create(gl_dev, W, H, 4)) {
            gpu::GpuCanvas gl_canvas(surface);
            gl_canvas.discard();
            render_canvas_parity_showcase(gl_canvas, *ui_font, mono_ref, ui_nine_pm, atlas_pm, W, H);
            gl_canvas.flush();
            if (auto gl_pm = surface->to_pixmap()) {
                const std::string gl_out = "nisaba_canvas_parity_showcase_gl.png";
                gl_pm->save_png(gl_out);
                const double mae = compute_mae(*cpu_pm, *gl_pm);
                std::cout << "[OpenGL] Saved " << gl_out
                          << " | Device: " << gl_dev->renderer_name()
                          << " | CPU-vs-GL MAE = " << std::fixed << std::setprecision(3) << mae << " / 255" << std::endl;
            }
        }
    }

    // 3. Vulkan Render + CPU-vs-Vulkan Parity MAE + discard() verification
    if (auto vk_dev = gpu::GpuDevice::create(gpu::GpuBackendType::Vulkan)) {
        if (auto surface = gpu::GpuSurface::create(vk_dev, W, H, 4)) {
            gpu::GpuCanvas vk_canvas(surface);
            vk_canvas.discard();
            render_canvas_parity_showcase(vk_canvas, *ui_font, mono_ref, ui_nine_pm, atlas_pm, W, H);
            vk_canvas.flush();
            if (auto vk_pm = surface->to_pixmap()) {
                const std::string vk_out = "nisaba_canvas_parity_showcase_vk.png";
                vk_pm->save_png(vk_out);
                const double mae = compute_mae(*cpu_pm, *vk_pm);
                std::cout << "[Vulkan] Saved " << vk_out
                          << " | Device: " << vk_dev->renderer_name()
                          << " | CPU-vs-VK MAE = " << std::fixed << std::setprecision(3) << mae << " / 255" << std::endl;
            }
        }
    }
#endif

    // 4. PDF Export with live ISO 32000-1 /Annots URI Link
    {
        pdf::PdfDocument doc;
        doc.set_title("Nisaba Canvas Parity Showcase");
        doc.set_author("Nisaba Engine");
        if (pdf::PdfPage* page = doc.add_page(static_cast<float>(W), static_cast<float>(H))) {
            render_canvas_parity_showcase(page->canvas(), *ui_font, mono_ref, ui_nine_pm, atlas_pm, W, H);
            const std::string pdf_out = "nisaba_canvas_parity_showcase.pdf";
            if (doc.save_to_file(pdf_out)) {
                std::cout << "[PDF]    Saved " << pdf_out
                          << " | Annotations emitted: " << page->canvas().annotations().size() << std::endl;
            }
        }
    }

    return 0;
}
