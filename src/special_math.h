#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace fates::special_math {

// Euler--Maclaurin summation for the real zeta function on s > 1. Return the
// value and its derivative together so equation search uses the same formula.
inline std::optional<std::pair<double, double>> zeta(double s) {
    if (!std::isfinite(s) || s <= 1.0) return std::nullopt;
    constexpr int n = 24;
    constexpr std::array<long double, 7> bernoulli_over_factorial{
        1.0L / 12.0L, -1.0L / 720.0L, 1.0L / 30240.0L,
        -1.0L / 1209600.0L, 1.0L / 47900160.0L,
        -691.0L / 1307674368000.0L, 7.0L / 523069747200.0L,
    };
    const long double exponent = s;
    const long double log_n = std::log(static_cast<long double>(n));
    long double sum = 0.0L;
    long double slope = 0.0L;
    for (int index = 1; index < n; ++index) {
        const long double log_index = std::log(static_cast<long double>(index));
        const long double term = std::exp(-exponent * log_index);
        sum += term;
        slope -= log_index * term;
    }
    const long double tail = std::exp((1.0L - exponent) * log_n) / (exponent - 1.0L);
    sum += tail;
    slope -= tail * (log_n + 1.0L / (exponent - 1.0L));
    const long double half_term = 0.5L * std::exp(-exponent * log_n);
    sum += half_term;
    slope -= log_n * half_term;

    long double rising = 1.0L;
    long double harmonic = 0.0L;
    for (std::size_t index = 0; index < bernoulli_over_factorial.size(); ++index) {
        const int order = static_cast<int>(2 * index + 1);
        for (int factor = index == 0 ? 0 : order - 2; factor < order; ++factor) {
            rising *= exponent + factor;
            harmonic += 1.0L / (exponent + factor);
        }
        const long double term = bernoulli_over_factorial[index] * rising *
            std::exp((-exponent - order) * log_n);
        sum += term;
        slope += term * (harmonic - log_n);
    }
    if (!std::isfinite(sum) || !std::isfinite(slope)) return std::nullopt;
    return std::pair{static_cast<double>(sum), static_cast<double>(slope)};
}

// Orders zero and one of the Bessel J function. The power series is portable
// to Android libc++, which does not guarantee C++17 special math functions.
// The bound avoids cancellation of very large intermediate terms.
inline std::optional<std::pair<double, double>> bessel_j(int order, double x) {
    if (!std::isfinite(x) || std::abs(x) > 16.0 || (order != 0 && order != 1)) {
        return std::nullopt;
    }
    const long double half = static_cast<long double>(x) / 2.0L;
    const long double square = -half * half;
    const auto series = [&](int degree) {
        long double term = degree == 0 ? 1.0L : half;
        long double value = term;
        for (int index = 1; index <= 80; ++index) {
            term *= square / (static_cast<long double>(index) * (index + degree));
            value += term;
            if (std::abs(term) < 1.0e-21L) break;
        }
        return static_cast<double>(value);
    };
    const double j0 = series(0);
    const double j1 = series(1);
    const double value = order == 0 ? j0 : j1;
    const double slope = order == 0 ? -j1 : (x == 0.0 ? 0.5 : j0 - j1 / x);
    return std::pair{value, slope};
}

// Complete elliptic integrals K(k) and E(k), using the AGM with the modulus
// convention (rather than the parameter m = k^2). Both are even in k.
inline std::optional<std::pair<double, double>> elliptic_complete(double k) {
    if (!std::isfinite(k) || std::abs(k) >= 1.0) return std::nullopt;
    long double a = 1.0L;
    long double b = std::sqrt((1.0L - k) * (1.0L + k));
    long double correction = static_cast<long double>(k) * k / 2.0L;
    long double weight = 1.0L;
    for (int index = 0; index < 32; ++index) {
        const long double c = (a - b) / 2.0L;
        if (std::abs(c) <= 2.0L * std::numeric_limits<long double>::epsilon() * a) break;
        correction += weight * c * c;
        weight *= 2.0L;
        const long double next_a = (a + b) / 2.0L;
        b = std::sqrt(a * b);
        a = next_a;
    }
    const long double first = std::numbers::pi_v<long double> / (2.0L * a);
    const long double second = first * (1.0L - correction);
    return std::pair{static_cast<double>(first), static_cast<double>(second)};
}

inline double elliptic_derivative(int kind, double k, double first, double second) {
    if (k == 0.0) return 0.0;
    if (kind == 1) return (second / (1.0 - k * k) - first) / k;
    return (second - first) / k;
}

}  // namespace fates::special_math
