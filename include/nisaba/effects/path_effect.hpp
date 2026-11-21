#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "nisaba/math/transform.hpp"
#include "nisaba/path/path.hpp"

namespace nisaba::effects {

/// Placement style for stamping a vector shape along a path via PathEffect::path_1d.
enum class Path1DStyle : uint8_t {
    Translate = 0, ///< Translate stamp center to each sample point without rotation
    Rotate    = 1, ///< Translate and rotate stamp to match the contour tangent
    Morph     = 2, ///< Deform stamp vertices along contour arc-length and normal
};

/// Operation kind for a composable Paint-level PathEffect (RTTI-free).
enum class PathEffectKind : uint8_t {
    None = 0,
    Corner = 1,
    Trim = 2,
    Discrete = 3,
    Dash = 4,
    Path1D = 5,
    Path2D = 6,
    Compose = 7,
    Sum = 8,
};

/// Immutable node inside a composable PathEffect graph.
struct PathEffectNode {
    PathEffectKind kind{PathEffectKind::None};
    float radius{0.0f};
    float start{0.0f};
    float stop{1.0f};
    bool inverted{false};
    float seg_length{0.0f};
    float deviation{0.0f};
    uint32_t seed{0};
    std::vector<float> intervals{};
    float phase{0.0f};
    float advance{0.0f};
    Path1DStyle style_1d{Path1DStyle::Translate};
    Transform matrix_2d{};
    Path stamp_path{};
    std::shared_ptr<const PathEffectNode> first{nullptr};
    std::shared_ptr<const PathEffectNode> second{nullptr};
};

/// Value-typed handle for composable Paint-level PathEffects with full CPU/GPU parity.
class PathEffect {
public:
    PathEffect() noexcept = default;
    explicit PathEffect(std::shared_ptr<const PathEffectNode> node) noexcept
        : node_(std::move(node)) {}

    /// Rounds sharp polygon/polyline corners with quadratic arcs of the given radius.
    [[nodiscard]] static PathEffect corner(float radius);

    /// Trims path contours to normalized arc-length range [start, stop] in [0, 1].
    [[nodiscard]] static PathEffect trim(float start, float stop, bool inverted = false);

    /// Subdivides contours into segments of length `seg_length` and perturbs vertices
    /// perpendicularly by up to `deviation` using deterministic pseudo-random noise.
    [[nodiscard]] static PathEffect discrete(float seg_length, float deviation, uint32_t seed = 0);

    /// Applies a dash pattern to the path.
    [[nodiscard]] static PathEffect dash(std::vector<float> intervals, float phase = 0.0f);

    /// Stamps `stamp_path` periodically along the target path every `advance` units.
    [[nodiscard]] static PathEffect path_1d(
        const Path& stamp_path,
        float advance,
        float phase = 0.0f,
        Path1DStyle style = Path1DStyle::Translate
    );

    /// Tiles `stamp_path` on a 2D lattice defined by `matrix` over the target path.
    [[nodiscard]] static PathEffect path_2d(const Transform& matrix, const Path& stamp_path);

    /// Composes `outer(inner(path))`: applies `inner` first, then `outer`.
    [[nodiscard]] static PathEffect compose(const PathEffect& outer, const PathEffect& inner);

    /// Applies `first` and `second` independently to the source path and combines both outputs.
    [[nodiscard]] static PathEffect sum(const PathEffect& first, const PathEffect& second);

    [[nodiscard]] bool is_active() const noexcept {
        return node_ != nullptr && node_->kind != PathEffectKind::None;
    }

    [[nodiscard]] PathEffectKind kind() const noexcept {
        return node_ ? node_->kind : PathEffectKind::None;
    }

    /// Returns true if the effect turns a stroke operation into filled stamped geometry
    /// (e.g. Path1D or Path2D).
    [[nodiscard]] bool produces_fill_geometry() const noexcept;

    /// Returns true if the effect requires clipping to the original source path (Path2D).
    [[nodiscard]] bool needs_source_clip() const noexcept;

    /// Evaluates this PathEffect on `src` and returns the transformed vector Path.
    [[nodiscard]] Path apply(const Path& src) const;

    [[nodiscard]] const std::shared_ptr<const PathEffectNode>& node() const noexcept { return node_; }

private:
    std::shared_ptr<const PathEffectNode> node_{nullptr};
};

} // namespace nisaba::effects

