#pragma once

/// @file expression.hpp
/// @brief Pluggable ExpressionManager and typed ExpressionEvaluator interfaces for arbitrary Lottie expressions.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nisaba::lottie {

/// Abstract runtime evaluator bound to a single Lottie expression script (`"x"`).
template <typename T>
class ExpressionEvaluator {
public:
    virtual ~ExpressionEvaluator() = default;

    /// Evaluates the expression at composition time `t_seconds`.
    [[nodiscard]] virtual T evaluate(float t_seconds) = 0;
};

using NumberExpressionEvaluator = ExpressionEvaluator<float>;
using StringExpressionEvaluator = ExpressionEvaluator<std::string>;
using ArrayExpressionEvaluator = ExpressionEvaluator<std::vector<float>>;

/// Convenience lambda-backed `ExpressionEvaluator<T>`.
template <typename T>
class CallbackExpressionEvaluator final : public ExpressionEvaluator<T> {
public:
    explicit CallbackExpressionEvaluator(std::function<T(float)> fn)
        : fn_(std::move(fn)) {}

    [[nodiscard]] T evaluate(float t_seconds) override {
        return fn_ ? fn_(t_seconds) : T{};
    }

private:
    std::function<T(float)> fn_;
};

/// Abstract factory enabling host applications to supply custom JS/math expression evaluators
/// for arbitrary `"x"` scripts alongside the built-in zero-overhead fast expressions.
class ExpressionManager {
public:
    virtual ~ExpressionManager() = default;

    /// Creates an evaluator for scalar floating-point expressions (opacity, rotation, width, etc.).
    /// Return `nullptr` to fall back to built-in expressions / keyframes.
    [[nodiscard]] virtual std::shared_ptr<ExpressionEvaluator<float>> create_number_expression_evaluator(
        std::string_view expression
    ) {
        (void)expression;
        return nullptr;
    }

    /// Creates an evaluator for string expressions (e.g. dynamic TextDocument `"t.d.x"`).
    /// Return `nullptr` to fall back to static/keyframed text.
    [[nodiscard]] virtual std::shared_ptr<ExpressionEvaluator<std::string>> create_string_expression_evaluator(
        std::string_view expression
    ) {
        (void)expression;
        return nullptr;
    }

    /// Creates an evaluator for vector/array expressions (2D/3D points, scale, RGBA colors, float arrays).
    /// Return `nullptr` to fall back to built-in expressions / keyframes.
    [[nodiscard]] virtual std::shared_ptr<ExpressionEvaluator<std::vector<float>>> create_array_expression_evaluator(
        std::string_view expression
    ) {
        (void)expression;
        return nullptr;
    }
};

/// Convenience `ExpressionManager` adapter backed by user-supplied callbacks.
class CallbackExpressionManager final : public ExpressionManager {
public:
    using NumberFactory = std::function<std::shared_ptr<ExpressionEvaluator<float>>(std::string_view)>;
    using StringFactory = std::function<std::shared_ptr<ExpressionEvaluator<std::string>>(std::string_view)>;
    using ArrayFactory = std::function<std::shared_ptr<ExpressionEvaluator<std::vector<float>>>(std::string_view)>;

    NumberFactory number_factory;
    StringFactory string_factory;
    ArrayFactory array_factory;

    [[nodiscard]] std::shared_ptr<ExpressionEvaluator<float>> create_number_expression_evaluator(
        std::string_view expression
    ) override {
        return number_factory ? number_factory(expression) : nullptr;
    }

    [[nodiscard]] std::shared_ptr<ExpressionEvaluator<std::string>> create_string_expression_evaluator(
        std::string_view expression
    ) override {
        return string_factory ? string_factory(expression) : nullptr;
    }

    [[nodiscard]] std::shared_ptr<ExpressionEvaluator<std::vector<float>>> create_array_expression_evaluator(
        std::string_view expression
    ) override {
        return array_factory ? array_factory(expression) : nullptr;
    }
};

} // namespace nisaba::lottie

