#pragma once

#include <cmath>
#include <optional>
#include <vector>

#include "extension_api.h"

// This is the stable, user-editable extension surface. It is intentionally a
// template so the header does not depend on Fates' internal Config definition.
// Keep the core search engine untouched and place private defaults here.
namespace fates {

inline void register_user_extensions(ExtensionRegistry& extensions) {
    // This optional operation demonstrates the stable operation API without
    // changing the default search alphabet. Enable it with --ops ...,sigmoid.
    // Hyperbolic functions are native operations, so the example intentionally
    // uses a distinct name that remains safe when the built-in set grows.
    UnaryOperationExtension sigmoid;
    sigmoid.name = "sigmoid";
    sigmoid.aliases = {"logistic"};
    sigmoid.default_cost = 2;
    sigmoid.evaluate = [](double value, const ExtensionLimits&) -> std::optional<double> {
        const double exponential = std::exp(value < 0.0 ? value : -value);
        const double result = value < 0.0 ? exponential / (1.0 + exponential)
                                          : 1.0 / (1.0 + exponential);
        return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
    };
    sigmoid.derivative = [](double, double result, double child_derivative) {
        return result * (1.0 - result) * child_derivative;
    };
    sigmoid.inverse = [](double target, const ExtensionLimits&) {
        if (target <= 0.0 || target >= 1.0) return std::vector<double>{};
        return std::vector<double>{std::log(target / (1.0 - target))};
    };
    sigmoid.render_latex = [](std::string_view child) {
        return "\\operatorname{sigmoid}\\left(" + std::string(child) + "\\right)";
    };
    extensions.add_unary(std::move(sigmoid));

    // Add another unary or binary operation by filling an extension object:
    // BinaryOperationExtension average;
    // average.name = "avg";
    // average.commutative = true;
    // average.evaluate = [](double a, double b, const ExtensionLimits&) {
    //     return std::optional<double>{(a + b) / 2.0};
    // };
    // extensions.add_binary(std::move(average));

    // Stateful structural constraints share a separate 32-bit extension state.
    // Multiple constraints may be registered when their state_bits total <= 32.
    // Atom/unary/binary callbacks return nullopt to prune a candidate early;
    // satisfied() decides whether a completed result may be shown.
    (void)extensions;
}

template <typename ConfigType>
inline void configure_user_extensions(ConfigType& cfg) {
    // Add custom named constants:
    // cfg.custom_constants.push_back("G=0.915965594177219:2");

    // Permanently constrain atom counts or controlled leaf order:
    // cfg.symbol_count_specs.push_back("pi=4");
    // cfg.required_symbol_order = {"1", "1", "4", "5", "1", "4"};

    // Change private-build defaults without editing the engine:
    // cfg.max_integer = 25;
    // cfg.ops = "+,-,*,/,^,neg,inv,sqrt,ln,exp";
    // cfg.beam = 6000;
    // cfg.pair_budget = 8'000'000;

    (void)cfg;
}

}  // namespace fates
