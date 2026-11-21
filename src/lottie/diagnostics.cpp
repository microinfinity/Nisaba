/// @file diagnostics.cpp
/// @brief Implementation of O(1) typed property handles for Lottie PropertyObserver.

#include "nisaba/lottie/diagnostics.hpp"
#include "nisaba/lottie/property.hpp"
#include "nisaba/lottie/layer.hpp"

namespace nisaba::lottie {

namespace {

inline void bump(const std::shared_ptr<std::atomic<uint64_t>>& gen) noexcept {
    if (gen) {
        gen->fetch_add(1, std::memory_order_relaxed);
    }
}

} // anonymous namespace

// ── ColorHandle ───────────────────────────────────────────────────────────

Color ColorHandle::get() const noexcept {
    auto guard = owner_.lock();
    if (!guard) return Color::TRANSPARENT;
    if (color_prop_) {
        return color_prop_->evaluate(0.0f);
    }
    if (solid_layer_) {
        return solid_layer_->color_override.value_or(solid_layer_->solid_color);
    }
    return Color::TRANSPARENT;
}

void ColorHandle::set(Color color) {
    auto guard = owner_.lock();
    if (!guard) return;
    if (color_prop_) {
        color_prop_->set_override(color);
        bump(mutation_gen_);
    } else if (solid_layer_) {
        solid_layer_->color_override = color;
        bump(mutation_gen_);
    }
}

void ColorHandle::clear() {
    auto guard = owner_.lock();
    if (!guard) return;
    if (color_prop_) {
        color_prop_->clear_override();
        bump(mutation_gen_);
    } else if (solid_layer_) {
        solid_layer_->color_override.reset();
        bump(mutation_gen_);
    }
}

// ── OpacityHandle ─────────────────────────────────────────────────────────

float OpacityHandle::get() const noexcept {
    auto guard = owner_.lock();
    if (!guard || !opacity_prop_) return 0.0f;
    return opacity_prop_->evaluate(0.0f);
}

void OpacityHandle::set(float opacity_0_to_100) {
    auto guard = owner_.lock();
    if (!guard || !opacity_prop_) return;
    opacity_prop_->set_override(opacity_0_to_100);
    bump(mutation_gen_);
}

void OpacityHandle::clear() {
    auto guard = owner_.lock();
    if (!guard || !opacity_prop_) return;
    opacity_prop_->clear_override();
    bump(mutation_gen_);
}

// ── TransformHandle ───────────────────────────────────────────────────────

TransformPropertyValue TransformHandle::get() const noexcept {
    TransformPropertyValue v;
    auto guard = owner_.lock();
    if (!guard || !transform_) return v;

    v.anchor = transform_->anchor.evaluate(0.0f);
    if (transform_->x_pos.has_value() || transform_->y_pos.has_value()) {
        const float px = transform_->x_pos ? transform_->x_pos->evaluate(0.0f) : 0.0f;
        const float py = transform_->y_pos ? transform_->y_pos->evaluate(0.0f) : 0.0f;
        v.position = Point::from_xy(px, py);
    } else {
        v.position = transform_->position.evaluate(0.0f);
    }
    v.scale = transform_->scale.evaluate(0.0f);
    v.rotation = transform_->rotation.evaluate(0.0f);
    v.skew = transform_->skew.evaluate(0.0f);
    v.skew_axis = transform_->skew_axis.evaluate(0.0f);
    return v;
}

void TransformHandle::set(const TransformPropertyValue& value) {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;

    transform_->anchor.set_override(value.anchor);
    if (transform_->x_pos.has_value() || transform_->y_pos.has_value()) {
        if (transform_->x_pos) transform_->x_pos->set_override(value.position.x);
        if (transform_->y_pos) transform_->y_pos->set_override(value.position.y);
    } else {
        transform_->position.set_override(value.position);
    }
    transform_->scale.set_override(value.scale);
    transform_->rotation.set_override(value.rotation);
    transform_->skew.set_override(value.skew);
    transform_->skew_axis.set_override(value.skew_axis);
    bump(mutation_gen_);
}

void TransformHandle::set_anchor(Point anchor) {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;
    transform_->anchor.set_override(anchor);
    bump(mutation_gen_);
}

void TransformHandle::set_position(Point position) {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;
    if (transform_->x_pos.has_value() || transform_->y_pos.has_value()) {
        if (transform_->x_pos) transform_->x_pos->set_override(position.x);
        if (transform_->y_pos) transform_->y_pos->set_override(position.y);
    } else {
        transform_->position.set_override(position);
    }
    bump(mutation_gen_);
}

void TransformHandle::set_scale(Point scale) {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;
    transform_->scale.set_override(scale);
    bump(mutation_gen_);
}

void TransformHandle::set_rotation(float degrees) {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;
    transform_->rotation.set_override(degrees);
    bump(mutation_gen_);
}

void TransformHandle::set_skew(float skew_deg, float skew_axis_deg) {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;
    transform_->skew.set_override(skew_deg);
    transform_->skew_axis.set_override(skew_axis_deg);
    bump(mutation_gen_);
}

void TransformHandle::clear() {
    auto guard = owner_.lock();
    if (!guard || !transform_) return;
    transform_->anchor.clear_override();
    transform_->position.clear_override();
    if (transform_->x_pos) transform_->x_pos->clear_override();
    if (transform_->y_pos) transform_->y_pos->clear_override();
    transform_->scale.clear_override();
    transform_->rotation.clear_override();
    transform_->skew.clear_override();
    transform_->skew_axis.clear_override();
    bump(mutation_gen_);
}

// ── TextHandle ────────────────────────────────────────────────────────────

TextPropertyValue TextHandle::get() const {
    TextPropertyValue out;
    auto guard = owner_.lock();
    if (!guard || !text_layer_) return out;

    const TextDocument& doc = text_layer_->doc_override.has_value()
        ? *text_layer_->doc_override
        : text_layer_->evaluate_document(0.0f);

    out.text = text_layer_->text_override.has_value() ? *text_layer_->text_override : doc.text;
    out.font_name = doc.font_name;
    out.font_size = doc.font_size;
    out.fill_color = doc.fill_color;
    out.stroke_color = doc.stroke_color;
    out.stroke_width = doc.stroke_width;
    out.stroke_over_fill = doc.stroke_over_fill;
    out.justify = doc.justify;
    out.tracking = doc.tracking;
    out.line_height = doc.line_height;
    out.baseline_shift = doc.baseline_shift;
    return out;
}

void TextHandle::set(const TextPropertyValue& value) {
    auto guard = owner_.lock();
    if (!guard || !text_layer_) return;

    TextDocument doc = text_layer_->doc_override.value_or(text_layer_->evaluate_document(0.0f));
    doc.text = value.text;
    if (!value.font_name.empty()) doc.font_name = value.font_name;
    doc.font_size = value.font_size;
    doc.fill_color = value.fill_color;
    doc.stroke_color = value.stroke_color;
    doc.stroke_width = value.stroke_width;
    doc.stroke_over_fill = value.stroke_over_fill;
    doc.justify = value.justify;
    doc.tracking = value.tracking;
    doc.line_height = value.line_height;
    doc.baseline_shift = value.baseline_shift;

    text_layer_->doc_override = std::move(doc);
    text_layer_->text_override.reset();
    bump(mutation_gen_);
}

void TextHandle::set_text(std::string text) {
    auto guard = owner_.lock();
    if (!guard || !text_layer_) return;
    if (text_layer_->doc_override.has_value()) {
        text_layer_->doc_override->text = std::move(text);
    } else {
        text_layer_->text_override = std::move(text);
    }
    bump(mutation_gen_);
}

void TextHandle::set_fill_color(Color color) {
    auto guard = owner_.lock();
    if (!guard || !text_layer_) return;
    if (!text_layer_->doc_override.has_value()) {
        text_layer_->doc_override = text_layer_->evaluate_document(0.0f);
        if (text_layer_->text_override.has_value()) {
            text_layer_->doc_override->text = *text_layer_->text_override;
            text_layer_->text_override.reset();
        }
    }
    text_layer_->doc_override->fill_color = color;
    bump(mutation_gen_);
}

void TextHandle::set_font_size(float size) {
    auto guard = owner_.lock();
    if (!guard || !text_layer_) return;
    if (!text_layer_->doc_override.has_value()) {
        text_layer_->doc_override = text_layer_->evaluate_document(0.0f);
        if (text_layer_->text_override.has_value()) {
            text_layer_->doc_override->text = *text_layer_->text_override;
            text_layer_->text_override.reset();
        }
    }
    text_layer_->doc_override->font_size = size;
    bump(mutation_gen_);
}

void TextHandle::clear() {
    auto guard = owner_.lock();
    if (!guard || !text_layer_) return;
    text_layer_->text_override.reset();
    text_layer_->doc_override.reset();
    bump(mutation_gen_);
}

} // namespace nisaba::lottie

