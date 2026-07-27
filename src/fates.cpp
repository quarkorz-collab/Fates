#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fast_containers.h"
#include "extension_api.h"
#include "user_extensions.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#if defined(_MSC_VER)
#include <ppl.h>
#endif
#endif

namespace fates {

using ExprId = std::uint32_t;
constexpr ExprId kNoExpr = std::numeric_limits<ExprId>::max();
inline constexpr std::string_view kProgramName = "Fates";
inline constexpr std::string_view kProgramFullName = "Finding Algebraic Targets via Expression Search";
inline constexpr std::string_view kProgramVersion = "1.0";

static void print_banner(std::ostream& output);

#ifdef _WIN32
static std::string utf16_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-16 文本过长，无法转换为 UTF-8");
    }
    const int source_size = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0, nullptr, nullptr);
    if (required == 0) throw std::runtime_error("命令行包含无效的 UTF-16 文本");
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), source_size,
                            output.data(), required, nullptr, nullptr) == 0) {
        throw std::runtime_error("无法将命令行转换为 UTF-8");
    }
    return output;
}

static std::filesystem::path path_from_utf8(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("参数文件路径过长");
    }
    const int source_size = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0);
    if (required == 0) throw std::runtime_error("参数文件路径不是有效的 UTF-8");
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size,
                            output.data(), required) == 0) {
        throw std::runtime_error("无法将参数文件路径转换为 UTF-16");
    }
    return std::filesystem::path(std::move(output));
}

static std::string path_to_utf8(const std::filesystem::path& path) {
    return utf16_to_utf8(path.native());
}
#else
static std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::string(text));
}

static std::string path_to_utf8(const std::filesystem::path& path) {
    return path.string();
}
#endif

static std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        out.push_back(trim(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin)));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return out;
}

static std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

static std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

static std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a * b;
}

static std::uint64_t saturating_triangle(std::uint64_t n) {
    return (n % 2 == 0) ? saturating_mul(n / 2, n + 1)
                        : saturating_mul(n, (n + 1) / 2);
}

static std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a + b;
}

enum class NodeTag : std::uint8_t { Atom, Unary, Binary };

enum class UnaryKind : std::uint8_t {
    Neg,
    Inv,
    Sqrt,
    Cbrt,
    Sqr,
    Cube,
    Ln,
    Log10,
    Exp,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
    Gamma,
    Abs,
    Fact,
};

enum class BinaryKind : std::uint8_t { Add, Sub, Mul, Div, Pow };

enum class ValuePruneMode : std::uint8_t {
    Bucket,
    Exact,
};

// Equation matching is deliberately separate from the ordinary expression
// search.  A value that is useful for finding a constant target is not
// necessarily useful as one side of f(x) = g(x), so equation mode has its own
// pair sampling policy.  The default remains the inexpensive stable policy;
// wider policies are opt-in and never affect constant searches.
enum class EquationSearchMode : std::uint8_t {
    Stable,
    Wide,
    Exhaustive,
};

enum class EquationQualityMode : std::uint8_t {
    Strict,
    Local,
    Off,
};

static constexpr std::string_view value_prune_mode_name(ValuePruneMode mode) {
    return mode == ValuePruneMode::Exact ? "exact" : "bucket";
}

static constexpr std::string_view equation_search_mode_name(EquationSearchMode mode) {
    switch (mode) {
        case EquationSearchMode::Stable: return "stable";
        case EquationSearchMode::Wide: return "wide";
        case EquationSearchMode::Exhaustive: return "exhaustive";
    }
    return "stable";
}

static constexpr std::string_view equation_quality_mode_name(EquationQualityMode mode) {
    switch (mode) {
        case EquationQualityMode::Strict: return "strict";
        case EquationQualityMode::Local: return "local";
        case EquationQualityMode::Off: return "off";
    }
    return "strict";
}

inline constexpr std::uint8_t kCustomOperationBase = 64;
inline constexpr std::size_t kMaximumCustomOperations = 256 - kCustomOperationBase;

static const ExtensionRegistry& extension_registry() {
    static const ExtensionRegistry registry = [] {
        ExtensionRegistry configured;
        register_user_extensions(configured);
        if (configured.unary_operations().size() > kMaximumCustomOperations ||
            configured.binary_operations().size() > kMaximumCustomOperations) {
            throw std::runtime_error("自定义一元或二元运算数量不能超过 192");
        }
        const std::set<std::string_view> builtin_names{
            "neg", "~", "inv", "recip", "sqrt", "cbrt", "sqr", "square", "cube",
            "ln", "log", "log10", "exp", "sin", "cos", "tan", "asin", "acos",
            "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "arcsinh",
            "arccosh", "arctanh", "gamma", "Gamma", "Γ", "abs", "fact", "!", "+",
            "add", "-", "sub", "*", "mul", "x", "/", "div", "^", "pow"};
        const auto validate_names = [&](const auto& operation) {
            if (builtin_names.contains(operation.name)) {
                throw std::runtime_error("自定义运算名称与内置运算冲突: " + operation.name);
            }
            for (const std::string& alias : operation.aliases) {
                if (builtin_names.contains(alias)) {
                    throw std::runtime_error("自定义运算别名与内置运算冲突: " + alias);
                }
            }
        };
        for (const auto& operation : configured.unary_operations()) validate_names(operation);
        for (const auto& operation : configured.binary_operations()) validate_names(operation);
        return configured;
    }();
    return registry;
}

static std::optional<std::size_t> custom_unary_index(UnaryKind kind) {
    const unsigned value = static_cast<std::uint8_t>(kind);
    if (value < kCustomOperationBase) return std::nullopt;
    const std::size_t index = value - kCustomOperationBase;
    if (index >= extension_registry().unary_operations().size()) return std::nullopt;
    return index;
}

static std::optional<std::size_t> custom_binary_index(BinaryKind kind) {
    const unsigned value = static_cast<std::uint8_t>(kind);
    if (value < kCustomOperationBase) return std::nullopt;
    const std::size_t index = value - kCustomOperationBase;
    if (index >= extension_registry().binary_operations().size()) return std::nullopt;
    return index;
}

struct UnarySpec {
    UnaryKind kind{};
    std::uint16_t cost{};
};

struct BinarySpec {
    BinaryKind kind{};
    std::uint16_t cost{};
};

static bool is_commutative(BinaryKind kind) {
    switch (kind) {
        case BinaryKind::Add:
        case BinaryKind::Mul:
            return true;
        case BinaryKind::Sub:
        case BinaryKind::Div:
        case BinaryKind::Pow:
            return false;
        default:
            const auto custom = custom_binary_index(kind);
            return custom && extension_registry().binary_operations()[*custom].commutative;
    }
}

struct AtomSpec {
    std::string text;
    double value{};
    std::uint16_t cost{};
    bool variable{};
};

struct SymbolCountRule {
    std::string symbol;
    std::uint32_t minimum{};
    std::uint32_t maximum{};
    std::uint16_t atom_cost{};
    std::uint8_t shift{};
    std::uint8_t bits{};
    bool unlimited{};
};

struct SymbolConstraintPlan {
    std::vector<SymbolCountRule> count_rules;
    std::vector<std::string> required_order;
    std::vector<std::uint16_t> order_atom_cost;
    std::uint32_t order_symbol_mask{};
    std::uint32_t unit_once_mask{};
    std::uint8_t order_mask_shift{};
    std::uint8_t order_length_shift{};
    std::uint8_t order_length_bits{};
    unsigned max_cost{};
    std::uint64_t maximum_missing_atom_cost{};
    bool active{};

    std::optional<std::uint32_t> atom_state(std::string_view symbol) const;
    std::optional<std::uint32_t> combine(std::uint32_t left, std::uint32_t right) const;
    bool satisfied(std::uint32_t state) const;
    bool can_finish(std::uint32_t state, unsigned current_cost) const;
};

struct CompiledConstraintExtension {
    const ConstraintExtension* extension{};
    std::uint8_t shift{};
    std::uint32_t mask{};
};

struct ExtensionConstraintPlan {
    std::vector<CompiledConstraintExtension> constraints;
    bool active{};

    std::optional<std::uint32_t> atom_state(const AtomSpec& atom) const;
    std::optional<std::uint32_t> apply_unary(std::uint32_t state,
                                             std::string_view operation,
                                             double input,
                                             double value,
                                             unsigned total_cost,
                                             unsigned max_cost) const;
    std::optional<std::uint32_t> apply_binary(std::uint32_t left,
                                              std::uint32_t right,
                                              std::string_view operation,
                                              double left_value,
                                              double right_value,
                                              double value,
                                              unsigned total_cost,
                                              unsigned max_cost) const;
    bool satisfied(std::uint32_t state) const;
    bool can_finish(std::uint32_t state, unsigned current_cost, unsigned max_cost) const;
};

using ConstraintState = std::uint64_t;

struct Node {
    double value{};
    double derivative{};
    std::uint64_t hash{};
    ConstraintState constraint_state{};
    ExprId left{kNoExpr};
    ExprId right{kNoExpr};
    std::uint32_t atom_index{};
    std::uint16_t cost{};
    std::uint16_t nodes{};
    std::uint16_t depth{};
    NodeTag tag{NodeTag::Atom};
    std::uint8_t op{};
    bool depends_on_x{};
    bool eligible{true};
};

struct Candidate {
    double value{};
    double derivative{};
    std::uint64_t hash{};
    ConstraintState constraint_state{};
    ExprId left{kNoExpr};
    ExprId right{kNoExpr};
    std::uint32_t atom_index{};
    std::uint16_t cost{};
    std::uint16_t nodes{};
    std::uint16_t depth{};
    NodeTag tag{NodeTag::Atom};
    std::uint8_t op{};
    bool depends_on_x{};
};

static_assert(sizeof(Node) <= 56, "Node constraint state must fit in existing tail padding");
static_assert(sizeof(Candidate) <= 56, "Candidate constraint state must fit in existing tail padding");

struct ErrorRange {
    double lower = -std::numeric_limits<double>::infinity();
    double upper = std::numeric_limits<double>::infinity();
    bool include_lower = false;
    bool include_upper = false;

    bool contains(double signed_error) const {
        if (!std::isfinite(signed_error)) return false;
        const bool above = include_lower ? signed_error >= lower : signed_error > lower;
        const bool below = include_upper ? signed_error <= upper : signed_error < upper;
        return above && below;
    }
};

struct Config {
    double target = std::numeric_limits<double>::quiet_NaN();
    std::string digits = "123456789";
    unsigned max_literal_len = 2;
    std::uint64_t max_integer = 25;
    unsigned digit_cost = 1;
    std::string constants = "pi,e,phi";
    std::vector<std::string> custom_constants;
    std::string ops = "+,-,*,/,^,neg,inv,sqrt,ln,exp";
    std::vector<std::string> symbol_count_specs;
    std::vector<std::string> required_symbol_order;
    SymbolConstraintPlan symbol_constraints;
    ExtensionConstraintPlan extension_constraints;

    unsigned max_cost = 10;
    unsigned side_cost = 0;
    unsigned deep_rounds = 0;
    std::size_t deep_beam = 0;
    std::size_t deep_frontier = 0;
    std::size_t beam = 3000;
    std::uint64_t pair_budget = 4'000'000;
    std::size_t task_chunks = 64;
    unsigned threads = std::max(1U, std::thread::hardware_concurrency());
    unsigned value_bits = 42;
    ValuePruneMode value_prune = ValuePruneMode::Bucket;
    std::size_t explore_pairs = 0;
    unsigned pareto_slots = 1;
    std::size_t pareto_extra = 0;
    std::size_t inverse_neighbors = 5;
    unsigned inverse_depth = 0;
    std::size_t inverse_beam = 64;
    std::uint64_t inverse_budget = 1'000'000;
    double max_abs = 1.0e100;
    double max_exponent = 32.0;
    double max_trig_arg = 1.0e6;
    double epsilon = 1.0e-12;
    ErrorRange error_range;
    std::size_t results = 20;
    std::size_t equation_neighbors = 64;
    EquationSearchMode equation_search = EquationSearchMode::Stable;
    EquationQualityMode equation_quality = EquationQualityMode::Strict;
    std::size_t genetic_population = 4096;
    unsigned genetic_generations = 64;
    std::uint64_t genetic_seed = 0x243f6a8885a308d3ULL;
    double genetic_elegance = 0.35;
    double genetic_repair = 0.15;
    unsigned genetic_repair_depth = 3;
    double genetic_crossover = 0.25;
    double genetic_novelty = 0.20;
    unsigned genetic_tournament = 4;
    std::size_t pslq_basis = 192;
    std::uint64_t pslq_pairs = 4'096;
    std::int64_t pslq_max_coefficient = 64;
    unsigned pslq_steps = 64;
    double pslq_tolerance = 1.0e-12;
    std::size_t egraph_seeds = 768;
    unsigned egraph_rounds = 2;
    std::size_t egraph_node_limit = 4'096;
    std::size_t mcts_iterations = 8'192;
    unsigned mcts_depth = 4;
    unsigned mcts_branching = 12;
    double mcts_exploration = 1.25;
    double mcts_elegance = 0.25;
    std::uint64_t mcts_seed = 0x13198a2e03707344ULL;
    std::size_t live_top = 0;
    double live_interval = 2.0;
    std::size_t max_atoms = 200'000;
    std::string mode = "nearest";

    bool json = false;
    bool bidirectional = true;
    bool equations = false;
    bool genetic = false;
    bool pslq = false;
    bool egraph = false;
    bool mcts = false;
    bool portfolio = false;
    bool live = false;
    bool live_json = false;
    bool latex = false;
    bool verbose = false;
    bool show_stats = true;
    bool stop_on_epsilon = true;
    bool list_symbols = false;
    bool self_test = false;
    bool dry_run = false;
};

static unsigned state_value_bits(const Config& cfg) {
    return cfg.value_prune == ValuePruneMode::Exact ? 52U : cfg.value_bits;
}

static void configure_source_extensions(Config& cfg) {
    configure_user_extensions(cfg);
}

static std::uint64_t value_bucket(double value, unsigned mantissa_bits) {
    if (value == 0.0) return 0;
    std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const unsigned keep = std::min(52U, mantissa_bits);
    const unsigned drop = 52U - keep;
    if (drop > 0) {
        const std::uint64_t low_mask = (drop == 64U) ? ~0ULL : ((1ULL << drop) - 1ULL);
        bits &= ~low_mask;
    }
    return bits;
}

static std::uint64_t state_bucket(double value,
                                  double derivative,
                                  bool depends_on_x,
                                  ConstraintState constraint_state,
                                  unsigned mantissa_bits,
                                  bool derivative_sensitive) {
    const std::uint64_t value_key = value_bucket(value, mantissa_bits);
    std::uint64_t key = value_key;
    if (derivative_sensitive) {
        const std::uint64_t derivative_key = value_bucket(derivative, mantissa_bits);
        key = mix64(value_key ^ std::rotl(derivative_key, 29) ^
                    (depends_on_x ? 0x9e3779b97f4a7c15ULL : 0ULL));
    }
    // Keep the unconstrained hot path bit-for-bit identical. Non-zero states
    // distinguish expressions that have the same value but different usable
    // symbol inventories/order prefixes.
    if (constraint_state != 0) {
        key = mix64(key ^ std::rotl(mix64(constraint_state), 17));
    }
    return key;
}

static double abs_error(double value, double target) {
    return std::abs(value - target);
}

static double rel_error(double value, double target) {
    if (target == 0.0) return std::abs(value);
    return std::abs(value - target) / std::abs(target);
}

struct IntegerRelation {
    std::array<std::int64_t, 4> coefficients{};
    std::size_t size{};
    long double residual{};
};

// Bounded PSLQ for the small relation vectors used by the search stage. The
// matrices stay on the stack and coefficients are capped before conversion.
static std::optional<IntegerRelation> find_integer_relation(
    const std::array<long double, 4>& input,
    std::size_t size,
    long double tolerance,
    std::int64_t maximum_coefficient,
    unsigned maximum_steps) {
    constexpr std::size_t capacity = 4;
    if (size < 2 || size > capacity || tolerance <= 0.0L || maximum_coefficient <= 0) {
        return std::nullopt;
    }

    long double norm2 = 0.0L;
    for (std::size_t i = 0; i < size; ++i) {
        if (!std::isfinite(input[i]) || input[i] == 0.0L) return std::nullopt;
        norm2 += input[i] * input[i];
    }
    const long double norm = std::sqrt(norm2);
    if (!std::isfinite(norm) || norm == 0.0L) return std::nullopt;

    std::array<long double, capacity> y{};
    std::array<long double, capacity> s{};
    std::array<std::array<long double, capacity>, capacity> h{};
    std::array<std::array<std::int64_t, capacity>, capacity> b{};
    for (std::size_t i = 0; i < size; ++i) b[i][i] = 1;

    for (std::size_t k = 0; k < size; ++k) {
        long double tail = 0.0L;
        for (std::size_t j = k; j < size; ++j) tail += input[j] * input[j];
        s[k] = std::sqrt(tail) / norm;
        y[k] = input[k] / norm;
    }
    for (std::size_t i = 0; i < size; ++i) {
        if (i + 1 < size && s[i] != 0.0L) h[i][i] = s[i + 1] / s[i];
        for (std::size_t j = 0; j < i; ++j) {
            const long double denominator = s[j] * s[j + 1];
            if (denominator != 0.0L) h[i][j] = -y[i] * y[j] / denominator;
        }
    }

    const long double coefficient_guard = std::max<long double>(
        1.0e6L, static_cast<long double>(maximum_coefficient) * 4096.0L);
    const auto reduce = [&](std::size_t row, std::size_t first_column) -> bool {
        for (std::size_t offset = 0; offset <= first_column; ++offset) {
            const std::size_t column = first_column - offset;
            if (h[column][column] == 0.0L) continue;
            const long double quotient = h[row][column] / h[column][column];
            if (!std::isfinite(quotient) || std::abs(quotient) > coefficient_guard) return false;
            const std::int64_t multiplier = static_cast<std::int64_t>(std::llround(quotient));
            if (multiplier == 0) continue;
            y[column] += static_cast<long double>(multiplier) * y[row];
            for (std::size_t k = 0; k <= column; ++k) {
                h[row][k] -= static_cast<long double>(multiplier) * h[column][k];
            }
            for (std::size_t k = 0; k < size; ++k) {
                const long double updated = static_cast<long double>(b[k][column]) +
                    static_cast<long double>(multiplier) * b[k][row];
                if (!std::isfinite(updated) || std::abs(updated) > coefficient_guard) return false;
                b[k][column] = static_cast<std::int64_t>(std::llround(updated));
            }
        }
        return true;
    };

    const long double normalized_tolerance = tolerance / norm;
    const auto extract_relation = [&]() -> std::optional<IntegerRelation> {
        for (std::size_t column = 0; column < size; ++column) {
            if (std::abs(y[column]) > normalized_tolerance) continue;
            IntegerRelation relation;
            relation.size = size;
            std::int64_t divisor = 0;
            std::int64_t largest = 0;
            for (std::size_t row = 0; row < size; ++row) {
                relation.coefficients[row] = b[row][column];
                const std::int64_t magnitude = static_cast<std::int64_t>(std::abs(b[row][column]));
                largest = std::max(largest, magnitude);
                divisor = std::gcd(divisor, magnitude);
            }
            if (largest == 0) continue;
            if (divisor > 1) {
                for (std::size_t row = 0; row < size; ++row) relation.coefficients[row] /= divisor;
                largest /= divisor;
            }
            if (largest > maximum_coefficient) continue;
            for (std::size_t row = 0; row < size; ++row) {
                if (relation.coefficients[row] == 0) continue;
                if (relation.coefficients[row] < 0) {
                    for (std::size_t k = 0; k < size; ++k) relation.coefficients[k] *= -1;
                }
                break;
            }
            relation.residual = 0.0L;
            for (std::size_t row = 0; row < size; ++row) {
                relation.residual += static_cast<long double>(relation.coefficients[row]) * input[row];
            }
            if (std::abs(relation.residual) <= tolerance) return relation;
        }
        return std::nullopt;
    };

    for (std::size_t i = 1; i < size; ++i) {
        if (!reduce(i, i - 1)) return std::nullopt;
    }
    // Exact and near-exact relations can emerge during initial reduction.
    // Check before the first row swap, which may expose a tiny pivot on
    // extended-precision platforms and make the next reduction ill-conditioned.
    if (auto relation = extract_relation()) return relation;

    const long double gamma = std::sqrt(4.0L / 3.0L);
    for (unsigned step = 0; step < maximum_steps; ++step) {
        std::size_t pivot = 0;
        long double maximum = -1.0L;
        long double weight = gamma;
        for (std::size_t i = 0; i + 1 < size; ++i) {
            const long double score = weight * std::abs(h[i][i]);
            if (score > maximum) {
                maximum = score;
                pivot = i;
            }
            weight *= gamma;
        }

        std::swap(y[pivot], y[pivot + 1]);
        std::swap(h[pivot], h[pivot + 1]);
        for (std::size_t i = 0; i < size; ++i) {
            std::swap(b[i][pivot], b[i][pivot + 1]);
        }

        if (pivot + 2 < size) {
            const long double diagonal = std::hypot(h[pivot][pivot], h[pivot][pivot + 1]);
            if (diagonal == 0.0L || !std::isfinite(diagonal)) break;
            const long double cosine = h[pivot][pivot] / diagonal;
            const long double sine = h[pivot][pivot + 1] / diagonal;
            for (std::size_t i = pivot; i < size; ++i) {
                const long double first = h[i][pivot];
                const long double second = h[i][pivot + 1];
                h[i][pivot] = cosine * first + sine * second;
                h[i][pivot + 1] = -sine * first + cosine * second;
            }
        }

        for (std::size_t i = pivot + 1; i < size; ++i) {
            if (!reduce(i, std::min(i - 1, pivot + 1))) return std::nullopt;
        }

        if (auto relation = extract_relation()) return relation;
    }
    return std::nullopt;
}

static std::uint64_t candidate_shape_signature(const Candidate& candidate) {
    std::uint64_t signature = static_cast<std::uint64_t>(candidate.tag);
    signature = (signature << 8U) | candidate.op;
    signature = (signature << 1U) | static_cast<std::uint64_t>(candidate.depends_on_x);
    signature ^= std::rotl(static_cast<std::uint64_t>(candidate.depth), 17);
    return mix64(signature);
}

class CandidateCollector {
public:
    CandidateCollector(double target,
                       unsigned value_bits,
                       std::size_t soft_cap,
                       bool derivative_sensitive = false,
                       std::size_t reserve_hint = 0,
                       unsigned pareto_slots = 1,
                       std::size_t extra_cap = 0,
                       bool parallel_sort = false)
        : target_(target),
          value_bits_(value_bits),
          soft_cap_(std::max<std::size_t>(1, soft_cap)),
          derivative_sensitive_(derivative_sensitive),
          pareto_slots_(std::max(1U, pareto_slots)),
          extra_cap_(pareto_slots_ <= 1
                         ? 0
                         : std::max<std::size_t>(1, extra_cap == 0 ? soft_cap_ / 4 : extra_cap)),
          parallel_sort_(parallel_sort) {
        const std::size_t maximum_size = soft_cap_ * 2 + 1;
        table_.reserve(reserve_hint == 0 ? maximum_size : std::min(maximum_size, reserve_hint));
    }

    void consider(const Candidate& candidate) {
        const auto key = state_bucket(candidate.value, candidate.derivative, candidate.depends_on_x,
                                      candidate.constraint_state, value_bits_, derivative_sensitive_);
        consider_with_key(candidate, key);
    }

    void consider_with_key(const Candidate& candidate, std::uint64_t key) {
        const auto it = table_.find(key);
        if (it == table_.end()) {
            table_.emplace(key, candidate);
        } else {
            if (better(candidate, it->second)) {
                const Candidate displaced = it->second;
                it->second = candidate;
                consider_extra_with_key(displaced, key);
            } else {
                consider_extra_with_key(candidate, key);
            }
        }
        if (table_.size() > soft_cap_ * 2) prune_to(soft_cap_);
    }

    void consider_extra_only(const Candidate& candidate) {
        if (pareto_slots_ <= 1) return;
        const auto key = state_bucket(candidate.value, candidate.derivative, candidate.depends_on_x,
                                      candidate.constraint_state, value_bits_, derivative_sensitive_);
        consider_extra_with_key(candidate, key);
    }

    std::vector<Candidate> take(std::size_t cap) {
        prune_to(cap);
        std::vector<Candidate> out;
        out.reserve(table_.size());
        for (auto& [_, candidate] : table_) out.push_back(candidate);
        sort_range(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
            if (a.value != b.value) return a.value < b.value;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            if (a.depth != b.depth) return a.depth < b.depth;
            return a.hash < b.hash;
        });
        return out;
    }

    std::vector<Candidate> take_extras(std::size_t cap) {
        if (pareto_slots_ <= 1 || cap == 0) return {};
        prune_extras_to(cap);

        FastSet<std::uint64_t> primary_hashes;
        primary_hashes.reserve(table_.size() * 2 + 1);
        for (const auto& [_, candidate] : table_) primary_hashes.emplace(candidate.hash);

        std::vector<Candidate> out;
        out.reserve(extra_table_.size());
        for (const auto& [_, candidate] : extra_table_) {
            if (primary_hashes.find(candidate.hash) == primary_hashes.end()) out.push_back(candidate);
        }
        sort_range(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
            if (a.value != b.value) return a.value < b.value;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            if (a.depth != b.depth) return a.depth < b.depth;
            return a.hash < b.hash;
        });
        if (out.size() > cap) out.resize(cap);
        return out;
    }

private:
    template <class Iterator, class Compare>
    void sort_range(Iterator begin, Iterator end, Compare compare) const {
#if defined(_WIN32) && defined(_MSC_VER)
        if (parallel_sort_ && static_cast<std::size_t>(end - begin) >= 8192) {
            concurrency::parallel_sort(begin, end, compare);
            return;
        }
#endif
        std::sort(begin, end, compare);
    }

    bool better(const Candidate& a, const Candidate& b) const {
        const double ea = abs_error(a.value, target_);
        const double eb = abs_error(b.value, target_);
        if (ea != eb) return ea < eb;
        if (a.nodes != b.nodes) return a.nodes < b.nodes;
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.hash < b.hash;
    }

    bool more_elegant(const Candidate& a, const Candidate& b) const {
        if (a.nodes != b.nodes) return a.nodes < b.nodes;
        if (a.depth != b.depth) return a.depth < b.depth;
        const double ea = abs_error(a.value, target_);
        const double eb = abs_error(b.value, target_);
        if (ea != eb) return ea < eb;
        return a.hash < b.hash;
    }

    std::uint64_t extra_key(const Candidate& candidate, std::uint64_t state_key) const {
        const std::uint64_t slot = candidate_shape_signature(candidate) % (pareto_slots_ - 1U);
        return mix64(state_key ^ (0x9e3779b97f4a7c15ULL * (slot + 1U)));
    }

    void consider_extra_with_key(const Candidate& candidate, std::uint64_t state_key) {
        if (pareto_slots_ <= 1) return;
        if (!extra_reserved_) {
            extra_table_.reserve(extra_cap_ * 2 + 1);
            extra_reserved_ = true;
        }
        const std::uint64_t key = extra_key(candidate, state_key);
        const auto it = extra_table_.find(key);
        if (it == extra_table_.end()) {
            extra_table_.emplace(key, candidate);
        } else if (more_elegant(candidate, it->second)) {
            it->second = candidate;
        }
        if (extra_table_.size() > extra_cap_ * 2) prune_extras_to(extra_cap_);
    }

    void prune_extras_to(std::size_t cap) {
        cap = std::max<std::size_t>(1, cap);
        if (extra_table_.size() <= cap) return;

        std::vector<Candidate> all;
        all.reserve(extra_table_.size());
        for (const auto& [_, candidate] : extra_table_) all.push_back(candidate);
        sort_range(all.begin(), all.end(), [&](const Candidate& a, const Candidate& b) {
            const double ea = abs_error(a.value, target_);
            const double eb = abs_error(b.value, target_);
            const double scale = std::max(1.0, std::abs(target_));
            const double precision_floor = std::ldexp(scale, -static_cast<int>(value_bits_));
            const bool a_equivalent = ea <= precision_floor;
            const bool b_equivalent = eb <= precision_floor;
            if (a_equivalent != b_equivalent) return a_equivalent;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            if (a.depth != b.depth) return a.depth < b.depth;
            if (ea != eb) return ea < eb;
            return a.hash < b.hash;
        });

        std::vector<Candidate> keep;
        keep.reserve(cap);
        const std::size_t elegant_count = std::min(cap, std::max<std::size_t>(1, cap / 2));
        keep.insert(keep.end(), all.begin(), all.begin() + static_cast<std::ptrdiff_t>(elegant_count));

        sort_range(all.begin() + static_cast<std::ptrdiff_t>(elegant_count), all.end(),
                   [](const Candidate& a, const Candidate& b) {
                      if (a.value != b.value) return a.value < b.value;
                      return a.hash < b.hash;
                  });
        const std::size_t rest_size = all.size() - elegant_count;
        const std::size_t spread_count = std::min(cap - keep.size(), rest_size);
        for (std::size_t i = 0; i < spread_count; ++i) {
            const std::size_t position = spread_count == 1
                ? rest_size / 2
                : (i * (rest_size - 1)) / (spread_count - 1);
            keep.push_back(all[elegant_count + position]);
        }

        extra_table_.clear();
        extra_table_.reserve(cap * 2 + 1);
        for (const Candidate& candidate : keep) {
            const auto state_key = state_bucket(candidate.value, candidate.derivative,
                                                candidate.depends_on_x, candidate.constraint_state,
                                                value_bits_, derivative_sensitive_);
            const auto key = extra_key(candidate, state_key);
            const auto it = extra_table_.find(key);
            if (it == extra_table_.end()) extra_table_.emplace(key, candidate);
            else if (more_elegant(candidate, it->second)) it->second = candidate;
        }
    }

    void prune_to(std::size_t cap) {
        cap = std::max<std::size_t>(1, cap);
        if (table_.size() <= cap) return;

        // Equation mode needs values spread over the whole numeric axis.  The
        // normal collector intentionally keeps mostly candidates close to the
        // requested constant target; applying that policy to f(x)=g(x) can
        // discard both sides of an otherwise useful equality before matching.
        if (derivative_sensitive_) {
            prune_equation_to(cap);
            return;
        }

        struct Entry {
            std::uint64_t key{};
            Candidate candidate;
        };
        std::vector<Entry> all;
        all.reserve(table_.size());
        for (const auto& [key, candidate] : table_) all.push_back({key, candidate});

        std::vector<std::size_t> order(all.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        const auto near_better = [&](std::size_t i, std::size_t j) {
            const Candidate& a = all[i].candidate;
            const Candidate& b = all[j].candidate;
            const double ei = abs_error(a.value, target_);
            const double ej = abs_error(b.value, target_);
            if (ei != ej) return ei < ej;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            return a.hash < b.hash;
        };

        const std::size_t near_count = std::min(cap, std::max<std::size_t>(1, (cap * 3) / 5));
        // We only need the best near_count entries.  Selecting that prefix in
        // linear time and sorting the prefix produces the exact same ordered
        // survivors as sorting the whole (normally 2*cap) table.
        if (near_count < order.size()) {
            std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(near_count),
                             order.end(), near_better);
        }
        sort_range(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(near_count), near_better);

        std::vector<unsigned char> selected(all.size(), 0);
        std::vector<std::size_t> keep_indices;
        keep_indices.reserve(cap);
        for (std::size_t k = 0; k < near_count; ++k) {
            selected[order[k]] = 1;
            keep_indices.push_back(order[k]);
        }

        std::vector<std::size_t> rest;
        rest.reserve(all.size() - near_count);
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (!selected[i]) rest.push_back(i);
        }
        sort_range(rest.begin(), rest.end(), [&](std::size_t i, std::size_t j) {
            const Candidate& a = all[i].candidate;
            const Candidate& b = all[j].candidate;
            if (a.value != b.value) return a.value < b.value;
            return a.hash < b.hash;
        });

        const std::size_t spread_count = std::min(cap - keep_indices.size(), rest.size());
        if (spread_count == 1) {
            const std::size_t selected_index = rest[rest.size() / 2];
            selected[selected_index] = 1;
            keep_indices.push_back(selected_index);
        } else if (spread_count > 1) {
            for (std::size_t k = 0; k < spread_count; ++k) {
                const std::size_t pos = (k * (rest.size() - 1)) / (spread_count - 1);
                const std::size_t selected_index = rest[pos];
                selected[selected_index] = 1;
                keep_indices.push_back(selected_index);
            }
        }

        if (pareto_slots_ > 1) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (!selected[i]) consider_extra_with_key(all[i].candidate, all[i].key);
            }
        }

        table_.clear();
        table_.reserve(cap * 2 + 1);
        for (const std::size_t index : keep_indices) {
            // Entries came from table_, so their keys are already unique.
            table_.emplace(all[index].key, all[index].candidate);
        }
    }

    void prune_equation_to(std::size_t cap) {
        std::vector<Candidate> all;
        all.reserve(table_.size());
        for (const auto& [_, candidate] : table_) all.push_back(candidate);
        if (all.size() <= cap) return;

        const auto by_value = [](const Candidate& a, const Candidate& b) {
            if (a.value != b.value) return a.value < b.value;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            if (a.depth != b.depth) return a.depth < b.depth;
            return a.hash < b.hash;
        };
        sort_range(all.begin(), all.end(), by_value);

        const std::size_t near_count = std::min(cap, std::max<std::size_t>(1, cap / 4));
        std::vector<std::size_t> near_order(all.size());
        for (std::size_t i = 0; i < near_order.size(); ++i) near_order[i] = i;
        sort_range(near_order.begin(), near_order.end(), [&](std::size_t i, std::size_t j) {
            const double a = abs_error(all[i].value, target_);
            const double b = abs_error(all[j].value, target_);
            if (a != b) return a < b;
            return by_value(all[i], all[j]);
        });

        std::vector<unsigned char> selected(all.size(), 0);
        std::vector<Candidate> keep;
        keep.reserve(cap);
        for (std::size_t i = 0; i < near_count; ++i) {
            selected[near_order[i]] = 1;
            keep.push_back(all[near_order[i]]);
        }

        std::vector<std::size_t> rest;
        rest.reserve(all.size() - near_count);
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (!selected[i]) rest.push_back(i);
        }
        const std::size_t spread_count = std::min(cap - keep.size(), rest.size());
        if (spread_count == 1) {
            keep.push_back(all[rest[rest.size() / 2]]);
        } else if (spread_count > 1) {
            for (std::size_t i = 0; i < spread_count; ++i) {
                const std::size_t position = (i * (rest.size() - 1)) / (spread_count - 1);
                keep.push_back(all[rest[position]]);
            }
        }

        table_.clear();
        table_.reserve(cap * 2 + 1);
        for (const Candidate& candidate : keep) {
            const auto key = state_bucket(candidate.value, candidate.derivative,
                                           candidate.depends_on_x, candidate.constraint_state,
                                           value_bits_, derivative_sensitive_);
            const auto it = table_.find(key);
            if (it == table_.end()) table_.emplace(key, candidate);
            else if (better(candidate, it->second)) it->second = candidate;
        }
    }

    double target_{};
    unsigned value_bits_{};
    std::size_t soft_cap_{};
    bool derivative_sensitive_{};
    unsigned pareto_slots_{1};
    std::size_t extra_cap_{};
    bool extra_reserved_{};
    bool parallel_sort_{};
    FastMap<std::uint64_t, Candidate> table_;
    FastMap<std::uint64_t, Candidate> extra_table_;
};

static std::pair<std::string, std::optional<unsigned>> split_optional_cost(std::string token) {
    token = trim(std::move(token));
    const auto colon = token.find_last_of(':');
    if (colon == std::string::npos || colon + 1 >= token.size()) return {token, std::nullopt};
    const std::string suffix = token.substr(colon + 1);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(suffix.c_str(), &end, 10);
    if (end == suffix.c_str() || *end != '\0') return {token, std::nullopt};
    return {token.substr(0, colon), static_cast<unsigned>(parsed)};
}

static unsigned default_unary_cost(UnaryKind kind) {
    if (const auto custom = custom_unary_index(kind)) {
        return extension_registry().unary_operations()[*custom].default_cost;
    }
    switch (kind) {
        case UnaryKind::Neg:
        case UnaryKind::Inv:
        case UnaryKind::Sqrt:
        case UnaryKind::Cbrt:
        case UnaryKind::Sqr:
        case UnaryKind::Cube:
        case UnaryKind::Abs:
            return 1;
        default:
            return 2;
    }
}

static unsigned default_binary_cost(BinaryKind kind) {
    if (const auto custom = custom_binary_index(kind)) {
        return extension_registry().binary_operations()[*custom].default_cost;
    }
    return kind == BinaryKind::Pow ? 2U : 1U;
}

static std::string_view unary_name(UnaryKind kind) {
    switch (kind) {
        case UnaryKind::Neg: return "neg";
        case UnaryKind::Inv: return "inv";
        case UnaryKind::Sqrt: return "sqrt";
        case UnaryKind::Cbrt: return "cbrt";
        case UnaryKind::Sqr: return "sqr";
        case UnaryKind::Cube: return "cube";
        case UnaryKind::Ln: return "ln";
        case UnaryKind::Log10: return "log10";
        case UnaryKind::Exp: return "exp";
        case UnaryKind::Sin: return "sin";
        case UnaryKind::Cos: return "cos";
        case UnaryKind::Tan: return "tan";
        case UnaryKind::Asin: return "asin";
        case UnaryKind::Acos: return "acos";
        case UnaryKind::Atan: return "atan";
        case UnaryKind::Sinh: return "sinh";
        case UnaryKind::Cosh: return "cosh";
        case UnaryKind::Tanh: return "tanh";
        case UnaryKind::Asinh: return "asinh";
        case UnaryKind::Acosh: return "acosh";
        case UnaryKind::Atanh: return "atanh";
        case UnaryKind::Gamma: return "gamma";
        case UnaryKind::Abs: return "abs";
        case UnaryKind::Fact: return "fact";
        default:
            if (const auto custom = custom_unary_index(kind)) {
                return extension_registry().unary_operations()[*custom].name;
            }
            return "unknown-unary";
    }
}

static std::string_view binary_name(BinaryKind kind) {
    switch (kind) {
        case BinaryKind::Add: return "+";
        case BinaryKind::Sub: return "-";
        case BinaryKind::Mul: return "*";
        case BinaryKind::Div: return "/";
        case BinaryKind::Pow: return "^";
        default:
            if (const auto custom = custom_binary_index(kind)) {
                return extension_registry().binary_operations()[*custom].name;
            }
            return "unknown-binary";
    }
}

static std::optional<UnaryKind> parse_unary_name(std::string name) {
    name = trim(std::move(name));
    if (name == "neg" || name == "~") return UnaryKind::Neg;
    if (name == "inv" || name == "recip") return UnaryKind::Inv;
    if (name == "sqrt") return UnaryKind::Sqrt;
    if (name == "cbrt") return UnaryKind::Cbrt;
    if (name == "sqr" || name == "square") return UnaryKind::Sqr;
    if (name == "cube") return UnaryKind::Cube;
    if (name == "ln" || name == "log") return UnaryKind::Ln;
    if (name == "log10") return UnaryKind::Log10;
    if (name == "exp") return UnaryKind::Exp;
    if (name == "sin") return UnaryKind::Sin;
    if (name == "cos") return UnaryKind::Cos;
    if (name == "tan") return UnaryKind::Tan;
    if (name == "asin") return UnaryKind::Asin;
    if (name == "acos") return UnaryKind::Acos;
    if (name == "atan") return UnaryKind::Atan;
    if (name == "sinh") return UnaryKind::Sinh;
    if (name == "cosh") return UnaryKind::Cosh;
    if (name == "tanh") return UnaryKind::Tanh;
    if (name == "asinh" || name == "arcsinh") return UnaryKind::Asinh;
    if (name == "acosh" || name == "arccosh") return UnaryKind::Acosh;
    if (name == "atanh" || name == "arctanh") return UnaryKind::Atanh;
    if (name == "gamma" || name == "Gamma" || name == "Γ") return UnaryKind::Gamma;
    if (name == "abs") return UnaryKind::Abs;
    if (name == "fact" || name == "!") return UnaryKind::Fact;
    const auto& operations = extension_registry().unary_operations();
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (operation.name == name ||
            std::find(operation.aliases.begin(), operation.aliases.end(), name) != operation.aliases.end()) {
            return static_cast<UnaryKind>(kCustomOperationBase + index);
        }
    }
    return std::nullopt;
}

static std::optional<BinaryKind> parse_binary_name(std::string name) {
    name = trim(std::move(name));
    if (name == "+" || name == "add") return BinaryKind::Add;
    if (name == "-" || name == "sub") return BinaryKind::Sub;
    if (name == "*" || name == "mul" || name == "x") return BinaryKind::Mul;
    if (name == "/" || name == "div") return BinaryKind::Div;
    if (name == "^" || name == "pow") return BinaryKind::Pow;
    const auto& operations = extension_registry().binary_operations();
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (operation.name == name ||
            std::find(operation.aliases.begin(), operation.aliases.end(), name) != operation.aliases.end()) {
            return static_cast<BinaryKind>(kCustomOperationBase + index);
        }
    }
    return std::nullopt;
}

static std::pair<std::vector<UnarySpec>, std::vector<BinarySpec>> parse_ops(const std::string& text) {
    std::vector<UnarySpec> unary;
    std::vector<BinarySpec> binary;
    if (trim(text).empty() || trim(text) == "none") return {unary, binary};

    for (const auto& raw : split_csv(text)) {
        if (raw.empty()) continue;
        auto [name, explicit_cost] = split_optional_cost(raw);
        if (const auto uk = parse_unary_name(name)) {
            const unsigned cost = explicit_cost.value_or(default_unary_cost(*uk));
            if (cost == 0 || cost > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("运算符成本必须在 1..65535 之间: " + raw);
            }
            auto it = std::find_if(unary.begin(), unary.end(), [&](const UnarySpec& s) { return s.kind == *uk; });
            if (it == unary.end()) unary.push_back({*uk, static_cast<std::uint16_t>(cost)});
            else it->cost = static_cast<std::uint16_t>(cost);
            continue;
        }
        if (const auto bk = parse_binary_name(name)) {
            const unsigned cost = explicit_cost.value_or(default_binary_cost(*bk));
            if (cost == 0 || cost > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("运算符成本必须在 1..65535 之间: " + raw);
            }
            auto it = std::find_if(binary.begin(), binary.end(), [&](const BinarySpec& s) { return s.kind == *bk; });
            if (it == binary.end()) binary.push_back({*bk, static_cast<std::uint16_t>(cost)});
            else it->cost = static_cast<std::uint16_t>(cost);
            continue;
        }
        throw std::runtime_error("未知运算符: " + raw);
    }
    return {unary, binary};
}

static const std::map<std::string, double>& builtin_constants() {
    static const std::map<std::string, double> values = {
        {"catalan", 0.91596559417721901505},
        {"e", std::numbers::e_v<double>},
        {"gamma", 0.57721566490153286061},
        {"ln2", std::numbers::ln2_v<double>},
        {"phi", (1.0 + std::sqrt(5.0)) / 2.0},
        {"pi", std::numbers::pi_v<double>},
        {"sqrt2", std::numbers::sqrt2_v<double>},
        {"tau", 2.0 * std::numbers::pi_v<double>},
    };
    return values;
}

static void generate_digit_literals(const Config& cfg, std::vector<AtomSpec>& atoms) {
    std::string digits;
    std::set<char> seen;
    for (const char c : cfg.digits) {
        if (c < '0' || c > '9') throw std::runtime_error("--digits 只能包含 0..9");
        if (seen.insert(c).second) digits.push_back(c);
    }
    if (digits.empty() || cfg.max_literal_len == 0) return;

    std::uint64_t estimate = 0;
    std::uint64_t power = 1;
    for (unsigned len = 1; len <= cfg.max_literal_len; ++len) {
        power = saturating_mul(power, digits.size());
        estimate = saturating_add(estimate, power);
    }
    const std::uint64_t numeric_bound = cfg.max_integer == std::numeric_limits<std::uint64_t>::max()
                                            ? cfg.max_integer
                                            : cfg.max_integer + 1;
    if (std::min(estimate, numeric_bound) > cfg.max_atoms * 2ULL) {
        throw std::runtime_error("数字字面量组合过多；请减小 --max-literal-len、减少 --digits，或增大 --max-atoms");
    }

    std::string current;
    std::function<void(unsigned, unsigned, std::uint64_t)> rec =
        [&](unsigned len, unsigned pos, std::uint64_t value) {
        if (pos == len) {
            const unsigned cost = len * cfg.digit_cost;
            if (cost <= cfg.max_cost) {
                atoms.push_back({current, static_cast<double>(value), static_cast<std::uint16_t>(cost)});
            }
            return;
        }
        for (const char c : digits) {
            if (pos == 0 && len > 1 && c == '0') continue;
            const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
            if (digit > cfg.max_integer || value > (cfg.max_integer - digit) / 10) continue;
            current.push_back(c);
            rec(len, pos + 1, value * 10 + digit);
            current.pop_back();
            if (atoms.size() > cfg.max_atoms) {
                throw std::runtime_error("原子数量超过 --max-atoms");
            }
        }
    };

    for (unsigned len = 1; len <= cfg.max_literal_len; ++len) rec(len, 0, 0);
}

static std::vector<AtomSpec> build_atoms(const Config& cfg) {
    std::vector<AtomSpec> atoms;
    atoms.reserve(1024);
    generate_digit_literals(cfg, atoms);

    const std::string constants_text = trim(cfg.constants);
    if (!constants_text.empty() && constants_text != "none") {
        for (const auto& raw : split_csv(constants_text)) {
            if (raw.empty()) continue;
            auto [name, explicit_cost] = split_optional_cost(raw);
            const auto it = builtin_constants().find(name);
            if (it == builtin_constants().end()) throw std::runtime_error("未知内置常数: " + name);
            const unsigned cost = explicit_cost.value_or(1U);
            if (cost == 0 || cost > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("内置常数成本必须在 1..65535 之间: " + raw);
            }
            if (cost <= cfg.max_cost) atoms.push_back({name, it->second, static_cast<std::uint16_t>(cost)});
        }
    }

    for (const auto& spec : cfg.custom_constants) {
        const auto eq = spec.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size()) {
            throw std::runtime_error("自定义常数格式应为 name=value[:cost]: " + spec);
        }
        const std::string name = trim(spec.substr(0, eq));
        if (cfg.equations && name == "x") {
            throw std::runtime_error("方程模式下自定义常数不能命名为 x");
        }
        auto [value_text, explicit_cost] = split_optional_cost(spec.substr(eq + 1));
        char* end = nullptr;
        const double value = std::strtod(value_text.c_str(), &end);
        if (end == value_text.c_str() || *end != '\0' || !std::isfinite(value)) {
            throw std::runtime_error("无效的自定义常数数值: " + spec);
        }
        const unsigned cost = explicit_cost.value_or(1U);
        if (cost == 0 || cost > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("自定义常数成本必须在 1..65535 之间: " + spec);
        }
        if (cost <= cfg.max_cost) atoms.push_back({name, value, static_cast<std::uint16_t>(cost)});
    }

    if (cfg.equations && cfg.max_cost >= 1) {
        atoms.push_back({"x", cfg.target, 1, true});
    }

    if (atoms.size() > cfg.max_atoms) throw std::runtime_error("原子数量超过 --max-atoms");
    return atoms;
}

static std::uint32_t low_u32_mask(unsigned bits) {
    if (bits == 0) return 0;
    if (bits >= 32) return std::numeric_limits<std::uint32_t>::max();
    return (std::uint32_t{1} << bits) - 1U;
}

static unsigned bits_for_u32(std::uint32_t maximum) {
    return maximum == 0 ? 0U : 32U - std::countl_zero(maximum);
}

struct ParsedCountBound {
    bool infinite{};
    std::uint64_t value{};
};

static ParsedCountBound parse_count_bound(std::string text, std::string_view position) {
    text = trim(std::move(text));
    std::string lowered = text;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lowered == "inf" || lowered == "+inf" || lowered == "infinity" ||
        lowered == "+infinity" || lowered == "*") {
        return {true, 0};
    }
    if (text.empty() || text.front() == '-') {
        throw std::runtime_error("--symbol-count " + std::string(position) + "不是非负整数或 inf: " + text);
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        throw std::runtime_error("--symbol-count " + std::string(position) + "不是非负整数或 inf: " + text);
    }
    return {false, static_cast<std::uint64_t>(value)};
}

struct ParsedCountSpec {
    std::string symbol;
    std::uint64_t minimum{};
    std::optional<std::uint64_t> maximum;
};

static ParsedCountSpec parse_symbol_count_spec(const std::string& text) {
    const auto equal = text.find('=');
    if (equal == std::string::npos || equal == 0 || text.find('=', equal + 1) != std::string::npos) {
        throw std::runtime_error("--symbol-count 格式应为 NAME=N 或 NAME=MIN:MAX: " + text);
    }
    ParsedCountSpec parsed;
    parsed.symbol = trim(text.substr(0, equal));
    if (parsed.symbol.empty()) {
        throw std::runtime_error("--symbol-count 的符号名不能为空: " + text);
    }

    const std::string range = trim(text.substr(equal + 1));
    const auto colon = range.find(':');
    if (colon == std::string::npos) {
        const ParsedCountBound exact = parse_count_bound(range, "次数");
        if (exact.infinite) {
            throw std::runtime_error("--symbol-count 的最小次数不能为 inf；有限成本表达式无法出现无限次: " + text);
        }
        parsed.minimum = exact.value;
        parsed.maximum = exact.value;
    } else {
        if (range.find(':', colon + 1) != std::string::npos) {
            throw std::runtime_error("--symbol-count 范围只能包含一个冒号: " + text);
        }
        const ParsedCountBound lower = parse_count_bound(range.substr(0, colon), "下界");
        const ParsedCountBound upper = parse_count_bound(range.substr(colon + 1), "上界");
        if (lower.infinite) {
            throw std::runtime_error("--symbol-count 的最小次数不能为 inf；有限成本表达式无法出现无限次: " + text);
        }
        parsed.minimum = lower.value;
        if (!upper.infinite) parsed.maximum = upper.value;
    }
    if (parsed.maximum && parsed.minimum > *parsed.maximum) {
        throw std::runtime_error("--symbol-count 下界不能大于上界: " + text);
    }
    return parsed;
}

static std::uint32_t count_field(std::uint32_t state, const SymbolCountRule& rule) {
    if (rule.bits == 0) return 0;
    return (state >> rule.shift) & low_u32_mask(rule.bits);
}

std::optional<std::uint32_t> SymbolConstraintPlan::atom_state(std::string_view symbol) const {
    if (!active) return 0U;
    std::uint32_t state = 0;
    for (const SymbolCountRule& rule : count_rules) {
        if (rule.symbol != symbol) continue;
        if (!rule.unlimited && rule.maximum == 0) return std::nullopt;
        const std::uint32_t stored = rule.unlimited ? std::min<std::uint32_t>(1U, rule.minimum) : 1U;
        if (rule.bits != 0) state |= stored << rule.shift;
        break;
    }

    if (!required_order.empty()) {
        std::uint32_t starts = 0;
        for (std::size_t i = 0; i < required_order.size(); ++i) {
            if (required_order[i] == symbol) starts |= std::uint32_t{1} << i;
        }
        if (starts != 0) {
            state |= starts << order_mask_shift;
            state |= std::uint32_t{1} << order_length_shift;
        }
    }
    return state;
}

std::optional<std::uint32_t> SymbolConstraintPlan::combine(std::uint32_t left,
                                                           std::uint32_t right) const {
    if (!active) return 0U;
    if (((left & right) & unit_once_mask) != 0) return std::nullopt;
    std::uint32_t state = (left | right) & unit_once_mask;
    for (const SymbolCountRule& rule : count_rules) {
        if (!rule.unlimited && rule.maximum == 1) continue;
        const std::uint32_t sum = count_field(left, rule) + count_field(right, rule);
        if (!rule.unlimited && sum > rule.maximum) return std::nullopt;
        const std::uint32_t stored = rule.unlimited ? std::min(sum, rule.minimum) : sum;
        if (rule.bits != 0) state |= stored << rule.shift;
    }

    if (!required_order.empty()) {
        const std::uint32_t left_mask = (left >> order_mask_shift) & order_symbol_mask;
        const std::uint32_t right_mask = (right >> order_mask_shift) & order_symbol_mask;
        const unsigned left_length = (left >> order_length_shift) & low_u32_mask(order_length_bits);
        const unsigned right_length = (right >> order_length_shift) & low_u32_mask(order_length_bits);
        const unsigned length = left_length + right_length;
        if (length > required_order.size()) return std::nullopt;

        std::uint32_t starts = 0;
        if (left_length == 0) {
            starts = right_mask;
        } else if (right_length == 0) {
            starts = left_mask;
        } else {
            starts = left_mask & (right_mask >> left_length);
        }
        if (length != 0 && starts == 0) return std::nullopt;
        state |= starts << order_mask_shift;
        state |= static_cast<std::uint32_t>(length) << order_length_shift;
    }
    return state;
}

bool SymbolConstraintPlan::satisfied(std::uint32_t state) const {
    if (!active) return true;
    for (const SymbolCountRule& rule : count_rules) {
        if (count_field(state, rule) < rule.minimum) return false;
    }
    if (!required_order.empty()) {
        const unsigned length = (state >> order_length_shift) & low_u32_mask(order_length_bits);
        const std::uint32_t starts = (state >> order_mask_shift) & order_symbol_mask;
        if (length != required_order.size() || (starts & 1U) == 0) return false;
    }
    return true;
}

bool SymbolConstraintPlan::can_finish(std::uint32_t state, unsigned current_cost) const {
    if (!active) return current_cost <= max_cost;
    if (current_cost > max_cost) return false;
    if (maximum_missing_atom_cost <= static_cast<std::uint64_t>(max_cost - current_cost)) return true;

    std::uint64_t count_cost = 0;
    for (const SymbolCountRule& rule : count_rules) {
        const std::uint32_t count = count_field(state, rule);
        if (count < rule.minimum) {
            count_cost = saturating_add(
                count_cost, saturating_mul(static_cast<std::uint64_t>(rule.minimum - count), rule.atom_cost));
        }
    }

    std::uint64_t order_cost = 0;
    if (!required_order.empty()) {
        const unsigned length = (state >> order_length_shift) & low_u32_mask(order_length_bits);
        const std::uint32_t starts = (state >> order_mask_shift) & order_symbol_mask;
        if (length == 0) {
            for (const std::uint16_t cost : order_atom_cost) order_cost += cost;
        } else {
            if (starts == 0 || length > required_order.size()) return false;
            order_cost = std::numeric_limits<std::uint64_t>::max();
            for (unsigned start = 0; start + length <= required_order.size(); ++start) {
                if ((starts & (std::uint32_t{1} << start)) == 0) continue;
                std::uint64_t missing = 0;
                for (unsigned i = 0; i < start; ++i) missing += order_atom_cost[i];
                for (unsigned i = start + length; i < order_atom_cost.size(); ++i) {
                    missing += order_atom_cost[i];
                }
                order_cost = std::min(order_cost, missing);
            }
            if (order_cost == std::numeric_limits<std::uint64_t>::max()) return false;
        }
    }
    const std::uint64_t missing_cost = std::max(count_cost, order_cost);
    return missing_cost <= static_cast<std::uint64_t>(max_cost - current_cost);
}

static SymbolConstraintPlan compile_symbol_constraints(const Config& cfg,
                                                       const std::vector<AtomSpec>& atoms) {
    SymbolConstraintPlan plan;
    plan.max_cost = cfg.max_cost;
    if (cfg.symbol_count_specs.empty() && cfg.required_symbol_order.empty()) return plan;

    std::map<std::string, std::uint16_t> atom_costs;
    for (const AtomSpec& atom : atoms) {
        const auto [it, inserted] = atom_costs.emplace(atom.text, atom.cost);
        if (!inserted) it->second = std::min(it->second, atom.cost);
    }

    struct MergedRange {
        std::uint64_t minimum{};
        std::optional<std::uint64_t> maximum;
    };
    std::map<std::string, MergedRange> merged;
    for (const std::string& text : cfg.symbol_count_specs) {
        const ParsedCountSpec parsed = parse_symbol_count_spec(text);
        auto [it, inserted] = merged.emplace(parsed.symbol, MergedRange{parsed.minimum, parsed.maximum});
        if (!inserted) {
            it->second.minimum = std::max(it->second.minimum, parsed.minimum);
            if (it->second.maximum && parsed.maximum) {
                it->second.maximum = std::min(*it->second.maximum, *parsed.maximum);
            } else if (parsed.maximum) {
                it->second.maximum = parsed.maximum;
            }
        }
        if (it->second.maximum && it->second.minimum > *it->second.maximum) {
            throw std::runtime_error("--symbol-count 对符号 '" + parsed.symbol + "' 的重复范围互相冲突");
        }
    }

    unsigned used_bits = 0;
    for (const auto& [symbol, range] : merged) {
        const auto atom = atom_costs.find(symbol);
        if (atom == atom_costs.end()) {
            throw std::runtime_error("--symbol-count 符号 '" + symbol +
                                     "' 不在当前原子集合中（或其成本超过 --max-cost）");
        }
        const std::uint64_t feasible = cfg.max_cost / atom->second;
        if (range.minimum > feasible) {
            throw std::runtime_error("--symbol-count 要求符号 '" + symbol + "' 至少出现 " +
                                     std::to_string(range.minimum) + " 次，但 --max-cost 下最多只能出现 " +
                                     std::to_string(feasible) + " 次");
        }
        const bool unlimited = !range.maximum || *range.maximum >= feasible;
        const std::uint64_t effective_max = unlimited ? range.minimum : *range.maximum;
        if (effective_max > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("--symbol-count 次数超出内部状态范围: " + symbol);
        }
        if (unlimited && range.minimum == 0) continue;  // 0:inf imposes no restriction.

        SymbolCountRule rule;
        rule.symbol = symbol;
        rule.minimum = static_cast<std::uint32_t>(range.minimum);
        rule.maximum = static_cast<std::uint32_t>(effective_max);
        rule.atom_cost = atom->second;
        rule.shift = static_cast<std::uint8_t>(used_bits);
        rule.bits = static_cast<std::uint8_t>(bits_for_u32(rule.maximum));
        rule.unlimited = unlimited;
        used_bits += rule.bits;
        if (used_bits > 32) {
            throw std::runtime_error("符号次数约束状态超过 32 位；请减少受约束符号或缩小最大次数");
        }
        if (!rule.unlimited && rule.maximum == 1 && rule.bits == 1) {
            plan.unit_once_mask |= std::uint32_t{1} << rule.shift;
        }
        plan.count_rules.push_back(std::move(rule));
    }

    plan.required_order = cfg.required_symbol_order;
    if (!plan.required_order.empty()) {
        if (plan.required_order.size() > 31) {
            throw std::runtime_error("--symbol-order 当前最多支持 31 个受控叶子符号");
        }
        std::map<std::string, std::uint32_t> order_counts;
        for (const std::string& symbol : plan.required_order) {
            if (symbol.empty()) throw std::runtime_error("--symbol-order 不能包含空符号");
            const auto atom = atom_costs.find(symbol);
            if (atom == atom_costs.end()) {
                throw std::runtime_error("--symbol-order 符号 '" + symbol +
                                         "' 不在当前原子集合中（或其成本超过 --max-cost）");
            }
            plan.order_atom_cost.push_back(atom->second);
            ++order_counts[symbol];
        }
        for (const SymbolCountRule& rule : plan.count_rules) {
            const auto found = order_counts.find(rule.symbol);
            if (found == order_counts.end()) continue;
            const std::uint32_t occurrences = found->second;
            if (rule.minimum > occurrences || (!rule.unlimited && rule.maximum < occurrences)) {
                throw std::runtime_error("符号 '" + rule.symbol +
                                         "' 的次数范围与 --symbol-order 中的出现次数冲突");
            }
        }

        const unsigned length = static_cast<unsigned>(plan.required_order.size());
        plan.order_mask_shift = static_cast<std::uint8_t>(used_bits);
        plan.order_length_shift = static_cast<std::uint8_t>(used_bits + length);
        plan.order_length_bits = static_cast<std::uint8_t>(bits_for_u32(length));
        if (used_bits + length + plan.order_length_bits > 32) {
            throw std::runtime_error("符号次数与顺序约束状态合计超过 32 位；请减少约束或缩短顺序");
        }
        plan.order_symbol_mask = low_u32_mask(length);
    }

    plan.active = !plan.count_rules.empty() || !plan.required_order.empty();
    std::uint64_t count_requirement_cost = 0;
    for (const SymbolCountRule& rule : plan.count_rules) {
        count_requirement_cost = saturating_add(
            count_requirement_cost,
            saturating_mul(static_cast<std::uint64_t>(rule.minimum), rule.atom_cost));
    }
    std::uint64_t order_requirement_cost = 0;
    for (const std::uint16_t cost : plan.order_atom_cost) {
        order_requirement_cost = saturating_add(order_requirement_cost, cost);
    }
    plan.maximum_missing_atom_cost = std::max(count_requirement_cost, order_requirement_cost);
    if (plan.active && !plan.can_finish(0, 0)) {
        throw std::runtime_error("符号约束所需的原子成本已经超过 --max-cost");
    }
    return plan;
}

static std::uint32_t extension_field(std::uint32_t state,
                                     const CompiledConstraintExtension& constraint) {
    return (state >> constraint.shift) & constraint.mask;
}

static bool extension_state_fits(std::uint32_t state,
                                 const CompiledConstraintExtension& constraint) {
    return (state & ~constraint.mask) == 0;
}

std::optional<std::uint32_t> ExtensionConstraintPlan::atom_state(const AtomSpec& atom) const {
    if (!active) return 0U;
    std::uint32_t combined = 0;
    const ExtensionAtomContext context{atom.text, atom.value, atom.cost, atom.variable};
    for (const CompiledConstraintExtension& compiled : constraints) {
        const auto state = compiled.extension->atom
                               ? compiled.extension->atom(context)
                               : std::optional<std::uint32_t>{0U};
        if (!state) return std::nullopt;
        if (!extension_state_fits(*state, compiled)) {
            throw std::runtime_error("自定义约束 '" + compiled.extension->name +
                                     "' 的 atom 回调返回值超出 state_bits");
        }
        combined |= *state << compiled.shift;
    }
    return combined;
}

std::optional<std::uint32_t> ExtensionConstraintPlan::apply_unary(
    std::uint32_t state,
    std::string_view operation,
    double input,
    double value,
    unsigned total_cost,
    unsigned max_cost) const {
    if (!active) return 0U;
    std::uint32_t combined = 0;
    const ExtensionUnaryContext context{operation, input, value, total_cost};
    for (const CompiledConstraintExtension& compiled : constraints) {
        const std::uint32_t before = extension_field(state, compiled);
        const auto after = compiled.extension->unary
                               ? compiled.extension->unary(before, context)
                               : std::optional<std::uint32_t>{before};
        if (!after) return std::nullopt;
        if (!extension_state_fits(*after, compiled)) {
            throw std::runtime_error("自定义约束 '" + compiled.extension->name +
                                     "' 的 unary 回调返回值超出 state_bits");
        }
        if (compiled.extension->can_finish &&
            !compiled.extension->can_finish(*after, total_cost, max_cost)) {
            return std::nullopt;
        }
        combined |= *after << compiled.shift;
    }
    return combined;
}

std::optional<std::uint32_t> ExtensionConstraintPlan::apply_binary(
    std::uint32_t left,
    std::uint32_t right,
    std::string_view operation,
    double left_value,
    double right_value,
    double value,
    unsigned total_cost,
    unsigned max_cost) const {
    if (!active) return 0U;
    std::uint32_t combined = 0;
    const ExtensionBinaryContext context{operation, left_value, right_value, value, total_cost};
    for (const CompiledConstraintExtension& compiled : constraints) {
        const std::uint32_t left_state = extension_field(left, compiled);
        const std::uint32_t right_state = extension_field(right, compiled);
        const auto after = compiled.extension->binary
                               ? compiled.extension->binary(left_state, right_state, context)
                               : std::optional<std::uint32_t>{left_state | right_state};
        if (!after) return std::nullopt;
        if (!extension_state_fits(*after, compiled)) {
            throw std::runtime_error("自定义约束 '" + compiled.extension->name +
                                     "' 的 binary 回调返回值超出 state_bits");
        }
        if (compiled.extension->can_finish &&
            !compiled.extension->can_finish(*after, total_cost, max_cost)) {
            return std::nullopt;
        }
        combined |= *after << compiled.shift;
    }
    return combined;
}

bool ExtensionConstraintPlan::satisfied(std::uint32_t state) const {
    if (!active) return true;
    for (const CompiledConstraintExtension& compiled : constraints) {
        if (compiled.extension->satisfied &&
            !compiled.extension->satisfied(extension_field(state, compiled))) {
            return false;
        }
    }
    return true;
}

bool ExtensionConstraintPlan::can_finish(std::uint32_t state,
                                         unsigned current_cost,
                                         unsigned max_cost) const {
    if (current_cost > max_cost) return false;
    if (!active) return true;
    for (const CompiledConstraintExtension& compiled : constraints) {
        if (compiled.extension->can_finish &&
            !compiled.extension->can_finish(
                extension_field(state, compiled), current_cost, max_cost)) {
            return false;
        }
    }
    return true;
}

static ExtensionConstraintPlan compile_extension_constraints() {
    ExtensionConstraintPlan plan;
    unsigned used_bits = 0;
    for (const ConstraintExtension& extension : extension_registry().constraints()) {
        if (used_bits + extension.state_bits > 32) {
            throw std::runtime_error("自定义约束状态合计超过 32 位，请减少 state_bits");
        }
        plan.constraints.push_back(
            {&extension, static_cast<std::uint8_t>(used_bits), low_u32_mask(extension.state_bits)});
        used_bits += extension.state_bits;
    }
    plan.active = !plan.constraints.empty();
    return plan;
}

static std::uint32_t builtin_constraint_state(ConstraintState state) {
    return static_cast<std::uint32_t>(state);
}

static std::uint32_t custom_constraint_state(ConstraintState state) {
    return static_cast<std::uint32_t>(state >> 32U);
}

static ConstraintState join_constraint_state(std::uint32_t builtin, std::uint32_t custom) {
    return static_cast<ConstraintState>(builtin) | (static_cast<ConstraintState>(custom) << 32U);
}

static bool constraints_active(const Config& cfg) {
    return cfg.symbol_constraints.active || cfg.extension_constraints.active;
}

static std::optional<ConstraintState> constraint_atom_state(const Config& cfg,
                                                            const AtomSpec& atom) {
    if (!constraints_active(cfg)) return ConstraintState{0};
    const auto builtin = cfg.symbol_constraints.atom_state(atom.text);
    if (!builtin) return std::nullopt;
    const auto custom = cfg.extension_constraints.atom_state(atom);
    if (!custom || !cfg.extension_constraints.can_finish(*custom, atom.cost, cfg.max_cost)) {
        return std::nullopt;
    }
    const ConstraintState state = join_constraint_state(*builtin, *custom);
    if (!cfg.symbol_constraints.can_finish(*builtin, atom.cost)) return std::nullopt;
    return state;
}

static std::optional<ConstraintState> constraint_apply_unary(const Config& cfg,
                                                             ConstraintState child,
                                                             UnaryKind operation,
                                                             double input,
                                                             double value,
                                                             unsigned total_cost) {
    const std::uint32_t builtin = builtin_constraint_state(child);
    if (!cfg.symbol_constraints.can_finish(builtin, total_cost)) return std::nullopt;
    const auto custom = cfg.extension_constraints.apply_unary(
        custom_constraint_state(child), unary_name(operation), input, value, total_cost, cfg.max_cost);
    if (!custom) return std::nullopt;
    return join_constraint_state(builtin, *custom);
}

static std::optional<ConstraintState> constraint_apply_binary(const Config& cfg,
                                                              ConstraintState left,
                                                              ConstraintState right,
                                                              BinaryKind operation,
                                                              double left_value,
                                                              double right_value,
                                                              double value,
                                                              unsigned total_cost) {
    const auto builtin = cfg.symbol_constraints.combine(
        builtin_constraint_state(left), builtin_constraint_state(right));
    if (!builtin || !cfg.symbol_constraints.can_finish(*builtin, total_cost)) return std::nullopt;
    const auto custom = cfg.extension_constraints.apply_binary(
        custom_constraint_state(left), custom_constraint_state(right), binary_name(operation),
        left_value, right_value, value, total_cost, cfg.max_cost);
    if (!custom) return std::nullopt;
    return join_constraint_state(*builtin, *custom);
}

static std::optional<ConstraintState> constraint_join_equation(const Config& cfg,
                                                               ConstraintState left,
                                                               ConstraintState right,
                                                               double left_value,
                                                               double right_value,
                                                               unsigned total_cost) {
    const auto builtin = cfg.symbol_constraints.combine(
        builtin_constraint_state(left), builtin_constraint_state(right));
    if (!builtin) return std::nullopt;
    const auto custom = cfg.extension_constraints.apply_binary(
        custom_constraint_state(left), custom_constraint_state(right), "=",
        left_value, right_value, left_value - right_value, total_cost, cfg.max_cost);
    if (!custom) return std::nullopt;
    return join_constraint_state(*builtin, *custom);
}

static bool constraint_satisfied(const Config& cfg, ConstraintState state) {
    if (!constraints_active(cfg)) return true;
    return cfg.symbol_constraints.satisfied(builtin_constraint_state(state)) &&
           cfg.extension_constraints.satisfied(custom_constraint_state(state));
}

static bool constraint_can_finish(const Config& cfg,
                                  ConstraintState state,
                                  unsigned current_cost) {
    if (!constraints_active(cfg)) return current_cost <= cfg.max_cost;
    return cfg.symbol_constraints.can_finish(builtin_constraint_state(state), current_cost) &&
           cfg.extension_constraints.can_finish(
               custom_constraint_state(state), current_cost, cfg.max_cost);
}

static std::uint16_t sat_u16(unsigned value) {
    return static_cast<std::uint16_t>(std::min<unsigned>(value, std::numeric_limits<std::uint16_t>::max()));
}

static bool valid_numeric(double value, const Config& cfg) {
    return std::isfinite(value) && std::abs(value) <= cfg.max_abs;
}

static bool gamma_input_is_pole(double value) {
    if (!std::isfinite(value) || value > 0.0) return false;
    const double nearest = std::nearbyint(value);
    const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() *
                             std::max(1.0, std::abs(value));
    return std::abs(value - nearest) <= tolerance;
}

// Digamma is only needed when an enabled gamma node depends on the equation
// variable.  Keeping it out of the default operation set means this branch has
// zero cost in ordinary searches.  Reflection handles negative non-integers;
// recurrence followed by an asymptotic expansion is accurate to roughly full
// double precision over the range where std::tgamma remains useful.
static double digamma(double value) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(value) || gamma_input_is_pole(value)) return nan;
    if (value < 0.5) {
        const double angle = std::numbers::pi_v<double> * value;
        const double sine = std::sin(angle);
        if (std::abs(sine) < 1.0e-14) return nan;
        return digamma(1.0 - value) -
               std::numbers::pi_v<double> * std::cos(angle) / sine;
    }

    double result = 0.0;
    while (value < 8.0) {
        result -= 1.0 / value;
        value += 1.0;
    }
    const double inverse = 1.0 / value;
    const double inverse2 = inverse * inverse;
    result += std::log(value) - 0.5 * inverse -
              inverse2 * (1.0 / 12.0 -
                          inverse2 * (1.0 / 120.0 -
                                      inverse2 * (1.0 / 252.0 - inverse2 / 240.0)));
    return result;
}

static ExtensionLimits extension_limits(const Config& cfg) {
    return {cfg.max_abs, cfg.max_exponent, cfg.max_trig_arg};
}

static std::uint64_t unary_hash(UnaryKind kind, std::uint64_t child) {
    return mix64(0x554e415259ULL ^ (static_cast<std::uint64_t>(kind) << 48U) ^ mix64(child));
}

static std::uint64_t binary_hash(BinaryKind kind, std::uint64_t left, std::uint64_t right) {
    return mix64(0x42494e415259ULL ^ (static_cast<std::uint64_t>(kind) << 48U) ^ mix64(left) ^ std::rotl(mix64(right), 23));
}

static bool unary_node_is(const Node& node, UnaryKind kind) {
    return node.tag == NodeTag::Unary && node.op == static_cast<std::uint8_t>(kind);
}

static std::optional<Candidate> apply_unary(const Config& cfg,
                                             const std::vector<Node>& arena,
                                             const UnarySpec& spec,
                                             ExprId child_id,
                                             std::uint16_t total_cost) {
    const Node& child = arena[child_id];
    const bool has_constraints = constraints_active(cfg);
    if (has_constraints && !constraint_can_finish(cfg, child.constraint_state, total_cost)) {
        return std::nullopt;
    }
    const double x = child.value;
    double value = 0.0;

    switch (spec.kind) {
        case UnaryKind::Neg:
            if (unary_node_is(child, UnaryKind::Neg)) return std::nullopt;
            value = -x;
            break;
        case UnaryKind::Inv:
            if (unary_node_is(child, UnaryKind::Inv) || x == 0.0) return std::nullopt;
            value = 1.0 / x;
            break;
        case UnaryKind::Sqrt:
            if (x < 0.0) return std::nullopt;
            value = std::sqrt(x);
            break;
        case UnaryKind::Cbrt:
            if (unary_node_is(child, UnaryKind::Cube)) return std::nullopt;
            value = std::cbrt(x);
            break;
        case UnaryKind::Sqr:
            if (unary_node_is(child, UnaryKind::Sqrt)) return std::nullopt;
            value = x * x;
            break;
        case UnaryKind::Cube:
            if (unary_node_is(child, UnaryKind::Cbrt)) return std::nullopt;
            value = x * x * x;
            break;
        case UnaryKind::Ln:
            if (x <= 0.0 || unary_node_is(child, UnaryKind::Exp)) return std::nullopt;
            value = std::log(x);
            break;
        case UnaryKind::Log10:
            if (x <= 0.0) return std::nullopt;
            value = std::log10(x);
            break;
        case UnaryKind::Exp:
            if (unary_node_is(child, UnaryKind::Ln)) return std::nullopt;
            if (x > std::log(cfg.max_abs)) return std::nullopt;
            value = std::exp(x);
            break;
        case UnaryKind::Sin:
            if (unary_node_is(child, UnaryKind::Asin)) return std::nullopt;
            if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
            value = std::sin(x);
            break;
        case UnaryKind::Cos:
            if (unary_node_is(child, UnaryKind::Acos)) return std::nullopt;
            if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
            value = std::cos(x);
            break;
        case UnaryKind::Tan:
            if (unary_node_is(child, UnaryKind::Atan)) return std::nullopt;
            if (std::abs(x) > cfg.max_trig_arg || std::abs(std::cos(x)) < 1.0e-12) return std::nullopt;
            value = std::tan(x);
            break;
        case UnaryKind::Asin:
            if (x < -1.0 || x > 1.0) return std::nullopt;
            value = std::asin(x);
            break;
        case UnaryKind::Acos:
            if (x < -1.0 || x > 1.0) return std::nullopt;
            value = std::acos(x);
            break;
        case UnaryKind::Atan:
            value = std::atan(x);
            break;
        case UnaryKind::Sinh:
            if (unary_node_is(child, UnaryKind::Asinh) || std::abs(x) > cfg.max_trig_arg) {
                return std::nullopt;
            }
            value = std::sinh(x);
            break;
        case UnaryKind::Cosh:
            if (unary_node_is(child, UnaryKind::Acosh) || std::abs(x) > cfg.max_trig_arg) {
                return std::nullopt;
            }
            value = std::cosh(x);
            break;
        case UnaryKind::Tanh:
            if (unary_node_is(child, UnaryKind::Atanh) || std::abs(x) > cfg.max_trig_arg) {
                return std::nullopt;
            }
            value = std::tanh(x);
            if (std::abs(value) == 1.0) return std::nullopt;
            break;
        case UnaryKind::Asinh:
            if (unary_node_is(child, UnaryKind::Sinh)) return std::nullopt;
            value = std::asinh(x);
            break;
        case UnaryKind::Acosh:
            if (x < 1.0) return std::nullopt;
            value = std::acosh(x);
            break;
        case UnaryKind::Atanh:
            if (x <= -1.0 || x >= 1.0 || unary_node_is(child, UnaryKind::Tanh)) {
                return std::nullopt;
            }
            value = std::atanh(x);
            break;
        case UnaryKind::Gamma:
            if (gamma_input_is_pole(x)) return std::nullopt;
            value = std::tgamma(x);
            if (value == 0.0) return std::nullopt;
            break;
        case UnaryKind::Abs:
            if (x >= 0.0 || unary_node_is(child, UnaryKind::Abs)) return std::nullopt;
            value = std::abs(x);
            break;
        case UnaryKind::Fact: {
            if (child.depends_on_x) return std::nullopt;
            const double rounded = std::nearbyint(x);
            if (x < 0.0 || rounded > 170.0 || std::abs(x - rounded) > 1.0e-12 * std::max(1.0, std::abs(x))) {
                return std::nullopt;
            }
            value = std::tgamma(rounded + 1.0);
            break;
        }
        default: {
            const auto custom = custom_unary_index(spec.kind);
            if (!custom) return std::nullopt;
            const auto result = extension_registry().unary_operations()[*custom].evaluate(
                x, extension_limits(cfg));
            if (!result) return std::nullopt;
            value = *result;
            break;
        }
    }
    if (!valid_numeric(value, cfg)) return std::nullopt;

    ConstraintState constraint_state = child.constraint_state;
    if (has_constraints) {
        const auto transitioned = constraint_apply_unary(
            cfg, child.constraint_state, spec.kind, x, value, total_cost);
        if (!transitioned) return std::nullopt;
        constraint_state = *transitioned;
    }

    double derivative = 0.0;
    if (child.depends_on_x) {
        const double dx = child.derivative;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        switch (spec.kind) {
            case UnaryKind::Neg: derivative = -dx; break;
            case UnaryKind::Inv: derivative = -dx / (x * x); break;
            case UnaryKind::Sqrt: derivative = value == 0.0 ? nan : dx / (2.0 * value); break;
            case UnaryKind::Cbrt: derivative = value == 0.0 ? nan : dx / (3.0 * value * value); break;
            case UnaryKind::Sqr: derivative = 2.0 * x * dx; break;
            case UnaryKind::Cube: derivative = 3.0 * x * x * dx; break;
            case UnaryKind::Ln: derivative = dx / x; break;
            case UnaryKind::Log10: derivative = dx / (x * std::numbers::ln10_v<double>); break;
            case UnaryKind::Exp: derivative = value * dx; break;
            case UnaryKind::Sin: derivative = std::cos(x) * dx; break;
            case UnaryKind::Cos: derivative = -std::sin(x) * dx; break;
            case UnaryKind::Tan: {
                const double cosine = std::cos(x);
                derivative = dx / (cosine * cosine);
                break;
            }
            case UnaryKind::Asin:
                derivative = (std::abs(x) == 1.0) ? nan : dx / std::sqrt(1.0 - x * x);
                break;
            case UnaryKind::Acos:
                derivative = (std::abs(x) == 1.0) ? nan : -dx / std::sqrt(1.0 - x * x);
                break;
            case UnaryKind::Atan: derivative = dx / (1.0 + x * x); break;
            case UnaryKind::Sinh: derivative = std::cosh(x) * dx; break;
            case UnaryKind::Cosh: derivative = std::sinh(x) * dx; break;
            case UnaryKind::Tanh: derivative = (1.0 - value * value) * dx; break;
            case UnaryKind::Asinh: derivative = dx / std::sqrt(1.0 + x * x); break;
            case UnaryKind::Acosh:
                derivative = x == 1.0 ? nan : dx / (std::sqrt(x - 1.0) * std::sqrt(x + 1.0));
                break;
            case UnaryKind::Atanh: derivative = dx / (1.0 - x * x); break;
            case UnaryKind::Gamma: derivative = value * digamma(x) * dx; break;
            case UnaryKind::Abs: derivative = x < 0.0 ? -dx : dx; break;
            case UnaryKind::Fact: derivative = nan; break;
            default: {
                const auto custom = custom_unary_index(spec.kind);
                const auto& operation = extension_registry().unary_operations()[*custom];
                derivative = operation.derivative ? operation.derivative(x, value, dx) : nan;
                break;
            }
        }
    }

    Candidate out;
    out.value = value;
    out.derivative = derivative;
    out.cost = total_cost;
    out.nodes = sat_u16(static_cast<unsigned>(child.nodes) + 1U);
    out.depth = sat_u16(static_cast<unsigned>(child.depth) + 1U);
    out.hash = unary_hash(spec.kind, child.hash);
    out.tag = NodeTag::Unary;
    out.op = static_cast<std::uint8_t>(spec.kind);
    out.left = child_id;
    out.constraint_state = constraint_state;
    out.depends_on_x = child.depends_on_x;
    return out;
}


static std::optional<Candidate> apply_binary(const Config& cfg,
                                              const std::vector<Node>& arena,
                                              const BinarySpec& spec,
                                              ExprId left_id,
                                              ExprId right_id,
                                              std::uint16_t total_cost) {
    const Node* left = &arena[left_id];
    const Node* right = &arena[right_id];

    if (is_commutative(spec.kind)) {
        const auto lk = std::pair{left->hash, left_id};
        const auto rk = std::pair{right->hash, right_id};
        if (rk < lk) {
            std::swap(left_id, right_id);
            std::swap(left, right);
        }
    }

    const double a = left->value;
    const double b = right->value;
    double value = 0.0;

    switch (spec.kind) {
        case BinaryKind::Add:
            if ((a == 0.0 && !left->depends_on_x) || (b == 0.0 && !right->depends_on_x)) return std::nullopt;
            value = a + b;
            break;
        case BinaryKind::Sub:
            if (b == 0.0 && !right->depends_on_x) return std::nullopt;
            value = a - b;
            break;
        case BinaryKind::Mul:
            if ((a == 0.0 && !left->depends_on_x) || (b == 0.0 && !right->depends_on_x) ||
                (a == 1.0 && !left->depends_on_x) || (b == 1.0 && !right->depends_on_x)) {
                return std::nullopt;
            }
            value = a * b;
            break;
        case BinaryKind::Div:
            if (b == 0.0 || (a == 0.0 && !left->depends_on_x) ||
                (b == 1.0 && !right->depends_on_x)) {
                return std::nullopt;
            }
            value = a / b;
            break;
        case BinaryKind::Pow: {
            if ((b == 1.0 && !right->depends_on_x) || (a == 1.0 && !left->depends_on_x) ||
                std::abs(b) > cfg.max_exponent) {
                return std::nullopt;
            }
            if (!right->depends_on_x) {
                const double rounded = std::nearbyint(b);
                const bool exact_integer = std::abs(b - rounded) <=
                    1.0e-12 * std::max(1.0, std::abs(b));
                if (exact_integer &&
                    ((rounded == 2.0 && unary_node_is(*left, UnaryKind::Sqrt)) ||
                     (rounded == 3.0 && unary_node_is(*left, UnaryKind::Cbrt)))) {
                    return std::nullopt;
                }
            }
            if (a == 0.0) {
                if (b <= 0.0) return std::nullopt;
                const double rounded = std::nearbyint(b);
                if (left->depends_on_x && !right->depends_on_x && rounded >= 2.0 &&
                    std::abs(b - rounded) <= 1.0e-12 * std::max(1.0, std::abs(b))) {
                    value = 0.0;
                    break;
                }
                return std::nullopt;  // Constant zero itself is always cheaper than 0^b.
            }
            if (a < 0.0) {
                const double rounded = std::nearbyint(b);
                if (std::abs(b - rounded) > 1.0e-12 * std::max(1.0, std::abs(b))) return std::nullopt;
                value = std::pow(a, rounded);
            } else {
                value = std::pow(a, b);
            }
            break;
        }
        default: {
            const auto custom = custom_binary_index(spec.kind);
            if (!custom) return std::nullopt;
            const auto result = extension_registry().binary_operations()[*custom].evaluate(
                a, b, extension_limits(cfg));
            if (!result) return std::nullopt;
            value = *result;
            break;
        }
    }
    if (!valid_numeric(value, cfg)) return std::nullopt;

    ConstraintState constraint_state = 0;
    if (constraints_active(cfg)) {
        const auto transitioned = constraint_apply_binary(
            cfg, left->constraint_state, right->constraint_state, spec.kind,
            a, b, value, total_cost);
        if (!transitioned) return std::nullopt;
        constraint_state = *transitioned;
    }

    const bool depends_on_x = left->depends_on_x || right->depends_on_x;
    double derivative = 0.0;
    if (depends_on_x) {
        const double da = left->derivative;
        const double db = right->derivative;
        switch (spec.kind) {
            case BinaryKind::Add: derivative = da + db; break;
            case BinaryKind::Sub: derivative = da - db; break;
            case BinaryKind::Mul: derivative = da * b + a * db; break;
            case BinaryKind::Div: derivative = (da * b - a * db) / (b * b); break;
            case BinaryKind::Pow:
                if (a > 0.0) {
                    derivative = value * (db * std::log(a) + b * da / a);
                } else if (!right->depends_on_x) {
                    derivative = b * std::pow(a, b - 1.0) * da;
                } else {
                    derivative = std::numeric_limits<double>::quiet_NaN();
                }
                break;
            default: {
                const auto custom = custom_binary_index(spec.kind);
                const auto& operation = extension_registry().binary_operations()[*custom];
                derivative = operation.derivative
                                 ? operation.derivative(a, b, value, da, db)
                                 : std::numeric_limits<double>::quiet_NaN();
                break;
            }
        }
    }

    Candidate out;
    out.value = value;
    out.derivative = derivative;
    out.cost = total_cost;
    out.nodes = sat_u16(static_cast<unsigned>(left->nodes) + right->nodes + 1U);
    out.depth = sat_u16(std::max(static_cast<unsigned>(left->depth), static_cast<unsigned>(right->depth)) + 1U);
    out.hash = binary_hash(spec.kind, left->hash, right->hash);
    out.tag = NodeTag::Binary;
    out.op = static_cast<std::uint8_t>(spec.kind);
    out.left = left_id;
    out.right = right_id;
    out.constraint_state = constraint_state;
    out.depends_on_x = depends_on_x;
    return out;
}

struct GenTask {
    enum class Type : std::uint8_t { Unary, Binary } type{Type::Unary};
    std::size_t op_index{};
    std::uint16_t left_cost{};
    std::uint16_t right_cost{};
    std::size_t begin{};
    std::size_t end{};
    bool full{};
    bool equal_commutative{};
    std::size_t samples_per_outer{1};
    std::uint16_t total_cost{};
    bool reverse{};
};

struct TaskResult {
    std::vector<Candidate> candidates;
    std::vector<Candidate> extra_candidates;
    std::uint64_t attempted{};
    std::uint64_t valid{};
};

static std::optional<double> desired_right(BinaryKind kind, double left, double target) {
    switch (kind) {
        case BinaryKind::Add:
            return target - left;
        case BinaryKind::Sub:
            return left - target;
        case BinaryKind::Mul:
            if (left != 0.0) return target / left;
            break;
        case BinaryKind::Div:
            if (target != 0.0) return left / target;
            break;
        case BinaryKind::Pow:
            if (left > 0.0 && left != 1.0 && target > 0.0) {
                const double d = std::log(target) / std::log(left);
                if (std::isfinite(d)) return d;
            }
            break;
        default:
            if (const auto custom = custom_binary_index(kind)) {
                const auto& operation = extension_registry().binary_operations()[*custom];
                if (operation.desired_right) return operation.desired_right(left, target);
            }
            break;
    }
    return std::nullopt;
}

static std::optional<double> desired_left(BinaryKind kind, double right, double target) {
    switch (kind) {
        case BinaryKind::Add:
            return target - right;
        case BinaryKind::Sub:
            return target + right;
        case BinaryKind::Mul:
            if (right != 0.0) return target / right;
            break;
        case BinaryKind::Div:
            return target * right;
        case BinaryKind::Pow:
            if (right == 0.0) break;
            if (target > 0.0) {
                const double base = std::exp(std::log(target) / right);
                if (std::isfinite(base)) return base;
            } else if (target < 0.0) {
                const double rounded = std::nearbyint(right);
                if (std::abs(right - rounded) <= 1.0e-12 * std::max(1.0, std::abs(right)) &&
                    std::fmod(std::abs(rounded), 2.0) == 1.0) {
                    const double base = -std::exp(std::log(-target) / rounded);
                    if (std::isfinite(base)) return base;
                }
            }
            break;
        default:
            if (const auto custom = custom_binary_index(kind)) {
                const auto& operation = extension_registry().binary_operations()[*custom];
                if (operation.desired_left) return operation.desired_left(right, target);
            }
            break;
    }
    return std::nullopt;
}

static std::size_t lower_bound_value(const std::vector<ExprId>& ids,
                                     const std::vector<Node>& arena,
                                     double value,
                                     std::size_t lo = 0) {
    auto first = ids.begin() + static_cast<std::ptrdiff_t>(lo);
    const auto it = std::lower_bound(first, ids.end(), value, [&](ExprId id, double v) {
        return arena[id].value < v;
    });
    return static_cast<std::size_t>(it - ids.begin());
}

static void add_near_indices(std::vector<std::size_t>& indices,
                             const std::vector<ExprId>& ids,
                             const std::vector<Node>& arena,
                             double desired,
                             std::size_t lower_limit,
                             std::size_t count = 5) {
    if (!std::isfinite(desired) || lower_limit >= ids.size()) return;
    const std::size_t pos = lower_bound_value(ids, arena, desired, lower_limit);
    std::size_t left = pos;
    std::size_t right = pos;
    bool have_left = left > lower_limit;
    if (have_left) --left;
    bool have_right = right < ids.size();
    while (count-- > 0 && (have_left || have_right)) {
        const bool take_left = !have_right ||
            (have_left && std::abs(arena[ids[left]].value - desired) <=
                          std::abs(arena[ids[right]].value - desired));
        if (take_left) {
            indices.push_back(left);
            if (left == lower_limit) have_left = false;
            else --left;
        } else {
            indices.push_back(right++);
            have_right = right < ids.size();
        }
    }
}

static void add_window_indices(std::vector<std::size_t>& indices,
                               const std::vector<ExprId>& ids,
                               const std::vector<Node>& arena,
                               double desired,
                               std::size_t lower_limit,
                               std::size_t count = 5) {
    if (!std::isfinite(desired) || lower_limit >= ids.size() || count == 0) return;
    const std::size_t pos = lower_bound_value(ids, arena, desired, lower_limit);
    const std::size_t before = count / 2;
    const std::size_t begin = pos > lower_limit + before ? pos - before : lower_limit;
    const std::size_t after = count - before;
    const std::size_t end = std::min(ids.size(), pos + after);
    for (std::size_t index = begin; index < end; ++index) indices.push_back(index);
}

class ParallelExecutor {
public:
    explicit ParallelExecutor(unsigned threads) : configured_threads_(std::max(1U, threads)) {}

    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor& operator=(const ParallelExecutor&) = delete;

    ~ParallelExecutor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        start_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    template <class Function>
    void run(std::size_t count, Function&& function) {
        if (count == 0) return;
        if (configured_threads_ == 1 || count == 1) {
            for (std::size_t i = 0; i < count; ++i) function(i);
            return;
        }
        ensure_workers();

        std::unique_lock<std::mutex> lock(mutex_);
        task_ = std::forward<Function>(function);
        task_count_ = count;
        next_.store(0, std::memory_order_relaxed);
        failed_.store(false, std::memory_order_relaxed);
        error_ = nullptr;
        pending_workers_ = workers_.size();
        ++generation_;
        start_.notify_all();

        lock.unlock();
        execute_available_tasks();
        lock.lock();
        done_.wait(lock, [this] { return pending_workers_ == 0; });
        const std::exception_ptr error = error_;
        task_ = {};
        lock.unlock();
        if (error) std::rethrow_exception(error);
    }

private:
    void ensure_workers() {
        if (!workers_.empty()) return;
        const unsigned background_threads = configured_threads_ - 1;
        workers_.reserve(background_threads);
        for (unsigned i = 0; i < background_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    void execute_available_tasks() noexcept {
        try {
            while (!failed_.load(std::memory_order_relaxed)) {
                const std::size_t i = next_.fetch_add(1, std::memory_order_relaxed);
                if (i >= task_count_) break;
                task_(i);
            }
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> error_lock(mutex_);
            if (!error_) error_ = std::current_exception();
        }
    }

    void worker_loop() {
        std::size_t observed_generation = 0;
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            start_.wait(lock, [&] { return stopping_ || generation_ != observed_generation; });
            if (stopping_) return;
            observed_generation = generation_;
            lock.unlock();

            execute_available_tasks();

            lock.lock();
            if (--pending_workers_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    unsigned configured_threads_{};
    std::mutex mutex_;
    std::condition_variable start_;
    std::condition_variable done_;
    std::function<void(std::size_t)> task_;
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> failed_{false};
    std::size_t task_count_{};
    std::size_t pending_workers_{};
    std::size_t generation_{};
    std::exception_ptr error_;
    bool stopping_{false};
};

struct CostStats {
    std::uint64_t attempted{};
    std::uint64_t valid{};
    std::size_t task_candidates{};
    std::size_t kept{};
    double seconds{};
};

struct SearchStats {
    std::vector<CostStats> by_cost;
    std::uint64_t attempted{};
    std::uint64_t valid{};
    std::size_t kept{};
    std::size_t pareto_extras{};
    double seconds{};
    unsigned completed_cost{};
    unsigned generated_cost{};
    bool used_mitm{};
    bool used_inverse_templates{};
    bool used_deep_compositions{};
    bool used_exploration{};
    bool used_genetic{};
    bool used_pslq{};
    bool used_egraph{};
    bool used_mcts{};
    bool used_portfolio{};
    unsigned genetic_generations{};
    std::size_t genetic_repairs{};
    std::size_t genetic_repairs_kept{};
    std::size_t genetic_crossovers{};
    std::size_t genetic_crossovers_kept{};
    std::size_t pslq_relations{};
    std::size_t pslq_candidates{};
    std::size_t egraph_rewrites{};
    std::size_t egraph_candidates{};
    std::size_t mcts_expansions{};
    std::size_t mcts_candidates{};
    std::size_t inverse_candidates{};
    double deterministic_seconds{};
    double mitm_seconds{};
    double inverse_seconds{};
    double deep_seconds{};
    double egraph_seconds{};
    double pslq_seconds{};
    double mcts_seconds{};
    double genetic_seconds{};
};

struct SearchRun {
    Config cfg;
    std::vector<AtomSpec> atoms;
    std::vector<Node> arena;
    std::vector<std::vector<ExprId>> layers;
    std::vector<std::vector<ExprId>> extra_layers;
    SearchStats stats;
};

struct EquationMatch {
    ExprId left{kNoExpr};
    ExprId right{kNoExpr};
    double root{};
    double abs_err{};
    double residual{};
    unsigned cost{};
    unsigned nodes{};
};

static bool equation_better(const EquationMatch& a, const EquationMatch& b) {
    if (a.abs_err != b.abs_err) return a.abs_err < b.abs_err;
    if (a.cost != b.cost) return a.cost < b.cost;
    if (a.nodes != b.nodes) return a.nodes < b.nodes;
    if (a.left != b.left) return a.left < b.left;
    return a.right < b.right;
}

static std::vector<EquationMatch> collect_equation_matches(const Config& cfg,
                                                           const std::vector<Node>& arena,
                                                           std::size_t limit);
static std::string render_expression_text(const std::vector<AtomSpec>& atoms,
                                          const std::vector<Node>& arena,
                                          ExprId id);
static std::string render_expression_latex_text(const std::vector<AtomSpec>& atoms,
                                                const std::vector<Node>& arena,
                                                ExprId id);

static std::string render_candidate_expression(const std::vector<AtomSpec>& atoms,
                                               const std::vector<Node>& arena,
                                               const Candidate& candidate);
static std::string render_candidate_latex(const std::vector<AtomSpec>& atoms,
                                          const std::vector<Node>& arena,
                                          const Candidate& candidate);
static std::string json_escape(std::string_view text);

class LiveTopReporter {
public:
    using Renderer = std::function<std::string(const Candidate&)>;
    using Acceptor = std::function<bool(const Candidate&)>;

    LiveTopReporter(double target,
                    unsigned value_bits,
                    ErrorRange error_range,
                    std::size_t capacity,
                    double interval,
                    Renderer renderer,
                    Renderer latex_renderer,
                    bool json_events,
                    bool latex_output,
                    Acceptor acceptor)
        : target_(target),
          value_bits_(value_bits),
          error_range_(error_range),
          capacity_(capacity),
          interval_(interval),
          renderer_(std::move(renderer)),
          latex_renderer_(std::move(latex_renderer)),
          json_events_(json_events),
          latex_output_(latex_output),
          acceptor_(std::move(acceptor)),
          started_(std::chrono::steady_clock::now()),
          last_emit_(started_) {}

    void consider_batch(const std::vector<Candidate>& candidates, unsigned current_cost) {
        if (capacity_ == 0 || candidates.empty()) return;

        std::vector<Candidate> local;
        local.reserve(std::min(capacity_, candidates.size()));
        auto heap_compare = [&](const Candidate& a, const Candidate& b) { return better(a, b); };
        for (const Candidate& candidate : candidates) {
            if (!error_range_.contains(candidate.value - target_) || !acceptor_(candidate)) continue;
            if (local.size() < capacity_) {
                local.push_back(candidate);
                std::push_heap(local.begin(), local.end(), heap_compare);
            } else if (better(candidate, local.front())) {
                std::pop_heap(local.begin(), local.end(), heap_compare);
                local.back() = candidate;
                std::push_heap(local.begin(), local.end(), heap_compare);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Candidate> merged = best_;
        merged.insert(merged.end(), local.begin(), local.end());
        std::sort(merged.begin(), merged.end(), [&](const Candidate& a, const Candidate& b) {
            return better(a, b);
        });

        std::vector<Candidate> next;
        next.reserve(std::min(capacity_, merged.size()));
        FastSet<std::uint64_t> buckets;
        buckets.reserve(capacity_ * 2 + 1);
        FastSet<std::string> formulas;
        formulas.reserve(capacity_ * 2 + 1);
        for (const Candidate& candidate : merged) {
            if (!buckets.insert(value_bucket(candidate.value, value_bits_)).second) continue;
            if (!formulas.insert(latex_renderer_(candidate)).second) continue;
            next.push_back(candidate);
            if (next.size() == capacity_) break;
        }

        const bool changed = !same_results(best_, next);
        best_ = std::move(next);
        const auto now = std::chrono::steady_clock::now();
        if (changed) dirty_ = true;
        if (changed && std::chrono::duration<double>(now - last_emit_).count() >= interval_) {
            emit_locked(current_cost, now);
        }
    }

    void snapshot(unsigned completed_cost) {
        if (capacity_ == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dirty_ && last_emit_cost_ == completed_cost) return;
        emit_locked(completed_cost, std::chrono::steady_clock::now());
    }

private:
    bool better(const Candidate& a, const Candidate& b) const {
        const double ea = abs_error(a.value, target_);
        const double eb = abs_error(b.value, target_);
        if (ea != eb) return ea < eb;
        if (a.cost != b.cost) return a.cost < b.cost;
        if (a.nodes != b.nodes) return a.nodes < b.nodes;
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.hash < b.hash;
    }

    static bool same_results(const std::vector<Candidate>& a, const std::vector<Candidate>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].hash != b[i].hash || a[i].value != b[i].value || a[i].cost != b[i].cost) return false;
        }
        return true;
    }

    void emit_locked(unsigned current_cost, std::chrono::steady_clock::time_point now) {
        last_emit_ = now;
        last_emit_cost_ = current_cost;
        dirty_ = false;
        std::ostringstream out;
        const double elapsed = std::chrono::duration<double>(now - started_).count();
        if (json_events_) {
            out << "[fates-live] {\"type\":\"top\",\"cost\":" << current_cost
                << ",\"elapsed\":" << std::setprecision(9) << elapsed
                << ",\"capacity\":" << capacity_ << ",\"results\":[";
            for (std::size_t index = 0; index < best_.size(); ++index) {
                const Candidate& candidate = best_[index];
                if (index != 0) out << ',';
                out << "{\"rank\":" << (index + 1) << ",\"cost\":" << candidate.cost
                    << ",\"value\":" << std::setprecision(17) << candidate.value
                    << ",\"signed_error\":" << (candidate.value - target_)
                    << ",\"absolute_error\":" << abs_error(candidate.value, target_)
                    << ",\"expression\":\"" << json_escape(renderer_(candidate))
                    << "\",\"latex\":\"" << json_escape(latex_renderer_(candidate)) << "\"}";
            }
            out << "]}\n";
            std::cerr << out.str() << std::flush;
            return;
        }
        out << "[live] cost=" << current_cost << " elapsed=" << std::fixed << std::setprecision(2)
            << elapsed << "s top=" << best_.size() << '/'
            << capacity_ << '\n'
            << "  #  cost  value                  signed_error    abs_error       expression\n"
            << "---  ----  ---------------------  --------------  --------------  ------------------------------\n";
        for (std::size_t i = 0; i < best_.size(); ++i) {
            const Candidate& candidate = best_[i];
            out << std::setw(3) << (i + 1) << "  " << std::setw(4) << candidate.cost << "  "
                << std::setw(21) << std::setprecision(15) << std::defaultfloat << candidate.value << "  "
                << std::setw(14) << std::scientific << std::setprecision(6) << (candidate.value - target_)
                << "  " << std::setw(14) << abs_error(candidate.value, target_)
                << "  " << (latex_output_ ? latex_renderer_(candidate) : renderer_(candidate)) << '\n';
        }
        std::cerr << out.str() << std::flush;
    }

    double target_{};
    unsigned value_bits_{};
    ErrorRange error_range_;
    std::size_t capacity_{};
    double interval_{};
    Renderer renderer_;
    Renderer latex_renderer_;
    bool json_events_{};
    bool latex_output_{};
    Acceptor acceptor_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_emit_;
    unsigned last_emit_cost_{};
    bool dirty_{false};
    std::mutex mutex_;
    std::vector<Candidate> best_;
};

class SearchEngine {
public:
    explicit SearchEngine(Config cfg)
        : cfg_(std::move(cfg)),
          executor_(cfg_.threads),
          atoms_(build_atoms(cfg_)),
          layers_(cfg_.max_cost + 1),
          extra_layers_(cfg_.max_cost + 1),
          stats_{std::vector<CostStats>(cfg_.max_cost + 1)} {
        cfg_.symbol_constraints = compile_symbol_constraints(cfg_, atoms_);
        cfg_.extension_constraints = compile_extension_constraints();
        auto parsed = parse_ops(cfg_.ops);
        unary_ops_ = std::move(parsed.first);
        binary_ops_ = std::move(parsed.second);
        arena_.reserve((cfg_.beam + pareto_extra_cap()) * cfg_.max_cost + atoms_.size());
        seen_.reserve(cfg_.beam * cfg_.max_cost * 2 + atoms_.size());
        if (cfg_.pareto_slots > 1) {
            extra_seen_.reserve(pareto_extra_cap() * cfg_.max_cost * 2 + 1);
        }
    }

    bool dry_run_requested() const { return cfg_.dry_run; }

    void print_configuration() const {
        const unsigned generated_cost = generation_cost_limit();
        if (cfg_.json) {
            std::cout << "{\n"
                      << "  \"program\": \"" << kProgramName << "\",\n"
                      << "  \"version\": \"" << kProgramVersion << "\",\n"
                      << "  \"configuration_valid\": true,\n"
                      << "  \"container_backend\": \"" << kContainerBackend << "\",\n"
                      << "  \"target\": " << std::setprecision(17) << cfg_.target << ",\n"
                      << "  \"result_mode\": \"" << cfg_.mode << "\",\n"
                      << "  \"search_mode\": \"" << configured_search_mode() << "\",\n"
                      << "  \"atoms\": " << atoms_.size() << ",\n"
                      << "  \"unary_operators\": " << unary_ops_.size() << ",\n"
                      << "  \"binary_operators\": " << binary_ops_.size() << ",\n"
                      << "  \"max_cost\": " << cfg_.max_cost << ",\n"
                      << "  \"generated_cost\": " << generated_cost << ",\n"
                      << "  \"beam\": " << cfg_.beam << ",\n"
                      << "  \"pairs\": " << cfg_.pair_budget << ",\n"
                      << "  \"deep_frontier\": " << cfg_.deep_frontier << ",\n"
                      << "  \"threads\": " << cfg_.threads << ",\n"
                      << "  \"value_prune\": \"" << value_prune_mode_name(cfg_.value_prune) << "\",\n"
                      << "  \"explore_pairs\": " << cfg_.explore_pairs << ",\n"
                      << "  \"digits\": \"" << json_escape(cfg_.digits) << "\",\n"
                      << "  \"constants\": \"" << json_escape(cfg_.constants) << "\",\n"
                      << "  \"operators\": \"" << json_escape(cfg_.ops) << "\",\n"
                      << "  \"symbol_count_rules\": " << cfg_.symbol_constraints.count_rules.size() << ",\n"
                      << "  \"symbol_order_length\": " << cfg_.symbol_constraints.required_order.size() << ",\n"
                      << "  \"extension_constraints\": " << cfg_.extension_constraints.constraints.size() << ",\n"
                      << "  \"equations\": " << (cfg_.equations ? "true" : "false") << ",\n"
                      << "  \"equation_search\": \"" << equation_search_mode_name(cfg_.equation_search) << "\",\n"
                      << "  \"equation_quality\": \"" << equation_quality_mode_name(cfg_.equation_quality) << "\",\n"
                      << "  \"portfolio\": " << (cfg_.portfolio ? "true" : "false") << ",\n"
                      << "  \"genetic\": " << (cfg_.genetic ? "true" : "false") << ",\n"
                      << "  \"pslq\": " << (cfg_.pslq ? "true" : "false") << ",\n"
                      << "  \"egraph\": " << (cfg_.egraph ? "true" : "false") << ",\n"
                      << "  \"mcts\": " << (cfg_.mcts ? "true" : "false") << ",\n"
                      << "  \"genetic_crossover\": " << cfg_.genetic_crossover << ",\n"
                      << "  \"genetic_novelty\": " << cfg_.genetic_novelty << ",\n"
                      << "  \"genetic_tournament\": " << cfg_.genetic_tournament << ",\n"
                      << "  \"pslq_basis\": " << cfg_.pslq_basis << ",\n"
                      << "  \"pslq_pairs\": " << cfg_.pslq_pairs << ",\n"
                      << "  \"pslq_coefficient\": " << cfg_.pslq_max_coefficient << ",\n"
                      << "  \"pslq_steps\": " << cfg_.pslq_steps << ",\n"
                      << "  \"pslq_tolerance\": " << cfg_.pslq_tolerance << ",\n"
                      << "  \"egraph_seeds\": " << cfg_.egraph_seeds << ",\n"
                      << "  \"egraph_rounds\": " << cfg_.egraph_rounds << ",\n"
                      << "  \"egraph_nodes\": " << cfg_.egraph_node_limit << ",\n"
                      << "  \"mcts_iterations\": " << cfg_.mcts_iterations << ",\n"
                      << "  \"mcts_depth\": " << cfg_.mcts_depth << ",\n"
                      << "  \"mcts_branching\": " << cfg_.mcts_branching << ",\n"
                      << "  \"mcts_exploration\": " << cfg_.mcts_exploration << ",\n"
                      << "  \"mcts_elegance\": " << cfg_.mcts_elegance << ",\n"
                      << "  \"mcts_seed\": " << cfg_.mcts_seed << "\n"
                      << "}\n";
            return;
        }

        print_banner(std::cout);
        std::cout << "Configuration valid\n\n"
                  << "  Target            : " << std::setprecision(17) << cfg_.target << '\n'
                  << "  Container backend : " << kContainerBackend << '\n'
                  << "  Search mode       : " << configured_search_mode() << '\n'
                  << "  Cost              : " << cfg_.max_cost << " (generated " << generated_cost << ")\n"
                  << "  Beam / pairs      : " << cfg_.beam << " / " << cfg_.pair_budget << '\n'
                   << "  Threads / bits    : " << cfg_.threads << " / " << cfg_.value_bits << '\n'
                   << "  Value pruning     : " << value_prune_mode_name(cfg_.value_prune)
                   << " (effective bits " << state_value_bits(cfg_) << ")\n"
                  << "  Equation policy   : " << equation_search_mode_name(cfg_.equation_search)
                  << " / " << equation_quality_mode_name(cfg_.equation_quality) << '\n'
                  << "  Explore pairs     : " << cfg_.explore_pairs << " / outer candidate\n"
                  << "  Deep frontier     : " << cfg_.deep_frontier
                  << (cfg_.deep_frontier == 0 ? " (unlimited)\n" : " / cost layer\n")
                  << "  Atoms / operators : " << atoms_.size() << " / "
                  << (unary_ops_.size() + binary_ops_.size()) << '\n'
                  << "  Digits            : '" << cfg_.digits << "'\n"
                  << "  Constants         : '" << cfg_.constants << "'\n"
                  << "  Operators         : '" << cfg_.ops << "'\n"
                  << "  Count / order     : " << cfg_.symbol_constraints.count_rules.size() << " / "
                  << cfg_.symbol_constraints.required_order.size() << '\n'
                  << "  Extensions        : " << extension_registry().unary_operations().size() << " unary / "
                  << extension_registry().binary_operations().size() << " binary / "
                  << cfg_.extension_constraints.constraints.size() << " constraints\n"
                  << "  Advanced phases   : PSLQ " << (cfg_.pslq ? "on" : "off")
                  << " / e-graph " << (cfg_.egraph ? "on" : "off")
                  << " / MCTS " << (cfg_.mcts ? "on" : "off")
                  << " / genetic " << (cfg_.genetic ? "on" : "off") << '\n';
    }

    SearchRun run() {
        const auto all_start = std::chrono::steady_clock::now();
        stats_.used_exploration = cfg_.explore_pairs > 0;
        stats_.used_portfolio = cfg_.portfolio;
        auto last_equation_live = all_start;
        const unsigned generation_limit = generation_cost_limit();
        bool stopped_on_epsilon = false;
        double best_error = std::numeric_limits<double>::infinity();
        double best_acceptable_error = std::numeric_limits<double>::infinity();
        LiveTopReporter live_reporter(
            cfg_.target, state_value_bits(cfg_), cfg_.error_range, cfg_.live_top, cfg_.live_interval,
            [&](const Candidate& candidate) { return render_candidate_expression(atoms_, arena_, candidate); },
            [&](const Candidate& candidate) { return render_candidate_latex(atoms_, arena_, candidate); },
            cfg_.live_json, cfg_.latex,
            [&](const Candidate& candidate) {
                return constraint_satisfied(cfg_, candidate.constraint_state);
            });

        for (unsigned cost = 1; cost <= generation_limit; ++cost) {
            const auto cost_start = std::chrono::steady_clock::now();
            std::vector<GenTask> tasks = build_tasks(static_cast<std::uint16_t>(cost));
            std::vector<TaskResult> task_results(tasks.size());

            executor_.run(tasks.size(), [&](std::size_t i) {
                task_results[i] = process_task(tasks[i]);
                if (cfg_.live && !cfg_.equations) live_reporter.consider_batch(task_results[i].candidates, cost);
            });

            CandidateCollector global(cfg_.target, state_value_bits(cfg_),
                                      std::max<std::size_t>(cfg_.beam * 3, 256), cfg_.equations, 0,
                                      cfg_.pareto_slots, pareto_extra_cap(), true);
            std::vector<Candidate> atom_candidates;
            for (std::uint32_t atom_index = 0; atom_index < atoms_.size(); ++atom_index) {
                const AtomSpec& atom = atoms_[atom_index];
                if (atom.cost != cost || !valid_numeric(atom.value, cfg_)) continue;
                const auto atom_state = constraint_atom_state(cfg_, atom);
                if (!atom_state) continue;
                Candidate candidate;
                candidate.value = atom.value;
                candidate.derivative = atom.variable ? 1.0 : 0.0;
                candidate.cost = atom.cost;
                candidate.nodes = 1;
                candidate.depth = 1;
                candidate.hash = mix64(fnv1a(atom.text) ^ std::bit_cast<std::uint64_t>(atom.value));
                candidate.tag = NodeTag::Atom;
                candidate.atom_index = atom_index;
                candidate.constraint_state = *atom_state;
                candidate.depends_on_x = atom.variable;
                const auto key = state_bucket(candidate.value, candidate.derivative, candidate.depends_on_x,
                                              candidate.constraint_state, state_value_bits(cfg_), cfg_.equations);
                if (seen_.find(key) == seen_.end()) {
                    global.consider_with_key(candidate, key);
                    if (cfg_.live && !cfg_.equations) atom_candidates.push_back(candidate);
                } else {
                    global.consider_extra_only(candidate);
                }
            }
            if (cfg_.live && !cfg_.equations) live_reporter.consider_batch(atom_candidates, cost);

            CostStats& cs = stats_.by_cost[cost];
            for (const auto& result : task_results) {
                cs.attempted = saturating_add(cs.attempted, result.attempted);
                cs.valid = saturating_add(cs.valid, result.valid);
                cs.task_candidates += result.candidates.size();
                for (const auto& candidate : result.candidates) {
                    const auto key = state_bucket(candidate.value, candidate.derivative, candidate.depends_on_x,
                                                  candidate.constraint_state, state_value_bits(cfg_), cfg_.equations);
                    if (seen_.find(key) == seen_.end()) {
                        global.consider_with_key(candidate, key);
                    } else {
                        global.consider_extra_only(candidate);
                    }
                }
                for (const auto& candidate : result.extra_candidates) {
                    global.consider_extra_only(candidate);
                }
            }

            auto survivors = global.take(cfg_.beam);
            auto extra_survivors = global.take_extras(pareto_extra_cap());
            auto& layer = layers_[cost];
            layer.reserve(survivors.size());
            for (const Candidate& c : survivors) {
                const ExprId id = append_candidate_node(c);
                layer.push_back(id);
                remember_primary_hash(c.hash);
            }
            for (const Candidate& candidate : extra_survivors) append_extra_candidate(candidate);
            std::sort(layer.begin(), layer.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                return arena_[a].hash < arena_[b].hash;
            });
            auto& extra_layer = extra_layers_[cost];
            std::sort(extra_layer.begin(), extra_layer.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                return arena_[a].hash < arena_[b].hash;
            });
            for (ExprId id : layer) {
                seen_.emplace(state_bucket(arena_[id].value, arena_[id].derivative, arena_[id].depends_on_x,
                                           arena_[id].constraint_state, state_value_bits(cfg_), cfg_.equations));
                const double error = abs_error(arena_[id].value, cfg_.target);
                best_error = std::min(best_error, error);
                if (cfg_.error_range.contains(arena_[id].value - cfg_.target) &&
                    constraint_satisfied(cfg_, arena_[id].constraint_state)) {
                    best_acceptable_error = std::min(best_acceptable_error, error);
                }
            }

            cs.kept = layer.size();
            cs.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - cost_start).count();
            stats_.attempted = saturating_add(stats_.attempted, cs.attempted);
            stats_.valid = saturating_add(stats_.valid, cs.valid);
            stats_.kept += cs.kept;
            stats_.completed_cost = cost;

            if (cfg_.live && !cfg_.equations) live_reporter.snapshot(cost);
            if (cfg_.live && cfg_.equations) {
                const auto now = std::chrono::steady_clock::now();
                if (cost == generation_limit ||
                    std::chrono::duration<double>(now - last_equation_live).count() >= cfg_.live_interval) {
                    const auto matches = collect_equation_matches(cfg_, arena_, cfg_.live_top);
                    std::ostringstream out;
                    const double elapsed = std::chrono::duration<double>(now - all_start).count();
                    if (cfg_.live_json) {
                        out << "[fates-live] {\"type\":\"equations\",\"cost\":" << cost
                            << ",\"total_cost\":" << cfg_.max_cost
                            << ",\"elapsed\":" << std::setprecision(9) << elapsed
                            << ",\"capacity\":" << cfg_.live_top << ",\"results\":[";
                        for (std::size_t index = 0; index < matches.size(); ++index) {
                            const auto& match = matches[index];
                            const std::string equation =
                                render_expression_text(atoms_, arena_, match.left) + " = " +
                                render_expression_text(atoms_, arena_, match.right);
                            const std::string latex =
                                render_expression_latex_text(atoms_, arena_, match.left) + " = " +
                                render_expression_latex_text(atoms_, arena_, match.right);
                            if (index != 0) out << ',';
                            out << "{\"rank\":" << (index + 1) << ",\"cost\":" << match.cost
                                << ",\"estimated_root\":" << std::setprecision(17) << match.root
                                << ",\"signed_error\":" << (match.root - cfg_.target)
                                << ",\"absolute_error\":" << match.abs_err
                                << ",\"residual_at_target\":" << match.residual
                                << ",\"equation\":\"" << json_escape(equation)
                                << "\",\"latex\":\"" << json_escape(latex) << "\"}";
                        }
                        out << "]}\n";
                        std::cerr << out.str() << std::flush;
                    } else {
                        out << "[live-equation] ";
                    if (cfg_.bidirectional && generation_limit < cfg_.max_cost) {
                        out << "side_cost=" << cost << " total_cost=" << cfg_.max_cost;
                    } else {
                        out << "cost=" << cost;
                    }
                    out << " elapsed=" << std::fixed << std::setprecision(2)
                        << elapsed << "s top="
                        << matches.size() << '/' << cfg_.live_top << '\n';
                    for (std::size_t i = 0; i < matches.size(); ++i) {
                        const auto& match = matches[i];
                        out << "[live-equation] #" << (i + 1) << " cost=" << match.cost
                            << " root=" << std::setprecision(15) << std::defaultfloat << match.root
                            << " signed_error=" << std::scientific << std::setprecision(6)
                            << (match.root - cfg_.target) << " abs_error=" << match.abs_err
                            << " equation=";
                        if (cfg_.latex) {
                            out << render_expression_latex_text(atoms_, arena_, match.left) << " = "
                                << render_expression_latex_text(atoms_, arena_, match.right) << '\n';
                        } else {
                            out << render_expression_text(atoms_, arena_, match.left) << " = "
                                << render_expression_text(atoms_, arena_, match.right) << '\n';
                        }
                    }
                        std::cerr << out.str() << std::flush;
                    }
                    last_equation_live = now;
                }
            }

            if (cfg_.verbose) {
                std::cerr << "cost=" << cost << " tasks=" << tasks.size() << " tried=" << cs.attempted
                          << " valid=" << cs.valid << " task_kept=" << cs.task_candidates
                          << " layer_kept=" << cs.kept << " best_error=" << std::scientific << best_error
                          << " time=" << std::fixed << std::setprecision(3) << cs.seconds << "s\n";
            }

            if (!cfg_.equations && cfg_.stop_on_epsilon && best_acceptable_error <= cfg_.epsilon) {
                stopped_on_epsilon = true;
                break;
            }
            if (layer.empty() && tasks.empty()) {
                // No expressions at this cost does not imply later costs are impossible, so continue.
            }
        }

        stats_.deterministic_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - all_start).count();
        stats_.generated_cost = stats_.completed_cost;
        if (cfg_.bidirectional && !stopped_on_epsilon && generation_limit < cfg_.max_cost) {
            stats_.used_mitm = true;
            if (!cfg_.equations) {
                auto stage_start = std::chrono::steady_clock::now();
                run_terminal_mitm(live_reporter, generation_limit);
                stats_.mitm_seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - stage_start).count();
                stage_start = std::chrono::steady_clock::now();
                run_inverse_templates(live_reporter, generation_limit);
                if (stats_.used_inverse_templates) {
                    stats_.inverse_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - stage_start).count();
                }
                if (cfg_.deep_rounds > 0) {
                    stage_start = std::chrono::steady_clock::now();
                    run_deep_compositions(live_reporter, generation_limit);
                    stats_.deep_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - stage_start).count();
                }
            }
            stats_.completed_cost = cfg_.max_cost;
        }
        if (cfg_.egraph && !cfg_.equations && !stopped_on_epsilon) {
            const auto stage_start = std::chrono::steady_clock::now();
            run_egraph_search(live_reporter);
            stats_.egraph_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - stage_start).count();
        }
        if (cfg_.pslq && !cfg_.equations && !stopped_on_epsilon) {
            const auto stage_start = std::chrono::steady_clock::now();
            run_pslq_search(live_reporter);
            stats_.pslq_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - stage_start).count();
        }
        if (cfg_.mcts && !cfg_.equations && !stopped_on_epsilon) {
            const auto stage_start = std::chrono::steady_clock::now();
            run_mcts_search(live_reporter);
            stats_.mcts_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - stage_start).count();
        }
        if (cfg_.genetic && !cfg_.equations && !stopped_on_epsilon) {
            const auto stage_start = std::chrono::steady_clock::now();
            run_genetic_search(live_reporter);
            stats_.genetic_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - stage_start).count();
        }

        stats_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - all_start).count();
        return SearchRun{std::move(cfg_), std::move(atoms_), std::move(arena_), std::move(layers_),
                         std::move(extra_layers_), std::move(stats_)};
    }

private:
    std::string configured_search_mode() const {
        if (cfg_.equations) return "equations";
        std::string mode = cfg_.portfolio ? "portfolio" : "deterministic";
        if (cfg_.pslq && !cfg_.portfolio) mode += "+pslq";
        if (cfg_.egraph && !cfg_.portfolio) mode += "+egraph";
        if (cfg_.mcts && !cfg_.portfolio) mode += "+mcts";
        if (cfg_.genetic) mode += "+genetic";
        return mode;
    }

    std::size_t pareto_extra_cap() const {
        if (cfg_.pareto_slots <= 1) return 0;
        if (cfg_.pareto_extra != 0) return cfg_.pareto_extra;
        return std::max<std::size_t>(32, cfg_.beam / 4);
    }

    ExprId append_candidate_node(const Candidate& candidate, bool eligible = true) {
        if (arena_.size() >= kNoExpr) throw std::runtime_error("表达式数量超过 32 位索引上限");
        Node node;
        node.value = candidate.value;
        node.derivative = candidate.derivative;
        node.cost = candidate.cost;
        node.nodes = candidate.nodes;
        node.depth = candidate.depth;
        node.hash = candidate.hash;
        node.tag = candidate.tag;
        node.op = candidate.op;
        node.left = candidate.left;
        node.right = candidate.right;
        node.atom_index = candidate.atom_index;
        node.constraint_state = candidate.constraint_state;
        node.depends_on_x = candidate.depends_on_x;
        node.eligible = eligible;
        const ExprId id = static_cast<ExprId>(arena_.size());
        arena_.push_back(node);
        return id;
    }

    void remember_primary_hash(std::uint64_t hash) {
        if (cfg_.pareto_slots > 1) extra_seen_.emplace(hash);
    }

    bool append_extra_candidate(const Candidate& candidate) {
        if (cfg_.pareto_slots <= 1 || candidate.cost >= extra_layers_.size()) return false;
        if (!extra_seen_.emplace(candidate.hash).second) return false;
        const ExprId id = append_candidate_node(candidate);
        extra_layers_[candidate.cost].push_back(id);
        ++stats_.pareto_extras;
        ++stats_.kept;
        return true;
    }

    unsigned generation_cost_limit() const {
        if (!cfg_.bidirectional) return cfg_.max_cost;
        if (!cfg_.equations && binary_ops_.empty()) return cfg_.max_cost;
        const unsigned automatic = std::max(4U, (cfg_.max_cost + 1U) / 2U);
        return std::min(cfg_.max_cost, cfg_.side_cost == 0 ? automatic : cfg_.side_cost);
    }

    ExprId append_auxiliary(const Candidate& candidate,
                            FastMap<std::uint64_t, ExprId>& auxiliary_ids) {
        if (const auto it = auxiliary_ids.find(candidate.hash); it != auxiliary_ids.end()) return it->second;
        if (arena_.size() >= kNoExpr) throw std::runtime_error("表达式数量超过 32 位索引上限");
        Node node;
        node.value = candidate.value;
        node.derivative = candidate.derivative;
        node.cost = candidate.cost;
        node.nodes = candidate.nodes;
        node.depth = candidate.depth;
        node.hash = candidate.hash;
        node.tag = candidate.tag;
        node.op = candidate.op;
        node.left = candidate.left;
        node.right = candidate.right;
        node.atom_index = candidate.atom_index;
        node.constraint_state = candidate.constraint_state;
        node.depends_on_x = candidate.depends_on_x;
        node.eligible = false;
        const ExprId id = static_cast<ExprId>(arena_.size());
        arena_.push_back(node);
        auxiliary_ids.emplace(candidate.hash, id);
        return id;
    }

    double nearest_layer_score(double desired, unsigned exact_cost) const {
        if (!std::isfinite(desired) || exact_cost >= layers_.size() || layers_[exact_cost].empty()) {
            return std::numeric_limits<double>::infinity();
        }
        const auto& ids = layers_[exact_cost];
        const std::size_t pos = lower_bound_value(ids, arena_, desired);
        double score = std::numeric_limits<double>::infinity();
        const auto consider = [&](std::size_t index) {
            score = std::min(score, std::abs(arena_[ids[index]].value - desired) /
                                     std::max(1.0, std::abs(desired)));
        };
        if (pos < ids.size()) consider(pos);
        if (pos > 0) consider(pos - 1);
        return score;
    }

    struct UnaryInverseValues {
        std::array<double, 2> values{};
        std::size_t count{};

        bool empty() const { return count == 0; }
        std::size_t size() const { return count; }
        double operator[](std::size_t index) const { return values[index]; }
        const double* begin() const { return values.data(); }
        const double* end() const { return values.data() + count; }

        void add(double value) {
            for (std::size_t i = 0; i < count; ++i) {
                if (values[i] == value) return;
            }
            if (count < values.size()) values[count++] = value;
        }
    };

    UnaryInverseValues inverse_unary_values(UnaryKind kind, double target) const {
        UnaryInverseValues out;
        const auto add = [&](double value) {
            if (valid_numeric(value, cfg_)) out.add(value);
        };
        switch (kind) {
            case UnaryKind::Neg: add(-target); break;
            case UnaryKind::Inv:
                if (target != 0.0) add(1.0 / target);
                break;
            case UnaryKind::Sqrt:
                if (target >= 0.0) add(target * target);
                break;
            case UnaryKind::Cbrt: add(target * target * target); break;
            case UnaryKind::Sqr:
                if (target >= 0.0) {
                    add(std::sqrt(target));
                    add(-std::sqrt(target));
                }
                break;
            case UnaryKind::Cube: add(std::cbrt(target)); break;
            case UnaryKind::Ln: add(std::exp(target)); break;
            case UnaryKind::Log10: add(std::pow(10.0, target)); break;
            case UnaryKind::Exp:
                if (target > 0.0) add(std::log(target));
                break;
            case UnaryKind::Sin:
                if (target >= -1.0 && target <= 1.0) add(std::asin(target));
                break;
            case UnaryKind::Cos:
                if (target >= -1.0 && target <= 1.0) add(std::acos(target));
                break;
            case UnaryKind::Tan: add(std::atan(target)); break;
            case UnaryKind::Asin: add(std::sin(target)); break;
            case UnaryKind::Acos: add(std::cos(target)); break;
            case UnaryKind::Atan: add(std::tan(target)); break;
            case UnaryKind::Sinh: add(std::asinh(target)); break;
            case UnaryKind::Cosh:
                if (target >= 1.0) {
                    add(std::acosh(target));
                    add(-std::acosh(target));
                }
                break;
            case UnaryKind::Tanh:
                if (target > -1.0 && target < 1.0) add(std::atanh(target));
                break;
            case UnaryKind::Asinh: add(std::sinh(target)); break;
            case UnaryKind::Acosh:
                if (target >= 0.0) add(std::cosh(target));
                break;
            case UnaryKind::Atanh: add(std::tanh(target)); break;
            case UnaryKind::Gamma:
                // Γ is non-monotonic and multi-valued over the reals.  It is
                // searched normally, but deliberately omitted from inverse
                // templates instead of returning an incomplete fake inverse.
                break;
            case UnaryKind::Abs:
                if (target >= 0.0) {
                    add(target);
                    add(-target);
                }
                break;
            case UnaryKind::Fact: break;
            default:
                if (const auto custom = custom_unary_index(kind)) {
                    const auto& operation = extension_registry().unary_operations()[*custom];
                    if (operation.inverse) {
                        for (const double value : operation.inverse(target, extension_limits(cfg_))) add(value);
                    }
                }
                break;
        }
        return out;
    }

    std::vector<Candidate> synthesize_binary_value(double desired,
                                                   unsigned exact_cost) {
        struct SynthesisPartition {
            std::size_t op_index{};
            unsigned left_cost{};
            unsigned right_cost{};
        };
        std::vector<SynthesisPartition> partitions;
        for (std::size_t op_index = 0; op_index < binary_ops_.size(); ++op_index) {
            const BinarySpec& op = binary_ops_[op_index];
            if (static_cast<unsigned>(op.cost) + 2U > exact_cost) continue;
            const unsigned remainder = exact_cost - op.cost;
            for (unsigned left_cost = 1; left_cost < remainder; ++left_cost) {
                const unsigned right_cost = remainder - left_cost;
                if (left_cost >= layers_.size() || right_cost >= layers_.size() ||
                    layers_[left_cost].empty() || layers_[right_cost].empty()) {
                    continue;
                }
                partitions.push_back({op_index, left_cost, right_cost});
            }
        }

        std::vector<TaskResult> results(partitions.size());
        executor_.run(partitions.size(), [&](std::size_t partition_index) {
            const SynthesisPartition& partition = partitions[partition_index];
            const BinarySpec& op = binary_ops_[partition.op_index];
            const auto& left_ids = layers_[partition.left_cost];
            const auto& right_ids = layers_[partition.right_cost];
            CandidateCollector local(desired, state_value_bits(cfg_), 64, false);
            TaskResult& result = results[partition_index];
            std::vector<std::size_t> nearby_indices;
            nearby_indices.reserve(5);
            for (ExprId left_id : left_ids) {
                const auto desired_right_value = desired_right(op.kind, arena_[left_id].value, desired);
                if (!desired_right_value) continue;
                nearby_indices.clear();
                add_window_indices(nearby_indices, right_ids, arena_, *desired_right_value, 0,
                                   cfg_.inverse_neighbors);
                for (std::size_t index : nearby_indices) {
                    ++result.attempted;
                    const auto candidate = apply_binary(cfg_, arena_, op, left_id, right_ids[index],
                                                        static_cast<std::uint16_t>(exact_cost));
                    if (!candidate) continue;
                    ++result.valid;
                    local.consider(*candidate);
                }
            }
            result.candidates = local.take(24);
        });

        CandidateCollector collector(desired, state_value_bits(cfg_), 96, false);
        for (const TaskResult& result : results) {
            stats_.attempted = saturating_add(stats_.attempted, result.attempted);
            stats_.valid = saturating_add(stats_.valid, result.valid);
            stats_.by_cost[exact_cost].attempted =
                saturating_add(stats_.by_cost[exact_cost].attempted, result.attempted);
            stats_.by_cost[exact_cost].valid =
                saturating_add(stats_.by_cost[exact_cost].valid, result.valid);
            for (const Candidate& candidate : result.candidates) collector.consider(candidate);
        }
        return collector.take(24);
    }

    std::vector<Candidate> synthesize_unary_binary(
        double desired,
        unsigned exact_cost,
        FastMap<std::uint64_t, ExprId>& auxiliary_ids,
        FastMap<std::uint64_t, std::vector<Candidate>>& cache) {
        const std::uint64_t cache_key = mix64(value_bucket(desired, 42) ^
                                              (static_cast<std::uint64_t>(exact_cost) << 48U));
        if (const auto it = cache.find(cache_key); it != cache.end()) return it->second;

        CandidateCollector collector(desired, state_value_bits(cfg_), 64, false);
        for (const UnarySpec& unary : unary_ops_) {
            if (static_cast<unsigned>(unary.cost) + 3U > exact_cost) continue;
            const unsigned child_cost = exact_cost - unary.cost;
            for (double desired_child : inverse_unary_values(unary.kind, desired)) {
                for (const Candidate& child : synthesize_binary_value(desired_child, child_cost)) {
                    const ExprId child_id = append_auxiliary(child, auxiliary_ids);
                    ++stats_.attempted;
                    ++stats_.by_cost[exact_cost].attempted;
                    const auto candidate = apply_unary(cfg_, arena_, unary, child_id,
                                                       static_cast<std::uint16_t>(exact_cost));
                    if (!candidate) continue;
                    ++stats_.valid;
                    ++stats_.by_cost[exact_cost].valid;
                    collector.consider(*candidate);
                }
            }
        }
        auto result = collector.take(12);
        cache.emplace(cache_key, result);
        return result;
    }

    Candidate candidate_from_node(ExprId id) const {
        const Node& node = arena_[id];
        Candidate candidate;
        candidate.value = node.value;
        candidate.derivative = node.derivative;
        candidate.cost = node.cost;
        candidate.nodes = node.nodes;
        candidate.depth = node.depth;
        candidate.hash = node.hash;
        candidate.tag = node.tag;
        candidate.op = node.op;
        candidate.left = node.left;
        candidate.right = node.right;
        candidate.atom_index = node.atom_index;
        candidate.constraint_state = node.constraint_state;
        candidate.depends_on_x = node.depends_on_x;
        return candidate;
    }

    std::vector<std::vector<ExprId>> extended_layers_snapshot() const {
        std::vector<std::vector<ExprId>> snapshot(layers_.size());
        for (std::size_t cost = 1; cost < layers_.size(); ++cost) {
            auto& out = snapshot[cost];
            out.reserve(layers_[cost].size() + extra_layers_[cost].size());
            out.insert(out.end(), layers_[cost].begin(), layers_[cost].end());
            out.insert(out.end(), extra_layers_[cost].begin(), extra_layers_[cost].end());
            std::sort(out.begin(), out.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                if (arena_[a].nodes != arena_[b].nodes) return arena_[a].nodes < arena_[b].nodes;
                return arena_[a].hash < arena_[b].hash;
            });
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
        return snapshot;
    }

    const UnarySpec* find_unary_operation(UnaryKind kind) const {
        const auto found = std::find_if(unary_ops_.begin(), unary_ops_.end(),
                                        [&](const UnarySpec& spec) { return spec.kind == kind; });
        return found == unary_ops_.end() ? nullptr : &*found;
    }

    const BinarySpec* find_binary_operation(BinaryKind kind) const {
        const auto found = std::find_if(binary_ops_.begin(), binary_ops_.end(),
                                        [&](const BinarySpec& spec) { return spec.kind == kind; });
        return found == binary_ops_.end() ? nullptr : &*found;
    }

    FastMap<std::uint64_t, ExprId> build_structure_index() const {
        FastMap<std::uint64_t, ExprId> index;
        index.reserve(arena_.size() * 2 + 1);
        for (ExprId id = 0; id < arena_.size(); ++id) {
            const Node& candidate = arena_[id];
            const auto found = index.find(candidate.hash);
            if (found == index.end()) {
                index.emplace(candidate.hash, id);
                continue;
            }
            const Node& current = arena_[found->second];
            if ((!current.eligible && candidate.eligible) ||
                (current.eligible == candidate.eligible &&
                 std::pair{candidate.cost, candidate.nodes} < std::pair{current.cost, current.nodes})) {
                found->second = id;
            }
        }
        return index;
    }

    ExprId materialize_candidate(const Candidate& candidate,
                                 FastMap<std::uint64_t, ExprId>& structure_index,
                                 std::vector<unsigned char>& touched_cost,
                                 bool eligible,
                                 bool prune_by_value,
                                 bool* newly_eligible = nullptr) {
        if (newly_eligible) *newly_eligible = false;
        if (candidate.cost == 0 || candidate.cost > cfg_.max_cost) return kNoExpr;

        if (const auto found = structure_index.find(candidate.hash); found != structure_index.end()) {
            const ExprId id = found->second;
            Node& node = arena_[id];
            if (eligible && !node.eligible) {
                node.eligible = true;
                layers_[node.cost].push_back(id);
                touched_cost[node.cost] = 1;
                seen_.emplace(state_bucket(node.value, node.derivative, node.depends_on_x,
                                           node.constraint_state, state_value_bits(cfg_), false));
                ++stats_.kept;
                if (newly_eligible) *newly_eligible = true;
            }
            return id;
        }

        const std::uint64_t value_key = state_bucket(
            candidate.value, candidate.derivative, candidate.depends_on_x,
            candidate.constraint_state, state_value_bits(cfg_), false);
        if (eligible && prune_by_value && seen_.find(value_key) != seen_.end()) return kNoExpr;

        const ExprId id = append_candidate_node(candidate, eligible);
        structure_index.emplace(candidate.hash, id);
        if (eligible) {
            layers_[candidate.cost].push_back(id);
            touched_cost[candidate.cost] = 1;
            seen_.emplace(value_key);
            ++stats_.kept;
            if (newly_eligible) *newly_eligible = true;
        }
        return id;
    }

    void sort_touched_layers(const std::vector<unsigned char>& touched_cost) {
        for (unsigned cost = 1; cost < layers_.size(); ++cost) {
            if (!touched_cost[cost]) continue;
            auto& ids = layers_[cost];
            std::sort(ids.begin(), ids.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                if (arena_[a].nodes != arena_[b].nodes) return arena_[a].nodes < arena_[b].nodes;
                if (arena_[a].hash != arena_[b].hash) return arena_[a].hash < arena_[b].hash;
                return a < b;
            });
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            stats_.by_cost[cost].kept = ids.size();
        }
    }

    static bool equivalent_value(double left, double right) {
        const double scale = std::max({1.0, std::abs(left), std::abs(right)});
        return std::abs(left - right) <= 128.0 * std::numeric_limits<double>::epsilon() * scale;
    }

    double nearest_value_score(const std::vector<ExprId>& ids, double desired) const {
        if (ids.empty() || !std::isfinite(desired)) return std::numeric_limits<double>::infinity();
        const std::size_t position = lower_bound_value(ids, arena_, desired);
        double best = std::numeric_limits<double>::infinity();
        if (position < ids.size()) best = std::abs(arena_[ids[position]].value - desired);
        if (position > 0) best = std::min(best, std::abs(arena_[ids[position - 1]].value - desired));
        return best / std::max(1.0, std::abs(desired));
    }

    std::vector<double> desired_hole_values(BinaryKind kind,
                                            double anchor,
                                            double desired,
                                            bool hole_on_left) const {
        std::vector<double> values;
        const auto primary = hole_on_left ? desired_left(kind, anchor, desired)
                                          : desired_right(kind, anchor, desired);
        if (primary && std::isfinite(*primary)) values.push_back(*primary);
        if (hole_on_left && kind == BinaryKind::Pow && desired > 0.0) {
            const double rounded = std::nearbyint(anchor);
            const bool even_integer = std::abs(anchor - rounded) <=
                    1.0e-12 * std::max(1.0, std::abs(anchor)) &&
                std::fmod(std::abs(rounded), 2.0) == 0.0 && rounded != 0.0;
            if (even_integer && primary && *primary > 0.0) values.push_back(-*primary);
        }
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }

    std::vector<Candidate> solve_inverse_value(
        double desired,
        unsigned exact_cost,
        unsigned depth,
        const std::vector<std::vector<ExprId>>& source_layers,
        FastMap<std::uint64_t, ExprId>& auxiliary_ids,
        FastMap<std::uint64_t, std::vector<Candidate>>& cache,
        std::uint64_t& remaining_budget) {
        if (!std::isfinite(desired) || exact_cost == 0 || exact_cost >= source_layers.size()) return {};
        const std::uint64_t cache_key = mix64(
            std::bit_cast<std::uint64_t>(desired) ^
            (static_cast<std::uint64_t>(exact_cost) << 32U) ^
            (static_cast<std::uint64_t>(depth) << 56U));
        if (const auto found = cache.find(cache_key); found != cache.end()) return found->second;

        const std::size_t local_cap = std::max<std::size_t>(8, cfg_.inverse_beam);
        CandidateCollector collector(desired, state_value_bits(cfg_), local_cap, false, 0,
                                     cfg_.pareto_slots, std::max<std::size_t>(4, local_cap / 4));
        const auto& exact_layer = source_layers[exact_cost];
        std::vector<std::size_t> indices;
        indices.reserve(cfg_.inverse_neighbors + 16);
        add_near_indices(indices, exact_layer, arena_, desired, 0,
                         std::min(local_cap, std::max<std::size_t>(cfg_.inverse_neighbors, 8)));
        for (std::size_t index : indices) collector.consider(candidate_from_node(exact_layer[index]));

        if (depth > 0 && remaining_budget > 0) {
            for (const UnarySpec& unary : unary_ops_) {
                if (unary.cost >= exact_cost) continue;
                const unsigned child_cost = exact_cost - unary.cost;
                for (double desired_child : inverse_unary_values(unary.kind, desired)) {
                    for (const Candidate& child : solve_inverse_value(
                             desired_child, child_cost, depth - 1, source_layers,
                             auxiliary_ids, cache, remaining_budget)) {
                        if (remaining_budget == 0) break;
                        const ExprId child_id = append_auxiliary(child, auxiliary_ids);
                        --remaining_budget;
                        ++stats_.attempted;
                        ++stats_.by_cost[exact_cost].attempted;
                        const auto candidate = apply_unary(
                            cfg_, arena_, unary, child_id, static_cast<std::uint16_t>(exact_cost));
                        if (!candidate) continue;
                        ++stats_.valid;
                        ++stats_.by_cost[exact_cost].valid;
                        collector.consider(*candidate);
                    }
                }
            }

            for (const BinarySpec& binary : binary_ops_) {
                if (static_cast<unsigned>(binary.cost) + 2U > exact_cost || remaining_budget == 0) continue;
                const unsigned remainder = exact_cost - binary.cost;
                for (unsigned left_cost = 1; left_cost < remainder && remaining_budget > 0; ++left_cost) {
                    const unsigned right_cost = remainder - left_cost;
                    if (is_commutative(binary.kind) && left_cost > right_cost) continue;
                    const auto& left_ids = source_layers[left_cost];
                    const auto& right_ids = source_layers[right_cost];
                    if (left_ids.empty() || right_ids.empty()) continue;

                    indices.clear();
                    for (ExprId left_id : left_ids) {
                        if (remaining_budget == 0) break;
                        const auto wanted = desired_right(binary.kind, arena_[left_id].value, desired);
                        if (!wanted || !std::isfinite(*wanted)) continue;
                        indices.clear();
                        add_near_indices(indices, right_ids, arena_, *wanted, 0, cfg_.inverse_neighbors);
                        if (binary.kind == BinaryKind::Pow) {
                            static constexpr double useful_exponents[] = {-3.0, -2.0, -1.0, -0.5,
                                                                          0.5, 2.0, 3.0};
                            for (double exponent : useful_exponents) {
                                add_near_indices(indices, right_ids, arena_, exponent, 0,
                                                 std::min<std::size_t>(3, cfg_.inverse_neighbors));
                            }
                            std::sort(indices.begin(), indices.end());
                            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
                        }
                        for (std::size_t index : indices) {
                            if (remaining_budget == 0) break;
                            --remaining_budget;
                            ++stats_.attempted;
                            ++stats_.by_cost[exact_cost].attempted;
                            const auto candidate = apply_binary(
                                cfg_, arena_, binary, left_id, right_ids[index],
                                static_cast<std::uint16_t>(exact_cost));
                            if (!candidate) continue;
                            ++stats_.valid;
                            ++stats_.by_cost[exact_cost].valid;
                            collector.consider(*candidate);
                        }
                    }

                    if (depth <= 1 || remaining_budget == 0) continue;
                    struct Request {
                        ExprId anchor{kNoExpr};
                        bool hole_on_left{};
                        double desired_hole{};
                        double score{};
                    };
                    const std::size_t request_cap = std::max<std::size_t>(
                        1, std::min<std::size_t>(4, cfg_.inverse_beam / 16));
                    std::vector<Request> requests;
                    const auto collect_requests = [&](const std::vector<ExprId>& anchors,
                                                      const std::vector<ExprId>& hole_layer,
                                                      bool hole_on_left) {
                        std::vector<Request> group;
                        group.reserve(anchors.size());
                        for (ExprId anchor_id : anchors) {
                            for (double wanted : desired_hole_values(
                                     binary.kind, arena_[anchor_id].value, desired, hole_on_left)) {
                                group.push_back({anchor_id, hole_on_left, wanted,
                                                 nearest_value_score(hole_layer, wanted)});
                            }
                        }
                        std::sort(group.begin(), group.end(), [&](const Request& a, const Request& b) {
                            if (a.score != b.score) return a.score < b.score;
                            const Node& na = arena_[a.anchor];
                            const Node& nb = arena_[b.anchor];
                            if (na.nodes != nb.nodes) return na.nodes < nb.nodes;
                            if (na.depth != nb.depth) return na.depth < nb.depth;
                            return na.hash < nb.hash;
                        });
                        if (group.size() > request_cap) group.resize(request_cap);
                        requests.insert(requests.end(), group.begin(), group.end());
                    };
                    collect_requests(left_ids, right_ids, false);
                    if (!is_commutative(binary.kind)) collect_requests(right_ids, left_ids, true);

                    for (const Request& request : requests) {
                        const unsigned hole_cost = request.hole_on_left ? left_cost : right_cost;
                        for (const Candidate& hole : solve_inverse_value(
                                 request.desired_hole, hole_cost, depth - 1, source_layers,
                                 auxiliary_ids, cache, remaining_budget)) {
                            if (remaining_budget == 0) break;
                            const ExprId hole_id = append_auxiliary(hole, auxiliary_ids);
                            --remaining_budget;
                            ++stats_.attempted;
                            ++stats_.by_cost[exact_cost].attempted;
                            const auto candidate = request.hole_on_left
                                ? apply_binary(cfg_, arena_, binary, hole_id, request.anchor,
                                               static_cast<std::uint16_t>(exact_cost))
                                : apply_binary(cfg_, arena_, binary, request.anchor, hole_id,
                                               static_cast<std::uint16_t>(exact_cost));
                            if (!candidate) continue;
                            ++stats_.valid;
                            ++stats_.by_cost[exact_cost].valid;
                            collector.consider(*candidate);
                        }
                    }
                }
            }
        }

        std::vector<Candidate> result = collector.take(local_cap);
        auto extras = collector.take_extras(std::max<std::size_t>(4, local_cap / 4));
        result.insert(result.end(), extras.begin(), extras.end());
        std::sort(result.begin(), result.end(), [&](const Candidate& a, const Candidate& b) {
            const double ea = abs_error(a.value, desired);
            const double eb = abs_error(b.value, desired);
            if (ea != eb) return ea < eb;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            if (a.depth != b.depth) return a.depth < b.depth;
            return a.hash < b.hash;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const Candidate& a, const Candidate& b) {
            return a.hash == b.hash;
        }), result.end());
        if (result.size() > local_cap) result.resize(local_cap);
        cache.emplace(cache_key, result);
        return result;
    }

    void run_inverse_templates(LiveTopReporter& live_reporter, unsigned side_cost) {
        if (cfg_.inverse_depth == 0 || cfg_.inverse_budget == 0) return;
        stats_.used_inverse_templates = true;
        const auto started = std::chrono::steady_clock::now();
        const auto source_layers = extended_layers_snapshot();
        FastMap<std::uint64_t, ExprId> auxiliary_ids;
        FastMap<std::uint64_t, std::vector<Candidate>> cache;
        FastSet<std::uint64_t> archive_hashes;
        auxiliary_ids.reserve(cfg_.inverse_beam * cfg_.inverse_depth * 16 + 1);
        cache.reserve(cfg_.inverse_beam * cfg_.inverse_depth * 8 + 1);
        archive_hashes.reserve(arena_.size() * 2 + 1);
        for (const Node& node : arena_) {
            if (node.eligible) archive_hashes.emplace(node.hash);
        }

        std::uint64_t remaining_budget = cfg_.inverse_budget;
        std::uint64_t attempted_budget = 0;
        std::size_t appended = 0;
        std::vector<Candidate> live_candidates;
        for (unsigned cost = side_cost + 1; cost <= cfg_.max_cost && remaining_budget > 0; ++cost) {
            const std::uint64_t costs_left = cfg_.max_cost - cost + 1ULL;
            const std::uint64_t cost_budget = std::max<std::uint64_t>(1, remaining_budget / costs_left);
            std::uint64_t cost_remaining = cost_budget;
            cache.clear();
            for (const Candidate& candidate : solve_inverse_value(
                     cfg_.target, cost, cfg_.inverse_depth, source_layers,
                     auxiliary_ids, cache, cost_remaining)) {
                if (!archive_hashes.emplace(candidate.hash).second) continue;
                const ExprId id = append_candidate_node(candidate);
                extra_layers_[cost].push_back(id);
                extra_seen_.emplace(candidate.hash);
                ++appended;
                ++stats_.inverse_candidates;
                ++stats_.kept;
                if (cfg_.live) live_candidates.push_back(candidate);
            }
            const std::uint64_t consumed = cost_budget - cost_remaining;
            attempted_budget += consumed;
            remaining_budget -= consumed;
            auto& layer = extra_layers_[cost];
            std::sort(layer.begin(), layer.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                return arena_[a].hash < arena_[b].hash;
            });
        }
        if (cfg_.live) {
            live_reporter.consider_batch(live_candidates, cfg_.max_cost);
            live_reporter.snapshot(cfg_.max_cost);
        }
        if (cfg_.verbose) {
            std::cerr << "inverse_depth=" << cfg_.inverse_depth << " appended=" << appended
                      << " attempted=" << attempted_budget
                      << " time=" << std::fixed << std::setprecision(3)
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                      << "s\n";
        }
    }

    void run_deep_unbalanced_mitm(std::map<unsigned, CandidateCollector>& collectors,
                                  unsigned side_cost,
                                  FastMap<std::uint64_t, ExprId>& auxiliary_ids) {
        struct TopRequest {
            std::size_t op_index{};
            ExprId anchor{kNoExpr};
            bool virtual_on_left{};
            double desired{};
            unsigned max_virtual_cost{};
            double score{};
        };
        struct ChildRequest {
            TopRequest top;
            std::size_t inner_op_index{};
            ExprId inner_anchor{kNoExpr};
            bool anchor_on_left{};
            double desired_child{};
            unsigned child_cost{};
            unsigned virtual_cost{};
            double score{};
        };

        std::vector<TopRequest> selected_top;
        for (std::size_t op_index = 0; op_index < binary_ops_.size(); ++op_index) {
            const BinarySpec& op = binary_ops_[op_index];
            const unsigned directions = is_commutative(op.kind) ? 1U : 2U;
            for (unsigned direction = 0; direction < directions; ++direction) {
                std::vector<TopRequest> group;
                const bool virtual_on_left = direction == 0;
                if (op.kind != BinaryKind::Pow || !virtual_on_left) continue;
                for (ExprId anchor_id : layers_[1]) {
                    const Node& anchor = arena_[anchor_id];
                    if ((op.kind == BinaryKind::Mul && (anchor.value == 0.0 || anchor.value == 1.0)) ||
                        (op.kind == BinaryKind::Pow && anchor.value == 1.0)) {
                        continue;
                    }
                    const double rounded_exponent = std::nearbyint(anchor.value);
                    if (std::abs(anchor.value - rounded_exponent) > 1.0e-12 ||
                        std::abs(rounded_exponent) < 2.0 || std::abs(rounded_exponent) > 9.0) {
                        continue;
                    }
                    if (static_cast<unsigned>(anchor.cost) + op.cost >= cfg_.max_cost) continue;
                    const unsigned max_virtual_cost = cfg_.max_cost - anchor.cost - op.cost;
                    if (max_virtual_cost <= side_cost) continue;
                    const auto desired = virtual_on_left
                        ? desired_left(op.kind, anchor.value, cfg_.target)
                        : desired_right(op.kind, anchor.value, cfg_.target);
                    if (!desired) continue;
                    double score = std::numeric_limits<double>::infinity();
                    for (unsigned cost = 1; cost <= side_cost; ++cost) {
                        score = std::min(score, nearest_layer_score(*desired, cost));
                    }
                    group.push_back({op_index, anchor_id, virtual_on_left, *desired,
                                     max_virtual_cost, score});
                }
                std::sort(group.begin(), group.end(), [](const TopRequest& a, const TopRequest& b) {
                    if (a.score != b.score) return a.score < b.score;
                    return a.anchor < b.anchor;
                });
                selected_top.insert(selected_top.end(), group.begin(), group.end());
            }
        }

        std::vector<ChildRequest> selected_children;
        for (const TopRequest& top : selected_top) {
            std::vector<ChildRequest> group;
            for (std::size_t inner_index = 0; inner_index < binary_ops_.size(); ++inner_index) {
                const BinarySpec& inner = binary_ops_[inner_index];
                const unsigned directions = is_commutative(inner.kind) ? 1U : 2U;
                for (unsigned direction = 0; direction < directions; ++direction) {
                    const bool anchor_on_left = direction == 0;
                    for (ExprId anchor_id : layers_[1]) {
                        const Node& anchor = arena_[anchor_id];
                        if ((inner.kind == BinaryKind::Mul &&
                             (anchor.value == 0.0 || anchor.value == 1.0)) ||
                            (inner.kind == BinaryKind::Pow && anchor.value == 1.0) ||
                            (inner.kind == BinaryKind::Div && !anchor_on_left && anchor.value == 1.0)) {
                            continue;
                        }
                        const auto desired_child = anchor_on_left
                            ? desired_right(inner.kind, anchor.value, top.desired)
                            : desired_left(inner.kind, anchor.value, top.desired);
                        if (!desired_child) continue;
                        for (unsigned child_cost = 4; child_cost <= side_cost; ++child_cost) {
                            const unsigned virtual_cost = anchor.cost + child_cost + inner.cost;
                            if (virtual_cost <= side_cost || virtual_cost > top.max_virtual_cost) continue;
                            group.push_back({top, inner_index, anchor_id, anchor_on_left, *desired_child,
                                             child_cost, virtual_cost,
                                             nearest_layer_score(*desired_child, child_cost)});
                        }
                    }
                }
            }
            std::sort(group.begin(), group.end(), [](const ChildRequest& a, const ChildRequest& b) {
                if (a.score != b.score) return a.score < b.score;
                if (a.child_cost != b.child_cost) return a.child_cost < b.child_cost;
                return a.inner_anchor < b.inner_anchor;
            });
            std::vector<ChildRequest> diverse;
            for (std::size_t op_index = 0; op_index < binary_ops_.size(); ++op_index) {
                for (unsigned direction = 0; direction < 2; ++direction) {
                    const bool anchor_on_left = direction == 0;
                    if (direction == 1 && is_commutative(binary_ops_[op_index].kind)) continue;
                    std::size_t added = 0;
                    for (const ChildRequest& request : group) {
                        if (request.inner_op_index != op_index ||
                            request.anchor_on_left != anchor_on_left) {
                            continue;
                        }
                        diverse.push_back(request);
                        if (++added == 1) break;
                    }
                }
            }
            if (binary_ops_[top.op_index].kind == BinaryKind::Pow && top.virtual_on_left) {
                const ChildRequest* normalized = nullptr;
                double normalized_score = std::numeric_limits<double>::infinity();
                for (const ChildRequest& request : group) {
                    if (binary_ops_[request.inner_op_index].kind != BinaryKind::Mul) {
                        continue;
                    }
                    const double score = std::min({std::abs(request.desired_child),
                                                   std::abs(request.desired_child - 1.0),
                                                   std::abs(request.desired_child + 1.0)});
                    if (score < normalized_score) {
                        normalized = &request;
                        normalized_score = score;
                    }
                }
                if (normalized != nullptr) {
                    const bool already_selected = std::any_of(
                        diverse.begin(), diverse.end(), [&](const ChildRequest& selected) {
                            return selected.inner_op_index == normalized->inner_op_index &&
                                   selected.inner_anchor == normalized->inner_anchor &&
                                   selected.anchor_on_left == normalized->anchor_on_left &&
                                   selected.child_cost == normalized->child_cost;
                        });
                    if (!already_selected) diverse.push_back(*normalized);
                }
            }
            group = std::move(diverse);
            selected_children.insert(selected_children.end(), group.begin(), group.end());
        }

        FastMap<std::uint64_t, std::vector<Candidate>> synthesis_cache;
        for (const ChildRequest& request : selected_children) {
            const BinarySpec& inner = binary_ops_[request.inner_op_index];
            const BinarySpec& top = binary_ops_[request.top.op_index];
            for (const Candidate& child : synthesize_unary_binary(
                     request.desired_child, request.child_cost, auxiliary_ids, synthesis_cache)) {
                const ExprId child_id = append_auxiliary(child, auxiliary_ids);
                ++stats_.attempted;
                ++stats_.by_cost[request.virtual_cost].attempted;
                const auto virtual_candidate = request.anchor_on_left
                    ? apply_binary(cfg_, arena_, inner, request.inner_anchor, child_id,
                                   static_cast<std::uint16_t>(request.virtual_cost))
                    : apply_binary(cfg_, arena_, inner, child_id, request.inner_anchor,
                                   static_cast<std::uint16_t>(request.virtual_cost));
                if (!virtual_candidate) continue;
                ++stats_.valid;
                ++stats_.by_cost[request.virtual_cost].valid;
                const ExprId virtual_id = append_auxiliary(*virtual_candidate, auxiliary_ids);
                const unsigned total_cost = request.virtual_cost + arena_[request.top.anchor].cost + top.cost;
                if (total_cost > cfg_.max_cost) continue;
                ++stats_.attempted;
                ++stats_.by_cost[total_cost].attempted;
                const auto final_candidate = request.top.virtual_on_left
                    ? apply_binary(cfg_, arena_, top, virtual_id, request.top.anchor,
                                   static_cast<std::uint16_t>(total_cost))
                    : apply_binary(cfg_, arena_, top, request.top.anchor, virtual_id,
                                   static_cast<std::uint16_t>(total_cost));
                if (!final_candidate) continue;
                ++stats_.valid;
                ++stats_.by_cost[total_cost].valid;
                auto [collector_it, _] = collectors.try_emplace(
                    total_cost, cfg_.target, state_value_bits(cfg_),
                    std::max<std::size_t>(cfg_.beam * 3, 256), false, 0,
                    cfg_.pareto_slots, pareto_extra_cap());
                collector_it->second.consider(*final_candidate);
            }
        }
    }

    void run_unbalanced_mitm(std::map<unsigned, CandidateCollector>& collectors,
                             unsigned side_cost) {
        if (layers_.size() <= 1 || layers_[1].empty()) return;
        const auto& anchors = layers_[1];
        FastMap<std::uint64_t, ExprId> auxiliary_ids;
        auxiliary_ids.reserve(4096);

        for (std::size_t top_index = 0; top_index < binary_ops_.size(); ++top_index) {
            const auto& top_op = binary_ops_[top_index];
            const unsigned top_directions = is_commutative(top_op.kind) ? 1U : 2U;
            for (unsigned top_direction = 0; top_direction < top_directions; ++top_direction) {
                const bool virtual_on_left = top_direction == 0;
                for (ExprId top_anchor_id : anchors) {
                    const unsigned top_anchor_cost = arena_[top_anchor_id].cost;
                    const double top_anchor_value = arena_[top_anchor_id].value;
                    if (top_anchor_cost + top_op.cost >= cfg_.max_cost) continue;
                    const unsigned max_virtual_cost = cfg_.max_cost - top_anchor_cost - top_op.cost;
                    if (max_virtual_cost <= side_cost) continue;
                    const auto desired_virtual = virtual_on_left
                        ? desired_left(top_op.kind, top_anchor_value, cfg_.target)
                        : desired_right(top_op.kind, top_anchor_value, cfg_.target);
                    if (!desired_virtual || !std::isfinite(*desired_virtual)) continue;

                    CandidateCollector virtuals(*desired_virtual, state_value_bits(cfg_), 96, false);
                    for (std::size_t inner_index = 0; inner_index < binary_ops_.size(); ++inner_index) {
                        const auto& inner_op = binary_ops_[inner_index];
                        const unsigned inner_directions = is_commutative(inner_op.kind) ? 1U : 2U;
                        for (unsigned inner_direction = 0; inner_direction < inner_directions; ++inner_direction) {
                            const bool anchor_on_left = inner_direction == 0;
                            for (ExprId inner_anchor_id : anchors) {
                                const Node& inner_anchor = arena_[inner_anchor_id];
                                const auto desired_child = anchor_on_left
                                    ? desired_right(inner_op.kind, inner_anchor.value, *desired_virtual)
                                    : desired_left(inner_op.kind, inner_anchor.value, *desired_virtual);
                                if (!desired_child || !std::isfinite(*desired_child)) continue;

                                std::vector<std::size_t> nearby_indices;
                                nearby_indices.reserve(5);
                                for (unsigned child_cost = 1; child_cost <= side_cost; ++child_cost) {
                                    const unsigned virtual_cost = inner_anchor.cost + child_cost + inner_op.cost;
                                    if (virtual_cost <= side_cost || virtual_cost > max_virtual_cost ||
                                        layers_[child_cost].empty()) {
                                        continue;
                                    }
                                    nearby_indices.clear();
                                    add_window_indices(nearby_indices, layers_[child_cost], arena_,
                                                       *desired_child, 0, cfg_.inverse_neighbors);
                                    for (std::size_t index : nearby_indices) {
                                        const ExprId child_id = layers_[child_cost][index];
                                        ++stats_.attempted;
                                        ++stats_.by_cost[virtual_cost].attempted;
                                        const auto candidate = anchor_on_left
                                            ? apply_binary(cfg_, arena_, inner_op, inner_anchor_id, child_id,
                                                           static_cast<std::uint16_t>(virtual_cost))
                                            : apply_binary(cfg_, arena_, inner_op, child_id, inner_anchor_id,
                                                           static_cast<std::uint16_t>(virtual_cost));
                                        if (!candidate) continue;
                                        ++stats_.valid;
                                        ++stats_.by_cost[virtual_cost].valid;
                                        virtuals.consider(*candidate);
                                    }
                                }
                            }
                        }
                    }

                    for (const Candidate& virtual_candidate : virtuals.take(32)) {
                        const ExprId virtual_id = append_auxiliary(virtual_candidate, auxiliary_ids);
                        const unsigned total_cost = virtual_candidate.cost + top_anchor_cost + top_op.cost;
                        if (total_cost > cfg_.max_cost) continue;
                        ++stats_.attempted;
                        ++stats_.by_cost[total_cost].attempted;
                        const auto final_candidate = virtual_on_left
                            ? apply_binary(cfg_, arena_, top_op, virtual_id, top_anchor_id,
                                           static_cast<std::uint16_t>(total_cost))
                            : apply_binary(cfg_, arena_, top_op, top_anchor_id, virtual_id,
                                           static_cast<std::uint16_t>(total_cost));
                        if (!final_candidate) continue;
                        ++stats_.valid;
                        ++stats_.by_cost[total_cost].valid;
                        auto [collector_it, _] = collectors.try_emplace(
                            total_cost, cfg_.target, state_value_bits(cfg_),
                            std::max<std::size_t>(cfg_.beam * 3, 256), false, 0,
                            cfg_.pareto_slots, pareto_extra_cap());
                        collector_it->second.consider(*final_candidate);
                    }
                }
            }
        }
        run_deep_unbalanced_mitm(collectors, side_cost, auxiliary_ids);
    }

    void run_terminal_mitm(LiveTopReporter& live_reporter, unsigned side_cost) {
        std::map<unsigned, CandidateCollector> collectors;

        for (std::size_t op_index = 0; op_index < binary_ops_.size(); ++op_index) {
            const auto& op = binary_ops_[op_index];
            for (unsigned left_cost = 1; left_cost <= side_cost; ++left_cost) {
                if (layers_[left_cost].empty()) continue;
                for (unsigned right_cost = 1; right_cost <= side_cost; ++right_cost) {
                    if (layers_[right_cost].empty()) continue;
                    if (is_commutative(op.kind) && left_cost > right_cost) continue;

                    const unsigned total_cost = left_cost + right_cost + op.cost;
                    if (total_cost <= side_cost || total_cost > cfg_.max_cost) continue;
                    auto [collector_it, _] = collectors.try_emplace(
                            total_cost, cfg_.target, state_value_bits(cfg_),
                        std::max<std::size_t>(cfg_.beam * 3, 256), false, 0,
                        cfg_.pareto_slots, pareto_extra_cap());
                    CandidateCollector& collector = collector_it->second;
                    const auto partition_start = std::chrono::steady_clock::now();

                    const auto process_direction = [&](bool reverse) {
                        const auto& outer_ids = reverse ? layers_[right_cost] : layers_[left_cost];
                        // Local collectors prune before the partition results are merged. Keep
                        // the partition boundaries independent of worker count so identical
                        // settings produce the same archive with any --threads value. Sixteen
                        // chunks preserves the established default eight-worker search shape.
                        const std::size_t desired_chunks = std::max<std::size_t>(
                            1, std::min<std::size_t>(cfg_.task_chunks, 16));
                        const std::size_t chunks = std::min(outer_ids.size(), desired_chunks);
                        const std::size_t chunk_size = (outer_ids.size() + chunks - 1) / chunks;
                        std::vector<GenTask> tasks;
                        tasks.reserve(chunks);
                        for (std::size_t begin = 0; begin < outer_ids.size(); begin += chunk_size) {
                            tasks.push_back({GenTask::Type::Binary, op_index,
                                             static_cast<std::uint16_t>(left_cost),
                                             static_cast<std::uint16_t>(right_cost), begin,
                                             std::min(outer_ids.size(), begin + chunk_size), false,
                                             is_commutative(op.kind) && left_cost == right_cost, 0,
                                             static_cast<std::uint16_t>(total_cost), reverse});
                        }

                        std::vector<TaskResult> results(tasks.size());
                        executor_.run(tasks.size(), [&](std::size_t i) {
                            results[i] = process_task(tasks[i]);
                        });

                        CostStats& cs = stats_.by_cost[total_cost];
                        for (const auto& result : results) {
                            cs.attempted = saturating_add(cs.attempted, result.attempted);
                            cs.valid = saturating_add(cs.valid, result.valid);
                            cs.task_candidates += result.candidates.size();
                            stats_.attempted = saturating_add(stats_.attempted, result.attempted);
                            stats_.valid = saturating_add(stats_.valid, result.valid);
                            if (cfg_.live) live_reporter.consider_batch(result.candidates, total_cost);
                            for (const Candidate& candidate : result.candidates) collector.consider(candidate);
                            for (const Candidate& candidate : result.extra_candidates) {
                                collector.consider_extra_only(candidate);
                            }
                        }
                    };

                    process_direction(false);
                    if (!is_commutative(op.kind)) process_direction(true);
                    stats_.by_cost[total_cost].seconds +=
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - partition_start).count();
                }
            }
        }

        run_unbalanced_mitm(collectors, side_cost);

        for (auto& [cost, collector] : collectors) {
            auto survivors = collector.take(cfg_.beam);
            auto extra_survivors = collector.take_extras(pareto_extra_cap());
            if (cfg_.live) live_reporter.consider_batch(survivors, cost);
            auto& layer = layers_[cost];
            layer.reserve(layer.size() + survivors.size());
            for (const Candidate& candidate : survivors) {
                const auto key = state_bucket(candidate.value, candidate.derivative, candidate.depends_on_x,
                                              candidate.constraint_state, state_value_bits(cfg_), cfg_.equations);
                if (seen_.find(key) != seen_.end()) continue;
                if (arena_.size() >= kNoExpr) throw std::runtime_error("表达式数量超过 32 位索引上限");
                Node node;
                node.value = candidate.value;
                node.derivative = candidate.derivative;
                node.cost = candidate.cost;
                node.nodes = candidate.nodes;
                node.depth = candidate.depth;
                node.hash = candidate.hash;
                node.tag = candidate.tag;
                node.op = candidate.op;
                node.left = candidate.left;
                node.right = candidate.right;
                node.atom_index = candidate.atom_index;
                node.constraint_state = candidate.constraint_state;
                node.depends_on_x = candidate.depends_on_x;
                const ExprId id = static_cast<ExprId>(arena_.size());
                arena_.push_back(node);
                layer.push_back(id);
                seen_.emplace(key);
                remember_primary_hash(candidate.hash);
            }
            for (const Candidate& candidate : extra_survivors) append_extra_candidate(candidate);
            std::sort(layer.begin(), layer.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                return arena_[a].hash < arena_[b].hash;
            });
            auto& extra_layer = extra_layers_[cost];
            std::sort(extra_layer.begin(), extra_layer.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                return arena_[a].hash < arena_[b].hash;
            });
            stats_.by_cost[cost].kept = layer.size();
            stats_.kept += layer.size();
        }
        if (cfg_.live) live_reporter.snapshot(cfg_.max_cost);
    }

    TaskResult process_deep_slice(const BinarySpec& op,
                                  const std::vector<ExprId>& outer_ids,
                                  const std::vector<ExprId>& inner_ids,
                                  std::size_t begin,
                                  std::size_t end,
                                  bool outer_on_left,
                                  std::uint16_t total_cost,
                                  std::size_t cap) const {
        CandidateCollector collector(cfg_.target, state_value_bits(cfg_), cap, false, 0,
                                     cfg_.pareto_slots, pareto_extra_cap());
        TaskResult result;
        std::vector<std::size_t> indices;
        indices.reserve(cfg_.inverse_neighbors + 24);
        for (std::size_t i = begin; i < end; ++i) {
            const ExprId outer_id = outer_ids[i];
            const double outer_value = arena_[outer_id].value;
            const auto desired = outer_on_left ? desired_right(op.kind, outer_value, cfg_.target)
                                               : desired_left(op.kind, outer_value, cfg_.target);
            indices.clear();
            if (desired) {
                add_near_indices(indices, inner_ids, arena_, *desired, 0, cfg_.inverse_neighbors);
            }
            if (outer_on_left && op.kind == BinaryKind::Pow) {
                static constexpr double useful_exponents[] = {-3.0, -2.0, -1.0, -0.5,
                                                               0.5, 2.0, 3.0};
                for (double exponent : useful_exponents) {
                    add_near_indices(indices, inner_ids, arena_, exponent, 0,
                                     std::min<std::size_t>(3, cfg_.inverse_neighbors));
                }
            }
            if (cfg_.explore_pairs > 0 && !inner_ids.empty()) {
                std::uint64_t seed = mix64(arena_[outer_id].hash ^
                                           (static_cast<std::uint64_t>(op.kind) << 56U) ^
                                           (static_cast<std::uint64_t>(outer_on_left) << 55U) ^
                                           total_cost);
                const std::size_t range = inner_ids.size();
                for (std::size_t sample = 0; sample < cfg_.explore_pairs; ++sample) {
                    seed = mix64(seed ^ (0x94d049bb133111ebULL + sample));
                    indices.push_back(static_cast<std::size_t>(seed % range));
                }
            }
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
            for (std::size_t index : indices) {
                ++result.attempted;
                const ExprId inner_id = inner_ids[index];
                const auto candidate = outer_on_left
                    ? apply_binary(cfg_, arena_, op, outer_id, inner_id, total_cost)
                    : apply_binary(cfg_, arena_, op, inner_id, outer_id, total_cost);
                if (!candidate) continue;
                ++result.valid;
                const auto key = state_bucket(candidate->value, candidate->derivative,
                                              candidate->depends_on_x, candidate->constraint_state,
                                              state_value_bits(cfg_), false);
                if (seen_.find(key) == seen_.end()) collector.consider_with_key(*candidate, key);
                else collector.consider_extra_only(*candidate);
            }
        }
        result.candidates = collector.take(cap);
        result.extra_candidates = collector.take_extras(pareto_extra_cap());
        return result;
    }

    std::vector<ExprId> select_deep_frontier(const std::vector<ExprId>& ids,
                                             std::size_t limit) const {
        if (limit == 0 || ids.size() <= limit) return ids;

        std::vector<ExprId> selected;
        selected.reserve(limit);
        FastSet<ExprId> selected_ids;
        selected_ids.reserve(limit * 2 + 1);
        const auto add = [&](ExprId id) {
            if (selected.size() < limit && selected_ids.emplace(id).second) selected.push_back(id);
        };

        std::vector<ExprId> ranked = ids;
        std::sort(ranked.begin(), ranked.end(), [&](ExprId a, ExprId b) {
            const double ea = std::abs(arena_[a].value - cfg_.target);
            const double eb = std::abs(arena_[b].value - cfg_.target);
            if (ea != eb) return ea < eb;
            if (arena_[a].nodes != arena_[b].nodes) return arena_[a].nodes < arena_[b].nodes;
            if (arena_[a].depth != arena_[b].depth) return arena_[a].depth < arena_[b].depth;
            return arena_[a].hash < arena_[b].hash;
        });
        const std::size_t near_quota = std::max<std::size_t>(1, (limit * 2) / 5);
        for (std::size_t i = 0; i < near_quota; ++i) add(ranked[i]);

        std::sort(ranked.begin(), ranked.end(), [&](ExprId a, ExprId b) {
            if (arena_[a].nodes != arena_[b].nodes) return arena_[a].nodes < arena_[b].nodes;
            if (arena_[a].depth != arena_[b].depth) return arena_[a].depth < arena_[b].depth;
            const double ea = std::abs(arena_[a].value - cfg_.target);
            const double eb = std::abs(arena_[b].value - cfg_.target);
            if (ea != eb) return ea < eb;
            return arena_[a].hash < arena_[b].hash;
        });
        const std::size_t elegant_target = std::min(limit, near_quota + (limit * 3) / 10);
        for (ExprId id : ranked) {
            if (selected.size() >= elegant_target) break;
            add(id);
        }

        // ids is value-sorted.  Uniform quantiles preserve numerically remote
        // building blocks that target-only ranking would discard.
        const std::size_t remaining = limit - selected.size();
        if (remaining == 1) {
            add(ids[ids.size() / 2]);
        } else if (remaining > 1) {
            for (std::size_t i = 0; i < remaining * 2 && selected.size() < limit; ++i) {
                const std::size_t position =
                    (i * (ids.size() - 1)) / std::max<std::size_t>(1, remaining * 2 - 1);
                add(ids[position]);
            }
        }
        for (ExprId id : ranked) {
            if (selected.size() == limit) break;
            add(id);
        }
        std::sort(selected.begin(), selected.end(), [&](ExprId a, ExprId b) {
            if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
            return arena_[a].hash < arena_[b].hash;
        });
        return selected;
    }

    void run_deep_compositions(LiveTopReporter& live_reporter, unsigned side_cost) {
        stats_.used_deep_compositions = true;
        const std::size_t deep_cap = cfg_.deep_beam == 0
            ? std::max<std::size_t>(256, std::min<std::size_t>(cfg_.beam, 4096))
            : cfg_.deep_beam;

        auto all_layers = extended_layers_snapshot();
        std::vector<std::vector<ExprId>> frontier(layers_.size());
        std::vector<std::vector<ExprId>> bounded_all_layers(layers_.size());
        FastSet<std::uint64_t> archive_hashes;
        archive_hashes.reserve(arena_.size() * 2 + 1);
        for (const Node& node : arena_) {
            if (node.eligible) archive_hashes.emplace(node.hash);
        }
        for (unsigned cost = 1; cost < all_layers.size(); ++cost) {
            bounded_all_layers[cost] = select_deep_frontier(all_layers[cost], cfg_.deep_frontier);
            // Primary side×side roots were already checked by terminal MITM.
            // At low cost only Pareto/extension sidecars are genuinely new;
            // above side_cost, terminal and inverse results form the frontier.
            if (cost <= side_cost) frontier[cost] = extra_layers_[cost];
            else frontier[cost] = bounded_all_layers[cost];
            frontier[cost] = select_deep_frontier(frontier[cost], cfg_.deep_frontier);
        }

        for (unsigned round = 0; round < cfg_.deep_rounds; ++round) {
            const auto round_start = std::chrono::steady_clock::now();
            std::map<unsigned, CandidateCollector> collectors;

            for (std::size_t op_index = 0; op_index < unary_ops_.size(); ++op_index) {
                const UnarySpec& op = unary_ops_[op_index];
                for (unsigned child_cost = 1; child_cost < frontier.size(); ++child_cost) {
                    const unsigned total_cost = child_cost + op.cost;
                    if (total_cost > cfg_.max_cost) continue;
                    const std::vector<ExprId>* ids_ptr = &frontier[child_cost];
                    if (round == 0 && child_cost <= side_cost && total_cost > side_cost) {
                        ids_ptr = &bounded_all_layers[child_cost];
                    }
                    const auto& ids = *ids_ptr;
                    if (ids.empty()) continue;
                    auto [collector_it, _] = collectors.try_emplace(
                        total_cost, cfg_.target, state_value_bits(cfg_), deep_cap, false, 0,
                        cfg_.pareto_slots, pareto_extra_cap());
                    CandidateCollector& collector = collector_it->second;
                    const std::size_t chunks = std::min<std::size_t>(
                        ids.size(), std::max<std::size_t>(1, std::min<std::size_t>(
                            cfg_.task_chunks, static_cast<std::size_t>(cfg_.threads) * 2)));
                    const std::size_t chunk_size = (ids.size() + chunks - 1) / chunks;
                    std::vector<TaskResult> results(chunks);
                    executor_.run(chunks, [&](std::size_t chunk) {
                        const std::size_t begin = chunk * chunk_size;
                        const std::size_t end = std::min(ids.size(), begin + chunk_size);
                        CandidateCollector local(cfg_.target, state_value_bits(cfg_), deep_cap, false, 0,
                                                 cfg_.pareto_slots, pareto_extra_cap());
                        TaskResult& result = results[chunk];
                        for (std::size_t i = begin; i < end; ++i) {
                            ++result.attempted;
                            const auto candidate = apply_unary(
                                cfg_, arena_, op, ids[i], static_cast<std::uint16_t>(total_cost));
                            if (!candidate) continue;
                            ++result.valid;
                            const auto key = state_bucket(candidate->value, candidate->derivative,
                                                          candidate->depends_on_x, candidate->constraint_state,
                                                          state_value_bits(cfg_), false);
                            if (seen_.find(key) == seen_.end()) local.consider_with_key(*candidate, key);
                            else local.consider_extra_only(*candidate);
                        }
                        result.candidates = local.take(deep_cap);
                        result.extra_candidates = local.take_extras(pareto_extra_cap());
                    });
                    CostStats& cs = stats_.by_cost[total_cost];
                    for (const TaskResult& result : results) {
                        cs.attempted = saturating_add(cs.attempted, result.attempted);
                        cs.valid = saturating_add(cs.valid, result.valid);
                        cs.task_candidates += result.candidates.size();
                        stats_.attempted = saturating_add(stats_.attempted, result.attempted);
                        stats_.valid = saturating_add(stats_.valid, result.valid);
                        for (const Candidate& candidate : result.candidates) collector.consider(candidate);
                        for (const Candidate& candidate : result.extra_candidates) {
                            collector.consider_extra_only(candidate);
                        }
                    }
                }
            }

            const auto merge_binary_results = [&](unsigned total_cost,
                                                  std::vector<TaskResult>& results) {
                auto [collector_it, _] = collectors.try_emplace(
                            total_cost, cfg_.target, state_value_bits(cfg_), deep_cap, false, 0,
                    cfg_.pareto_slots, pareto_extra_cap());
                CandidateCollector& collector = collector_it->second;
                CostStats& cs = stats_.by_cost[total_cost];
                for (const TaskResult& result : results) {
                    cs.attempted = saturating_add(cs.attempted, result.attempted);
                    cs.valid = saturating_add(cs.valid, result.valid);
                    cs.task_candidates += result.candidates.size();
                    stats_.attempted = saturating_add(stats_.attempted, result.attempted);
                    stats_.valid = saturating_add(stats_.valid, result.valid);
                    for (const Candidate& candidate : result.candidates) collector.consider(candidate);
                    for (const Candidate& candidate : result.extra_candidates) {
                        collector.consider_extra_only(candidate);
                    }
                }
            };

            for (const BinarySpec& op : binary_ops_) {
                for (unsigned frontier_cost = 1; frontier_cost < frontier.size(); ++frontier_cost) {
                    const auto& new_ids = frontier[frontier_cost];
                    if (new_ids.empty()) continue;
                    for (unsigned partner_cost = 1; partner_cost < all_layers.size(); ++partner_cost) {
                        const auto& all_ids = all_layers[partner_cost];
                        if (all_ids.empty()) continue;
                        const unsigned total_cost = frontier_cost + partner_cost + op.cost;
                        if (total_cost > cfg_.max_cost) continue;

                        const auto run_direction = [&](const std::vector<ExprId>& outer_ids,
                                                       const std::vector<ExprId>& inner_ids,
                                                       bool outer_on_left) {
                            const std::size_t chunks = std::min<std::size_t>(
                                outer_ids.size(), std::max<std::size_t>(1, std::min<std::size_t>(
                                    cfg_.task_chunks, static_cast<std::size_t>(cfg_.threads) * 2)));
                            const std::size_t chunk_size = (outer_ids.size() + chunks - 1) / chunks;
                            std::vector<TaskResult> results(chunks);
                            executor_.run(chunks, [&](std::size_t chunk) {
                                const std::size_t begin = chunk * chunk_size;
                                results[chunk] = process_deep_slice(
                                    op, outer_ids, inner_ids, begin,
                                    std::min(outer_ids.size(), begin + chunk_size), outer_on_left,
                                    static_cast<std::uint16_t>(total_cost), deep_cap);
                            });
                            merge_binary_results(total_cost, results);
                        };

                        run_direction(new_ids, all_ids, true);
                        if (!is_commutative(op.kind)) run_direction(new_ids, all_ids, false);
                    }
                }
            }

            std::vector<std::vector<ExprId>> next_frontier(layers_.size());
            std::size_t appended = 0;
            std::vector<Candidate> live_candidates;
            for (auto& [cost, collector] : collectors) {
                auto candidates = collector.take(deep_cap);
                auto extras = collector.take_extras(pareto_extra_cap());
                candidates.insert(candidates.end(), extras.begin(), extras.end());
                for (const Candidate& candidate : candidates) {
                    const auto state_key = state_bucket(candidate.value, candidate.derivative,
                                                        candidate.depends_on_x, candidate.constraint_state,
                                                        state_value_bits(cfg_), false);
                    const bool new_state = seen_.find(state_key) == seen_.end();
                    if (!new_state && cfg_.pareto_slots <= 1) continue;
                    if (!archive_hashes.emplace(candidate.hash).second) continue;
                    const ExprId id = append_candidate_node(candidate);
                    extra_layers_[cost].push_back(id);
                    next_frontier[cost].push_back(id);
                    all_layers[cost].push_back(id);
                    extra_seen_.emplace(candidate.hash);
                    if (new_state) seen_.emplace(state_key);
                    ++appended;
                    ++stats_.kept;
                    if (!new_state) ++stats_.pareto_extras;
                    if (cfg_.live) live_candidates.push_back(candidate);
                }
                std::sort(all_layers[cost].begin(), all_layers[cost].end(), [&](ExprId a, ExprId b) {
                    if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                    return arena_[a].hash < arena_[b].hash;
                });
                std::sort(extra_layers_[cost].begin(), extra_layers_[cost].end(), [&](ExprId a, ExprId b) {
                    if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                    return arena_[a].hash < arena_[b].hash;
                });
                stats_.by_cost[cost].kept = layers_[cost].size() + extra_layers_[cost].size();
            }
            if (cfg_.live) live_reporter.consider_batch(live_candidates, cfg_.max_cost);
            if (cfg_.verbose) {
                std::cerr << "deep_round=" << (round + 1) << " frontier=" << appended
                          << " time=" << std::fixed << std::setprecision(3)
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - round_start).count()
                          << "s\n";
            }
            if (cfg_.live) live_reporter.snapshot(cfg_.max_cost);
            if (appended == 0) break;
            if (cfg_.deep_frontier != 0) {
                for (auto& ids : next_frontier) {
                    ids = select_deep_frontier(ids, cfg_.deep_frontier);
                }
            }
            frontier = std::move(next_frontier);
        }
    }

    std::vector<ExprId> select_pslq_basis() const {
        struct RankedBasis {
            ExprId id{kNoExpr};
            double score{};
            std::uint64_t value_key{};
        };

        std::vector<RankedBasis> ranked;
        ranked.reserve(arena_.size());
        for (ExprId id = 0; id < arena_.size(); ++id) {
            const Node& node = arena_[id];
            if (!node.eligible || node.depends_on_x || node.value == 0.0 ||
                !std::isfinite(node.value) || node.cost > cfg_.max_cost) {
                continue;
            }
            const double magnitude = std::abs(node.value);
            const double scale_penalty = std::abs(std::log(std::max(magnitude, 1.0e-300)));
            const double target_ratio = std::abs(std::log(
                std::max(magnitude, 1.0e-300) /
                std::max(std::abs(cfg_.target), 1.0e-300)));
            const double score = static_cast<double>(node.cost) + 0.06 * node.nodes +
                                 0.015 * scale_penalty + 0.01 * target_ratio;
            ranked.push_back({id, score, value_bucket(node.value, 48)});
        }
        std::sort(ranked.begin(), ranked.end(), [&](const RankedBasis& left, const RankedBasis& right) {
            if (left.score != right.score) return left.score < right.score;
            const Node& a = arena_[left.id];
            const Node& b = arena_[right.id];
            if (a.cost != b.cost) return a.cost < b.cost;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            return a.hash < b.hash;
        });

        std::vector<ExprId> basis;
        basis.reserve(std::min(cfg_.pslq_basis, ranked.size()));
        FastSet<std::uint64_t> values;
        values.reserve(cfg_.pslq_basis * 2 + 1);
        for (const RankedBasis& entry : ranked) {
            if (!values.emplace(entry.value_key).second) continue;
            basis.push_back(entry.id);
            if (basis.size() == cfg_.pslq_basis) break;
        }
        return basis;
    }

    std::vector<ExprId> pslq_integer_library(std::int64_t maximum) const {
        std::vector<ExprId> integers(static_cast<std::size_t>(maximum) + 1, kNoExpr);
        for (ExprId id = 0; id < arena_.size(); ++id) {
            const Node& node = arena_[id];
            if (!node.eligible || node.depends_on_x || node.value < 1.0 ||
                node.value > static_cast<double>(maximum)) {
                continue;
            }
            const double rounded = std::nearbyint(node.value);
            if (std::abs(node.value - rounded) >
                8.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, node.value)) {
                continue;
            }
            const std::size_t value = static_cast<std::size_t>(rounded);
            ExprId& current = integers[value];
            if (current == kNoExpr ||
                std::pair{node.cost, node.nodes} <
                    std::pair{arena_[current].cost, arena_[current].nodes}) {
                current = id;
            }
        }
        return integers;
    }

    void run_pslq_search(LiveTopReporter& live_reporter) {
        stats_.used_pslq = true;
        const std::vector<ExprId> basis = select_pslq_basis();
        if (basis.empty()) return;

        const auto integer_ids = pslq_integer_library(cfg_.pslq_max_coefficient);
        const BinarySpec* add = find_binary_operation(BinaryKind::Add);
        const BinarySpec* sub = find_binary_operation(BinaryKind::Sub);
        const BinarySpec* mul = find_binary_operation(BinaryKind::Mul);
        const BinarySpec* div = find_binary_operation(BinaryKind::Div);
        const UnarySpec* neg = find_unary_operation(UnaryKind::Neg);
        FastMap<std::uint64_t, ExprId> structure_index = build_structure_index();
        std::vector<unsigned char> touched_cost(cfg_.max_cost + 1, 0);
        std::vector<Candidate> live_candidates;
        live_candidates.reserve(std::min<std::size_t>(basis.size() * 2, 512));

        double existing_best = std::numeric_limits<double>::infinity();
        for (const Node& node : arena_) {
            if (!node.eligible || !constraint_satisfied(cfg_, node.constraint_state) ||
                !cfg_.error_range.contains(node.value - cfg_.target)) {
                continue;
            }
            existing_best = std::min(existing_best, std::abs(node.value - cfg_.target));
        }
        if (!std::isfinite(existing_best)) existing_best = std::max(1.0, std::abs(cfg_.target));

        const auto evaluate_unary = [&](const UnarySpec& operation, ExprId child) -> ExprId {
            const unsigned cost = static_cast<unsigned>(arena_[child].cost) + operation.cost;
            if (cost > cfg_.max_cost) return kNoExpr;
            ++stats_.attempted;
            ++stats_.by_cost[cost].attempted;
            const auto candidate = apply_unary(
                cfg_, arena_, operation, child, static_cast<std::uint16_t>(cost));
            if (!candidate) return kNoExpr;
            ++stats_.valid;
            ++stats_.by_cost[cost].valid;
            return materialize_candidate(*candidate, structure_index, touched_cost, false, false);
        };
        const auto evaluate_binary = [&](const BinarySpec& operation, ExprId left, ExprId right) -> ExprId {
            const unsigned cost = static_cast<unsigned>(arena_[left].cost) + arena_[right].cost + operation.cost;
            if (cost > cfg_.max_cost) return kNoExpr;
            ++stats_.attempted;
            ++stats_.by_cost[cost].attempted;
            const auto candidate = apply_binary(
                cfg_, arena_, operation, left, right, static_cast<std::uint16_t>(cost));
            if (!candidate) return kNoExpr;
            ++stats_.valid;
            ++stats_.by_cost[cost].valid;
            return materialize_candidate(*candidate, structure_index, touched_cost, false, false);
        };

        const auto build_relation = [&](IntegerRelation relation,
                                        const std::array<ExprId, 3>& relation_basis) {
            if (relation.size < 2 || relation.coefficients[0] == 0) return;
            if (relation.coefficients[0] < 0) {
                for (std::size_t index = 0; index < relation.size; ++index) {
                    relation.coefficients[index] *= -1;
                }
            }

            std::array<ExprId, 3> positive{kNoExpr, kNoExpr, kNoExpr};
            std::array<ExprId, 3> negative{kNoExpr, kNoExpr, kNoExpr};
            std::size_t positive_count = 0;
            std::size_t negative_count = 0;
            for (std::size_t index = 1; index < relation.size; ++index) {
                const std::int64_t coefficient = -relation.coefficients[index];
                if (coefficient == 0) continue;
                const std::int64_t magnitude = static_cast<std::int64_t>(std::abs(coefficient));
                ExprId term = relation_basis[index - 1];
                if (magnitude > 1) {
                    if (!mul || magnitude >= static_cast<std::int64_t>(integer_ids.size()) ||
                        integer_ids[static_cast<std::size_t>(magnitude)] == kNoExpr) {
                        return;
                    }
                    term = evaluate_binary(*mul, integer_ids[static_cast<std::size_t>(magnitude)], term);
                    if (term == kNoExpr) return;
                }
                if (coefficient > 0) positive[positive_count++] = term;
                else negative[negative_count++] = term;
            }
            if (positive_count == 0 && negative_count == 0) return;

            const auto combine_terms = [&](const std::array<ExprId, 3>& terms,
                                           std::size_t count) -> ExprId {
                if (count == 0) return kNoExpr;
                ExprId current = terms[0];
                for (std::size_t index = 1; index < count; ++index) {
                    if (!add) return kNoExpr;
                    current = evaluate_binary(*add, current, terms[index]);
                    if (current == kNoExpr) return kNoExpr;
                }
                return current;
            };

            const ExprId positive_sum = combine_terms(positive, positive_count);
            const ExprId negative_sum = combine_terms(negative, negative_count);
            if ((positive_count != 0 && positive_sum == kNoExpr) ||
                (negative_count != 0 && negative_sum == kNoExpr)) {
                return;
            }
            ExprId result = kNoExpr;
            if (positive_sum != kNoExpr && negative_sum != kNoExpr) {
                if (!sub) return;
                result = evaluate_binary(*sub, positive_sum, negative_sum);
            } else if (positive_sum != kNoExpr) {
                result = positive_sum;
            } else {
                if (!neg) return;
                result = evaluate_unary(*neg, negative_sum);
            }
            if (result == kNoExpr) return;

            const std::int64_t target_coefficient = relation.coefficients[0];
            if (target_coefficient > 1) {
                if (!div || target_coefficient >= static_cast<std::int64_t>(integer_ids.size()) ||
                    integer_ids[static_cast<std::size_t>(target_coefficient)] == kNoExpr) {
                    return;
                }
                result = evaluate_binary(
                    *div, result, integer_ids[static_cast<std::size_t>(target_coefficient)]);
                if (result == kNoExpr) return;
            }

            bool newly_eligible = false;
            const Candidate final_candidate = candidate_from_node(result);
            const ExprId final_id = materialize_candidate(
                final_candidate, structure_index, touched_cost, true, true, &newly_eligible);
            if (final_id == kNoExpr || !newly_eligible) return;
            ++stats_.pslq_candidates;
            live_candidates.push_back(candidate_from_node(final_id));
        };

        const auto probe = [&](const std::array<ExprId, 3>& ids, std::size_t relation_size) {
            std::array<long double, 4> values{};
            values[0] = static_cast<long double>(cfg_.target);
            long double scale = std::max(1.0L, std::abs(values[0]));
            for (std::size_t index = 1; index < relation_size; ++index) {
                values[index] = static_cast<long double>(arena_[ids[index - 1]].value);
                scale += std::abs(values[index]);
            }
            const long double configured = static_cast<long double>(cfg_.pslq_tolerance) * scale;
            const long double adaptive = static_cast<long double>(existing_best) *
                                         std::max<std::int64_t>(1, cfg_.pslq_max_coefficient);
            const long double tolerance = std::max(configured, std::min(scale * 1.0e-3L, adaptive));
            const auto relation = find_integer_relation(
                values, relation_size, tolerance, cfg_.pslq_max_coefficient, cfg_.pslq_steps);
            if (!relation || relation->coefficients[0] == 0) return;
            ++stats_.pslq_relations;
            build_relation(*relation, ids);
        };

        for (ExprId id : basis) probe({id, kNoExpr, kNoExpr}, 2);
        std::uint64_t pair_count = 0;
        for (std::size_t left = 0; left < basis.size() && pair_count < cfg_.pslq_pairs; ++left) {
            for (std::size_t right = left + 1;
                 right < basis.size() && pair_count < cfg_.pslq_pairs; ++right, ++pair_count) {
                probe({basis[left], basis[right], kNoExpr}, 3);
            }
        }

        sort_touched_layers(touched_cost);
        if (cfg_.live && !live_candidates.empty()) {
            live_reporter.consider_batch(live_candidates, cfg_.max_cost);
            live_reporter.snapshot(cfg_.max_cost);
        }
        if (cfg_.verbose) {
            std::cerr << "pslq_basis=" << basis.size()
                      << " relations=" << stats_.pslq_relations
                      << " appended=" << stats_.pslq_candidates << '\n';
        }
    }

    void run_egraph_search(LiveTopReporter& live_reporter) {
        stats_.used_egraph = true;
        if (cfg_.egraph_node_limit == 0 || cfg_.egraph_rounds == 0) return;

        struct SaturationGraph {
            std::vector<std::size_t> parent;
            std::vector<ExprId> best;
            FastMap<ExprId, std::size_t> membership;

            std::size_t add(ExprId id) {
                if (const auto found = membership.find(id); found != membership.end()) {
                    return found->second;
                }
                const std::size_t index = parent.size();
                parent.push_back(index);
                best.push_back(id);
                membership.emplace(id, index);
                return index;
            }

            std::size_t root(std::size_t index) {
                while (parent[index] != index) {
                    parent[index] = parent[parent[index]];
                    index = parent[index];
                }
                return index;
            }

            void unite(ExprId left, ExprId right, const std::vector<Node>& arena) {
                std::size_t a = root(add(left));
                std::size_t b = root(add(right));
                if (a == b) return;
                if (b < a) std::swap(a, b);
                parent[b] = a;
                const ExprId left_best = best[a];
                const ExprId right_best = best[b];
                const Node& x = arena[left_best];
                const Node& y = arena[right_best];
                if (std::pair{y.cost, y.nodes} < std::pair{x.cost, x.nodes}) best[a] = right_best;
            }
        } graph;

        std::vector<ExprId> candidates;
        candidates.reserve(arena_.size());
        for (ExprId id = 0; id < arena_.size(); ++id) {
            const Node& node = arena_[id];
            if (node.eligible && !node.depends_on_x && node.tag != NodeTag::Atom) candidates.push_back(id);
        }
        const auto elegance_order = [&](ExprId left, ExprId right) {
            const Node& a = arena_[left];
            const Node& b = arena_[right];
            if (a.cost != b.cost) return a.cost < b.cost;
            if (a.nodes != b.nodes) return a.nodes < b.nodes;
            return a.hash < b.hash;
        };
        const auto target_order = [&](ExprId left, ExprId right) {
            const Node& a = arena_[left];
            const Node& b = arena_[right];
            const double ae = std::abs(a.value - cfg_.target) / std::max(1.0, std::abs(cfg_.target));
            const double be = std::abs(b.value - cfg_.target) / std::max(1.0, std::abs(cfg_.target));
            const double as = std::log1p(ae * 1.0e12) + 0.08 * a.cost + 0.01 * a.nodes;
            const double bs = std::log1p(be * 1.0e12) + 0.08 * b.cost + 0.01 * b.nodes;
            if (as != bs) return as < bs;
            return elegance_order(left, right);
        };

        std::vector<ExprId> seeds;
        seeds.reserve(std::min(cfg_.egraph_seeds, candidates.size()));
        FastSet<ExprId> selected;
        selected.reserve(cfg_.egraph_seeds * 2 + 1);
        std::sort(candidates.begin(), candidates.end(), elegance_order);
        const std::size_t elegance_count = std::min(candidates.size(), (cfg_.egraph_seeds + 1) / 2);
        for (std::size_t index = 0; index < elegance_count; ++index) {
            if (selected.emplace(candidates[index]).second) seeds.push_back(candidates[index]);
        }
        std::sort(candidates.begin(), candidates.end(), target_order);
        for (ExprId id : candidates) {
            if (selected.emplace(id).second) seeds.push_back(id);
            if (seeds.size() == cfg_.egraph_seeds) break;
        }
        for (ExprId id : seeds) graph.add(id);

        FastMap<std::uint64_t, ExprId> structure_index = build_structure_index();
        std::vector<unsigned char> touched_cost(cfg_.max_cost + 1, 0);
        std::vector<Candidate> live_candidates;
        live_candidates.reserve(std::min<std::size_t>(cfg_.egraph_node_limit, 512));

        const auto make_unary = [&](UnaryKind kind, ExprId child) -> std::optional<Candidate> {
            const UnarySpec* operation = find_unary_operation(kind);
            if (!operation) return std::nullopt;
            const unsigned cost = static_cast<unsigned>(arena_[child].cost) + operation->cost;
            if (cost > cfg_.max_cost) return std::nullopt;
            ++stats_.attempted;
            ++stats_.by_cost[cost].attempted;
            auto candidate = apply_unary(
                cfg_, arena_, *operation, child, static_cast<std::uint16_t>(cost));
            if (candidate) {
                ++stats_.valid;
                ++stats_.by_cost[cost].valid;
            }
            return candidate;
        };
        const auto make_binary = [&](BinaryKind kind, ExprId left, ExprId right) -> std::optional<Candidate> {
            const BinarySpec* operation = find_binary_operation(kind);
            if (!operation) return std::nullopt;
            const unsigned cost = static_cast<unsigned>(arena_[left].cost) + arena_[right].cost + operation->cost;
            if (cost > cfg_.max_cost) return std::nullopt;
            ++stats_.attempted;
            ++stats_.by_cost[cost].attempted;
            auto candidate = apply_binary(
                cfg_, arena_, *operation, left, right, static_cast<std::uint16_t>(cost));
            if (candidate) {
                ++stats_.valid;
                ++stats_.by_cost[cost].valid;
            }
            return candidate;
        };
        const auto materialize_auxiliary = [&](const std::optional<Candidate>& candidate) -> ExprId {
            if (!candidate) return kNoExpr;
            return materialize_candidate(
                *candidate, structure_index, touched_cost, false, false);
        };

        std::size_t appended = 0;
        const auto emit = [&](ExprId source_id,
                              const std::optional<Candidate>& candidate,
                              std::vector<ExprId>& next) {
            if (!candidate || appended >= cfg_.egraph_node_limit) return;
            const Node source = arena_[source_id];
            if (!equivalent_value(source.value, candidate->value)) return;
            if (candidate->cost > static_cast<unsigned>(source.cost) + 1U ||
                candidate->nodes > static_cast<unsigned>(source.nodes) + 1U) {
                return;
            }
            bool newly_eligible = false;
            const ExprId id = materialize_candidate(
                *candidate, structure_index, touched_cost, true, false, &newly_eligible);
            if (id == kNoExpr) return;
            graph.unite(source_id, id, arena_);
            ++stats_.egraph_rewrites;
            if (!newly_eligible) return;
            ++appended;
            ++stats_.egraph_candidates;
            next.push_back(id);
            live_candidates.push_back(candidate_from_node(id));
        };

        std::vector<ExprId> frontier = seeds;
        for (unsigned round = 0;
             round < cfg_.egraph_rounds && !frontier.empty() && appended < cfg_.egraph_node_limit;
             ++round) {
            std::vector<ExprId> next;
            next.reserve(std::min<std::size_t>(frontier.size() * 2,
                                               cfg_.egraph_node_limit - appended));
            for (ExprId source_id : frontier) {
                if (appended >= cfg_.egraph_node_limit) break;
                const Node source = arena_[source_id];
                if (source.tag == NodeTag::Unary) continue;
                const BinaryKind kind = static_cast<BinaryKind>(source.op);
                const Node left = arena_[source.left];
                const Node right = arena_[source.right];

                if (kind == BinaryKind::Add) {
                    if (unary_node_is(right, UnaryKind::Neg)) {
                        emit(source_id, make_binary(BinaryKind::Sub, source.left, right.left), next);
                    }
                    if (unary_node_is(left, UnaryKind::Neg)) {
                        emit(source_id, make_binary(BinaryKind::Sub, source.right, left.left), next);
                    }
                    if (unary_node_is(left, UnaryKind::Ln) && unary_node_is(right, UnaryKind::Ln) &&
                        arena_[left.left].value > 0.0 && arena_[right.left].value > 0.0) {
                        const ExprId product = materialize_auxiliary(
                            make_binary(BinaryKind::Mul, left.left, right.left));
                        if (product != kNoExpr) emit(source_id, make_unary(UnaryKind::Ln, product), next);
                    }
                    if (unary_node_is(left, UnaryKind::Neg) && unary_node_is(right, UnaryKind::Neg)) {
                        const ExprId sum = materialize_auxiliary(
                            make_binary(BinaryKind::Add, left.left, right.left));
                        if (sum != kNoExpr) emit(source_id, make_unary(UnaryKind::Neg, sum), next);
                    }
                }

                if (kind == BinaryKind::Sub) {
                    const ExprId negated = materialize_auxiliary(
                        make_unary(UnaryKind::Neg, source.right));
                    if (negated != kNoExpr) {
                        emit(source_id, make_binary(BinaryKind::Add, source.left, negated), next);
                    }
                }

                if (kind == BinaryKind::Mul) {
                    if (unary_node_is(right, UnaryKind::Inv)) {
                        emit(source_id, make_binary(BinaryKind::Div, source.left, right.left), next);
                    }
                    if (unary_node_is(left, UnaryKind::Inv)) {
                        emit(source_id, make_binary(BinaryKind::Div, source.right, left.left), next);
                    }
                    if (unary_node_is(left, UnaryKind::Exp) && unary_node_is(right, UnaryKind::Exp)) {
                        const ExprId sum = materialize_auxiliary(
                            make_binary(BinaryKind::Add, left.left, right.left));
                        if (sum != kNoExpr) emit(source_id, make_unary(UnaryKind::Exp, sum), next);
                    }
                    if (unary_node_is(left, UnaryKind::Sqrt) && unary_node_is(right, UnaryKind::Sqrt) &&
                        arena_[left.left].value >= 0.0 && arena_[right.left].value >= 0.0) {
                        const ExprId product = materialize_auxiliary(
                            make_binary(BinaryKind::Mul, left.left, right.left));
                        if (product != kNoExpr) emit(source_id, make_unary(UnaryKind::Sqrt, product), next);
                    }
                    if (unary_node_is(left, UnaryKind::Inv) && unary_node_is(right, UnaryKind::Inv)) {
                        const ExprId product = materialize_auxiliary(
                            make_binary(BinaryKind::Mul, left.left, right.left));
                        if (product != kNoExpr) emit(source_id, make_unary(UnaryKind::Inv, product), next);
                    }
                    if (left.tag == NodeTag::Binary && right.tag == NodeTag::Binary &&
                        static_cast<BinaryKind>(left.op) == BinaryKind::Pow &&
                        static_cast<BinaryKind>(right.op) == BinaryKind::Pow &&
                        arena_[left.left].hash == arena_[right.left].hash &&
                        arena_[left.left].value > 0.0) {
                        const ExprId exponent = materialize_auxiliary(
                            make_binary(BinaryKind::Add, left.right, right.right));
                        if (exponent != kNoExpr) {
                            emit(source_id, make_binary(BinaryKind::Pow, left.left, exponent), next);
                        }
                    }
                }

                if (kind == BinaryKind::Div) {
                    const ExprId inverse = materialize_auxiliary(
                        make_unary(UnaryKind::Inv, source.right));
                    if (inverse != kNoExpr) {
                        emit(source_id, make_binary(BinaryKind::Mul, source.left, inverse), next);
                    }
                    if (unary_node_is(right, UnaryKind::Inv)) {
                        emit(source_id, make_binary(BinaryKind::Mul, source.left, right.left), next);
                    }
                    if (unary_node_is(left, UnaryKind::Inv) && unary_node_is(right, UnaryKind::Inv)) {
                        emit(source_id, make_binary(BinaryKind::Div, right.left, left.left), next);
                    }
                }


                if (kind == BinaryKind::Add || kind == BinaryKind::Mul) {
                    if (left.tag == NodeTag::Binary &&
                        static_cast<BinaryKind>(left.op) == kind) {
                        const ExprId inner = materialize_auxiliary(
                            make_binary(kind, left.right, source.right));
                        if (inner != kNoExpr) {
                            emit(source_id, make_binary(kind, left.left, inner), next);
                        }
                    }
                    if (right.tag == NodeTag::Binary &&
                        static_cast<BinaryKind>(right.op) == kind) {
                        const ExprId inner = materialize_auxiliary(
                            make_binary(kind, source.left, right.left));
                        if (inner != kNoExpr) {
                            emit(source_id, make_binary(kind, inner, right.right), next);
                        }
                    }
                }

                if (kind == BinaryKind::Pow && left.tag == NodeTag::Binary &&
                    static_cast<BinaryKind>(left.op) == BinaryKind::Pow &&
                    arena_[left.left].value > 0.0) {
                    const ExprId exponent = materialize_auxiliary(
                        make_binary(BinaryKind::Mul, left.right, source.right));
                    if (exponent != kNoExpr) {
                        emit(source_id, make_binary(BinaryKind::Pow, left.left, exponent), next);
                    }
                }

                if ((kind == BinaryKind::Add || kind == BinaryKind::Sub) &&
                    left.tag == NodeTag::Binary && right.tag == NodeTag::Binary) {
                    const BinaryKind left_kind = static_cast<BinaryKind>(left.op);
                    const BinaryKind right_kind = static_cast<BinaryKind>(right.op);
                    if (left_kind == BinaryKind::Div && right_kind == BinaryKind::Div &&
                        arena_[left.right].hash == arena_[right.right].hash) {
                        const ExprId numerator = materialize_auxiliary(
                            make_binary(kind, left.left, right.left));
                        if (numerator != kNoExpr) {
                            emit(source_id, make_binary(BinaryKind::Div, numerator, left.right), next);
                        }
                    }
                    if (left_kind == BinaryKind::Mul && right_kind == BinaryKind::Mul) {
                        const std::array<ExprId, 2> left_factors{left.left, left.right};
                        const std::array<ExprId, 2> right_factors{right.left, right.right};
                        bool factored = false;
                        for (std::size_t li = 0; li < 2 && !factored; ++li) {
                            for (std::size_t ri = 0; ri < 2 && !factored; ++ri) {
                                if (arena_[left_factors[li]].hash != arena_[right_factors[ri]].hash) continue;
                                const ExprId left_remainder = left_factors[1 - li];
                                const ExprId right_remainder = right_factors[1 - ri];
                                const ExprId inner = materialize_auxiliary(
                                    make_binary(kind, left_remainder, right_remainder));
                                if (inner == kNoExpr) continue;
                                emit(source_id,
                                     make_binary(BinaryKind::Mul, left_factors[li], inner), next);
                                factored = true;
                            }
                        }
                    }
                }
            }
            frontier = std::move(next);
        }

        sort_touched_layers(touched_cost);
        if (cfg_.live && !live_candidates.empty()) {
            live_reporter.consider_batch(live_candidates, cfg_.max_cost);
            live_reporter.snapshot(cfg_.max_cost);
        }
        if (cfg_.verbose) {
            std::cerr << "egraph_seeds=" << seeds.size()
                      << " rewrites=" << stats_.egraph_rewrites
                      << " appended=" << stats_.egraph_candidates << '\n';
        }
    }

    void run_mcts_search(LiveTopReporter& live_reporter) {
        stats_.used_mcts = true;
        if (cfg_.mcts_iterations == 0 || cfg_.mcts_depth == 0 || cfg_.mcts_branching == 0) return;

        struct TreeNode {
            ExprId expression{kNoExpr};
            std::size_t parent{};
            std::vector<std::size_t> children;
            std::uint32_t visits{};
            double reward_sum{};
            unsigned depth{};
            std::uint32_t expansion_attempts{};
        };

        const double search_target = genetic_search_target();
        const auto reward_for = [&](ExprId id) {
            const Node& node = arena_[id];
            const double relative = std::abs(node.value - search_target) /
                                    std::max(1.0, std::abs(search_target));
            const double precision = -std::log10(std::max(relative, 1.0e-18));
            const double elegance = static_cast<double>(node.cost) + 0.12 * node.nodes +
                                    0.08 * node.depth;
            const double range_penalty = std::log1p(
                1.0e6 * genetic_range_violation(node.value - cfg_.target));
            double reward = precision - cfg_.mcts_elegance * elegance - 8.0 * range_penalty;
            if (constraint_satisfied(cfg_, node.constraint_state)) reward += 0.5;
            else reward -= 0.5;
            if (cfg_.error_range.contains(node.value - cfg_.target)) reward += 0.25;
            return reward;
        };

        std::vector<ExprId> ranked;
        ranked.reserve(arena_.size());
        for (ExprId id = 0; id < arena_.size(); ++id) {
            const Node& node = arena_[id];
            if (node.eligible && !node.depends_on_x && node.cost < cfg_.max_cost) ranked.push_back(id);
        }
        std::sort(ranked.begin(), ranked.end(), [&](ExprId left, ExprId right) {
            const double a = reward_for(left);
            const double b = reward_for(right);
            if (a != b) return a > b;
            const Node& x = arena_[left];
            const Node& y = arena_[right];
            if (x.cost != y.cost) return x.cost < y.cost;
            if (x.nodes != y.nodes) return x.nodes < y.nodes;
            return x.hash < y.hash;
        });

        const std::size_t root_limit = std::min<std::size_t>(
            ranked.size(), std::max<std::size_t>(8, std::min<std::size_t>(128, cfg_.mcts_branching * 4ULL)));
        std::vector<ExprId> roots;
        roots.reserve(root_limit);
        FastSet<std::uint64_t> root_shapes;
        root_shapes.reserve(root_limit * 2 + 1);
        for (ExprId id : ranked) {
            const std::uint64_t shape = candidate_shape_signature(candidate_from_node(id));
            if (!root_shapes.emplace(shape).second) continue;
            roots.push_back(id);
            if (roots.size() == root_limit) break;
        }
        if (roots.empty()) return;

        const auto source_layers = extended_layers_snapshot();
        FastMap<std::uint64_t, ExprId> structure_index = build_structure_index();
        std::vector<unsigned char> touched_cost(cfg_.max_cost + 1, 0);
        std::vector<TreeNode> tree;
        tree.reserve(cfg_.mcts_iterations + roots.size() + 1);
        tree.push_back({});
        tree[0].children.reserve(roots.size());
        for (ExprId id : roots) {
            const std::size_t index = tree.size();
            tree.push_back({id, 0, {}, 0, 0.0, 0, 0});
            tree[0].children.push_back(index);
        }

        const auto choose_child = [&](std::size_t parent_index) {
            const TreeNode& parent = tree[parent_index];
            std::size_t best = parent.children.front();
            double best_score = -std::numeric_limits<double>::infinity();
            const double logarithm = std::log(static_cast<double>(parent.visits) + 2.0);
            for (std::size_t child_index : parent.children) {
                const TreeNode& child = tree[child_index];
                const double score = child.visits == 0
                    ? std::numeric_limits<double>::infinity()
                    : child.reward_sum / child.visits + cfg_.mcts_exploration *
                          std::sqrt(logarithm / child.visits);
                if (score > best_score || (score == best_score && child_index < best)) {
                    best = child_index;
                    best_score = score;
                }
            }
            return best;
        };
        const auto backpropagate = [&](std::size_t index, double reward) {
            for (;;) {
                TreeNode& node = tree[index];
                ++node.visits;
                node.reward_sum += reward;
                if (index == 0) break;
                index = node.parent;
            }
        };

        const auto propose = [&](ExprId current_id, std::uint64_t random) -> std::optional<Candidate> {
            const Node& current = arena_[current_id];
            const bool can_unary = !unary_ops_.empty();
            const bool can_binary = !binary_ops_.empty();
            if (!can_unary && !can_binary) return std::nullopt;

            const bool choose_unary = can_unary &&
                (!can_binary || (random & 3ULL) == 0ULL);
            if (choose_unary) {
                const UnarySpec& operation = unary_ops_[static_cast<std::size_t>(random % unary_ops_.size())];
                const unsigned total_cost = static_cast<unsigned>(current.cost) + operation.cost;
                if (total_cost > cfg_.max_cost) return std::nullopt;
                ++stats_.attempted;
                ++stats_.mcts_expansions;
                ++stats_.by_cost[total_cost].attempted;
                auto candidate = apply_unary(
                    cfg_, arena_, operation, current_id, static_cast<std::uint16_t>(total_cost));
                if (candidate) {
                    ++stats_.valid;
                    ++stats_.by_cost[total_cost].valid;
                }
                return candidate;
            }

            const BinarySpec& operation = binary_ops_[static_cast<std::size_t>(random % binary_ops_.size())];
            if (static_cast<unsigned>(current.cost) + operation.cost + 1U > cfg_.max_cost) {
                return std::nullopt;
            }
            const bool current_on_left = is_commutative(operation.kind) || ((random >> 8U) & 1ULL) == 0ULL;
            const auto desired = current_on_left
                ? desired_right(operation.kind, current.value, search_target)
                : desired_left(operation.kind, current.value, search_target);
            const unsigned maximum_partner_cost = cfg_.max_cost - current.cost - operation.cost;

            ExprId partner = kNoExpr;
            std::vector<std::pair<double, ExprId>> near_candidates;
            near_candidates.reserve(static_cast<std::size_t>(maximum_partner_cost) * 4 + 4);
            if (desired && std::isfinite(*desired) && ((random >> 12U) & 3ULL) != 0ULL) {
                for (unsigned cost = 1; cost <= maximum_partner_cost && cost < source_layers.size(); ++cost) {
                    const auto& ids = source_layers[cost];
                    if (ids.empty()) continue;
                    const std::size_t position = lower_bound_value(ids, arena_, *desired);
                    const std::size_t radius = std::min<std::size_t>(
                        2, std::max<std::size_t>(1, cfg_.mcts_branching / 4));
                    const std::size_t begin = position > radius ? position - radius : 0;
                    const std::size_t end = std::min(ids.size(), position + radius + 1);
                    for (std::size_t index = begin; index < end; ++index) {
                        const ExprId id = ids[index];
                        const double error = std::abs(arena_[id].value - *desired) /
                                             std::max(1.0, std::abs(*desired));
                        const double score = error + 1.0e-5 * cfg_.mcts_elegance *
                                                       (arena_[id].cost + 0.1 * arena_[id].nodes);
                        near_candidates.emplace_back(score, id);
                    }
                }
                std::sort(near_candidates.begin(), near_candidates.end(), [&](const auto& left, const auto& right) {
                    if (left.first != right.first) return left.first < right.first;
                    const Node& a = arena_[left.second];
                    const Node& b = arena_[right.second];
                    if (a.cost != b.cost) return a.cost < b.cost;
                    if (a.nodes != b.nodes) return a.nodes < b.nodes;
                    return a.hash < b.hash;
                });
                if (!near_candidates.empty()) {
                    const std::size_t choice_count = std::min<std::size_t>(
                        cfg_.mcts_branching, near_candidates.size());
                    partner = near_candidates[
                        static_cast<std::size_t>((random >> 24U) % choice_count)].second;
                }
            }
            if (partner == kNoExpr) {
                std::vector<unsigned> available_costs;
                available_costs.reserve(maximum_partner_cost);
                for (unsigned cost = 1;
                     cost <= maximum_partner_cost && cost < source_layers.size(); ++cost) {
                    if (!source_layers[cost].empty()) available_costs.push_back(cost);
                }
                if (available_costs.empty()) return std::nullopt;
                const unsigned partner_cost = available_costs[
                    static_cast<std::size_t>((random >> 16U) % available_costs.size())];
                const auto& ids = source_layers[partner_cost];
                partner = ids[static_cast<std::size_t>((random >> 32U) % ids.size())];
            }

            const unsigned total_cost = static_cast<unsigned>(current.cost) +
                                        arena_[partner].cost + operation.cost;
            ++stats_.attempted;
            ++stats_.mcts_expansions;
            ++stats_.by_cost[total_cost].attempted;
            auto candidate = current_on_left
                ? apply_binary(cfg_, arena_, operation, current_id, partner,
                               static_cast<std::uint16_t>(total_cost))
                : apply_binary(cfg_, arena_, operation, partner, current_id,
                               static_cast<std::uint16_t>(total_cost));
            if (candidate) {
                ++stats_.valid;
                ++stats_.by_cost[total_cost].valid;
            }
            return candidate;
        };

        std::vector<Candidate> live_candidates;
        live_candidates.reserve(128);
        std::size_t stagnant = 0;
        for (std::size_t iteration = 0; iteration < cfg_.mcts_iterations; ++iteration) {
            std::size_t selected_index = 0;
            while (!tree[selected_index].children.empty()) {
                if (selected_index != 0) {
                    const TreeNode& selected = tree[selected_index];
                    const std::size_t width = std::min<std::size_t>(
                        cfg_.mcts_branching,
                        1 + static_cast<std::size_t>(std::sqrt(static_cast<double>(selected.visits + 1))));
                    if (selected.depth < cfg_.mcts_depth && selected.children.size() < width) break;
                }
                selected_index = choose_child(selected_index);
            }

            TreeNode& selected = tree[selected_index];
            if (selected_index == 0 || selected.depth >= cfg_.mcts_depth) {
                const double reward = selected_index == 0 ? -1.0 : reward_for(selected.expression);
                backpropagate(selected_index, reward);
                continue;
            }

            bool expanded = false;
            for (unsigned attempt = 0; attempt < std::max(4U, cfg_.mcts_branching); ++attempt) {
                const std::uint64_t random = mix64(
                    cfg_.mcts_seed ^ arena_[selected.expression].hash ^
                    (static_cast<std::uint64_t>(iteration) * 0x9e3779b97f4a7c15ULL) ^
                    static_cast<std::uint64_t>(selected.expansion_attempts++));
                const auto candidate = propose(selected.expression, random);
                if (!candidate) continue;
                bool newly_eligible = false;
                const ExprId id = materialize_candidate(
                    *candidate, structure_index, touched_cost, true, true, &newly_eligible);
                if (id == kNoExpr || !newly_eligible) continue;

                const std::size_t child_index = tree.size();
                tree.push_back({id, selected_index, {}, 0, 0.0, selected.depth + 1, 0});
                tree[selected_index].children.push_back(child_index);
                ++stats_.mcts_candidates;
                live_candidates.push_back(candidate_from_node(id));
                backpropagate(child_index, reward_for(id));
                expanded = true;
                stagnant = 0;
                break;
            }
            if (!expanded) {
                ++stagnant;
                backpropagate(selected_index, reward_for(selected.expression) - 0.25);
            }
            if (cfg_.live && live_candidates.size() >= 128) {
                live_reporter.consider_batch(live_candidates, cfg_.max_cost);
                live_candidates.clear();
            }
            if (stagnant >= 2048) break;
        }

        sort_touched_layers(touched_cost);
        if (cfg_.live && !live_candidates.empty()) {
            live_reporter.consider_batch(live_candidates, cfg_.max_cost);
        }
        if (cfg_.live) live_reporter.snapshot(cfg_.max_cost);
        if (cfg_.verbose) {
            std::cerr << "mcts_roots=" << roots.size()
                      << " expansions=" << stats_.mcts_expansions
                      << " appended=" << stats_.mcts_candidates << '\n';
        }
    }

    struct GeneticScore {
        double fitness{};
        double range_violation{};
        bool acceptable{};
    };

    double genetic_search_target() const {
        if (cfg_.error_range.contains(0.0)) return cfg_.target;

        double boundary = 0.0;
        double direction = 0.0;
        if (cfg_.error_range.lower >= 0.0 && std::isfinite(cfg_.error_range.lower)) {
            boundary = cfg_.error_range.lower;
            direction = std::numeric_limits<double>::infinity();
        } else if (cfg_.error_range.upper <= 0.0 && std::isfinite(cfg_.error_range.upper)) {
            boundary = cfg_.error_range.upper;
            direction = -std::numeric_limits<double>::infinity();
        } else {
            return cfg_.target;
        }

        double goal = cfg_.target + boundary;
        if (!std::isfinite(goal)) return cfg_.target;
        if (!cfg_.error_range.contains(goal - cfg_.target)) {
            goal = std::nextafter(goal, direction);
        }
        if (goal == cfg_.target || !cfg_.error_range.contains(goal - cfg_.target)) {
            goal = std::nextafter(cfg_.target, direction);
        }
        return std::isfinite(goal) ? goal : cfg_.target;
    }

    double genetic_range_violation(double signed_error) const {
        if (cfg_.error_range.contains(signed_error)) return 0.0;

        double violation = 0.0;
        if (signed_error < cfg_.error_range.lower ||
            (signed_error == cfg_.error_range.lower && !cfg_.error_range.include_lower)) {
            violation = cfg_.error_range.lower - signed_error;
        } else if (signed_error > cfg_.error_range.upper ||
                   (signed_error == cfg_.error_range.upper && !cfg_.error_range.include_upper)) {
            violation = signed_error - cfg_.error_range.upper;
        }
        const double scale = std::max(1.0, std::abs(cfg_.target));
        if (!(violation > 0.0) || !std::isfinite(violation)) {
            violation = std::numeric_limits<double>::epsilon() * scale;
        }
        return violation / scale;
    }

    GeneticScore make_genetic_score(ExprId id) const {
        const Node& node = arena_[id];
        const double scale = std::max(1.0, std::abs(cfg_.target));
        const double relative_error = std::abs(node.value - cfg_.target) / scale;
        const double precision_floor = std::max(1.0e-15, cfg_.epsilon / scale);
        const double accuracy = std::log10(std::max(relative_error, precision_floor));
        const double signed_error = node.value - cfg_.target;
        return {accuracy + cfg_.genetic_elegance * node.cost +
                    0.015 * node.depth + 0.002 * node.nodes,
                genetic_range_violation(signed_error),
                cfg_.error_range.contains(signed_error)};
    }

    bool genetic_score_better(ExprId a,
                              ExprId b,
                              const std::vector<GeneticScore>& scores) const {
        const GeneticScore& sa = scores[a];
        const GeneticScore& sb = scores[b];
        if (sa.acceptable != sb.acceptable) return sa.acceptable;
        if (!sa.acceptable && sa.range_violation != sb.range_violation) {
            return sa.range_violation < sb.range_violation;
        }
        if (sa.fitness != sb.fitness) return sa.fitness < sb.fitness;
        const Node& na = arena_[a];
        const Node& nb = arena_[b];
        if (na.cost != nb.cost) return na.cost < nb.cost;
        if (na.nodes != nb.nodes) return na.nodes < nb.nodes;
        if (na.depth != nb.depth) return na.depth < nb.depth;
        if (na.hash != nb.hash) return na.hash < nb.hash;
        return a < b;
    }

    std::vector<ExprId> select_genetic_population(
        std::vector<ExprId> candidates,
        std::size_t limit,
        const std::vector<GeneticScore>& scores,
        const std::vector<std::uint64_t>& shape_hashes) const {
        if (candidates.empty() || limit == 0) return {};

        const auto score_better = [&](ExprId a, ExprId b) {
            return genetic_score_better(a, b, scores);
        };
        if (candidates.size() <= limit) {
            std::sort(candidates.begin(), candidates.end(), score_better);
            return candidates;
        }

        std::vector<ExprId> selected;
        selected.reserve(limit);
        std::vector<unsigned char> selected_ids(arena_.size(), 0);
        const auto add = [&](ExprId id) {
            if (selected.size() < limit && !selected_ids[id]) {
                selected_ids[id] = 1;
                selected.push_back(id);
            }
        };

        std::vector<ExprId> by_score = candidates;
        std::sort(by_score.begin(), by_score.end(), score_better);
        const std::size_t score_quota = std::max<std::size_t>(1, limit / 2);
        for (std::size_t i = 0; i < score_quota && i < by_score.size(); ++i) add(by_score[i]);

        if (cfg_.genetic_novelty > 0.0) {
            FastMap<std::uint64_t, ExprId> best_by_shape;
            best_by_shape.reserve(std::min<std::size_t>(candidates.size(), limit * 2) + 1);
            for (ExprId id : candidates) {
                const std::uint64_t shape = shape_hashes[id];
                const auto found = best_by_shape.find(shape);
                if (found == best_by_shape.end()) best_by_shape.emplace(shape, id);
                else if (genetic_score_better(id, found->second, scores)) found->second = id;
            }
            std::vector<ExprId> shape_representatives;
            shape_representatives.reserve(best_by_shape.size());
            for (const auto& [_, id] : best_by_shape) shape_representatives.push_back(id);
            std::sort(shape_representatives.begin(), shape_representatives.end(), score_better);
            const std::size_t structure_quota = static_cast<std::size_t>(
                std::round(cfg_.genetic_novelty * static_cast<double>(limit)));
            const std::size_t structure_target = std::min(limit, score_quota + structure_quota);
            for (ExprId id : shape_representatives) {
                if (selected.size() >= structure_target) break;
                add(id);
            }
        }

        std::vector<ExprId> by_cost = candidates;
        std::sort(by_cost.begin(), by_cost.end(), [&](ExprId a, ExprId b) {
            const Node& na = arena_[a];
            const Node& nb = arena_[b];
            if (na.cost != nb.cost) return na.cost < nb.cost;
            return genetic_score_better(a, b, scores);
        });
        const std::size_t per_cost_quota = std::max<std::size_t>(
            1, limit / (2 * std::max<std::size_t>(1, cfg_.max_cost)));
        std::size_t begin = 0;
        while (begin < by_cost.size() && selected.size() < (limit * 3) / 4) {
            std::size_t end = begin + 1;
            while (end < by_cost.size() && arena_[by_cost[end]].cost == arena_[by_cost[begin]].cost) ++end;
            for (std::size_t i = begin; i < end && i < begin + per_cost_quota; ++i) add(by_cost[i]);
            begin = end;
        }

        std::vector<ExprId> by_value = candidates;
        std::sort(by_value.begin(), by_value.end(), [&](ExprId a, ExprId b) {
            if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
            if (arena_[a].hash != arena_[b].hash) return arena_[a].hash < arena_[b].hash;
            return a < b;
        });
        const std::size_t diversity_target = limit - selected.size();
        if (diversity_target == 1) {
            add(by_value[by_value.size() / 2]);
        } else if (diversity_target > 1) {
            for (std::size_t i = 0; i < diversity_target * 2 && selected.size() < limit; ++i) {
                const std::size_t position =
                    (i * (by_value.size() - 1)) / std::max<std::size_t>(1, diversity_target * 2 - 1);
                add(by_value[position]);
            }
        }
        for (ExprId id : by_score) {
            if (selected.size() == limit) break;
            add(id);
        }
        std::sort(selected.begin(), selected.end(), score_better);
        return selected;
    }

    void run_genetic_search(LiveTopReporter& live_reporter) {
        struct RepairFrame {
            NodeTag tag{NodeTag::Atom};
            std::uint8_t op{};
            std::uint16_t op_cost{};
            ExprId sibling{kNoExpr};
            bool hole_on_left{};
        };
        struct RepairPlan {
            std::array<RepairFrame, 8> frames{};
            ExprId replacement{kNoExpr};
            std::uint8_t frame_count{};
        };
        enum class TrialKind : std::uint8_t { Direct, Repair, Crossover };
        struct Trial {
            std::optional<Candidate> candidate;
            RepairPlan repair_plan;
            std::uint16_t cost{};
            TrialKind kind{TrialKind::Direct};
        };

        stats_.used_genetic = true;
        std::vector<ExprId> seeds;
        seeds.reserve(arena_.size());
        std::vector<std::vector<ExprId>> library_by_cost(cfg_.max_cost + 1);
        for (std::size_t i = 0; i < arena_.size(); ++i) {
            if (arena_[i].eligible) {
                const ExprId id = static_cast<ExprId>(i);
                seeds.push_back(id);
                if (arena_[i].cost <= cfg_.max_cost) library_by_cost[arena_[i].cost].push_back(id);
            }
        }
        for (auto& ids : library_by_cost) {
            std::sort(ids.begin(), ids.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                if (arena_[a].hash != arena_[b].hash) return arena_[a].hash < arena_[b].hash;
                return a < b;
            });
        }
        std::vector<GeneticScore> score_cache;
        score_cache.reserve(arena_.size() + cfg_.genetic_population);
        std::vector<std::uint64_t> shape_cache;
        shape_cache.reserve(arena_.size() + cfg_.genetic_population);
        const auto update_score_cache = [&]() {
            while (score_cache.size() < arena_.size()) {
                const ExprId id = static_cast<ExprId>(score_cache.size());
                if (arena_[id].eligible) {
                    score_cache.push_back(make_genetic_score(id));
                } else {
                    score_cache.push_back({std::numeric_limits<double>::infinity(),
                                           std::numeric_limits<double>::infinity(), false});
                }
            }
        };
        const auto update_shape_cache = [&]() {
            while (shape_cache.size() < arena_.size()) {
                const Node& node = arena_[shape_cache.size()];
                std::uint64_t shape = mix64(
                    static_cast<std::uint64_t>(node.tag) |
                    (static_cast<std::uint64_t>(node.op) << 8U));
                if (node.tag == NodeTag::Atom) {
                    shape = mix64(shape ^
                                  (static_cast<std::uint64_t>(node.atom_index) << 17U));
                } else if (node.tag == NodeTag::Unary) {
                    shape = mix64(shape ^ std::rotl(shape_cache[node.left], 19));
                } else {
                    std::uint64_t left_shape = shape_cache[node.left];
                    std::uint64_t right_shape = shape_cache[node.right];
                    if (is_commutative(static_cast<BinaryKind>(node.op)) &&
                        right_shape < left_shape) {
                        std::swap(left_shape, right_shape);
                    }
                    shape = mix64(shape ^ std::rotl(left_shape, 17) ^
                                  std::rotl(right_shape, 41));
                }
                shape_cache.push_back(shape);
            }
        };
        update_score_cache();
        update_shape_cache();
        std::vector<ExprId> population =
            select_genetic_population(std::move(seeds), cfg_.genetic_population,
                                      score_cache, shape_cache);
        if (population.empty()) return;

        std::vector<unsigned char> touched_cost(cfg_.max_cost + 1, 0);
        unsigned stagnant_generations = 0;
        const double search_target = genetic_search_target();
        const std::uint64_t target_seed = value_bucket(search_target, 52);
        const long double repair_cutoff = static_cast<long double>(cfg_.genetic_repair) *
                                          static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
        const long double crossover_cutoff = static_cast<long double>(cfg_.genetic_crossover) *
                                             static_cast<long double>(std::numeric_limits<std::uint64_t>::max());

        FastMap<std::uint64_t, ExprId> repair_auxiliary_ids;
        repair_auxiliary_ids.reserve(std::min<std::size_t>(cfg_.genetic_population, 16'384));

        const auto materialize_repair = [&](const RepairPlan& plan) -> std::optional<Candidate> {
            ExprId current = plan.replacement;
            for (std::size_t count = plan.frame_count; count > 0; --count) {
                const std::size_t frame_index = count - 1;
                const RepairFrame& frame = plan.frames[frame_index];
                std::optional<Candidate> candidate;
                if (frame.tag == NodeTag::Unary) {
                    const UnarySpec op{static_cast<UnaryKind>(frame.op), frame.op_cost};
                    const unsigned total_cost = static_cast<unsigned>(arena_[current].cost) + frame.op_cost;
                    if (total_cost > cfg_.max_cost) return std::nullopt;
                    candidate = apply_unary(cfg_, arena_, op, current,
                                            static_cast<std::uint16_t>(total_cost));
                } else if (frame.tag == NodeTag::Binary) {
                    const BinarySpec op{static_cast<BinaryKind>(frame.op), frame.op_cost};
                    const unsigned total_cost = static_cast<unsigned>(arena_[current].cost) +
                                                arena_[frame.sibling].cost + frame.op_cost;
                    if (total_cost > cfg_.max_cost) return std::nullopt;
                    candidate = frame.hole_on_left
                        ? apply_binary(cfg_, arena_, op, current, frame.sibling,
                                       static_cast<std::uint16_t>(total_cost))
                        : apply_binary(cfg_, arena_, op, frame.sibling, current,
                                       static_cast<std::uint16_t>(total_cost));
                }
                if (!candidate) return std::nullopt;
                if (frame_index == 0) return candidate;
                current = append_auxiliary(*candidate, repair_auxiliary_ids);
            }
            return std::nullopt;
        };

        for (unsigned generation = 0; generation < cfg_.genetic_generations; ++generation) {
            const auto generation_start = std::chrono::steady_clock::now();
            std::vector<unsigned> populated_costs;
            populated_costs.reserve(cfg_.max_cost);
            for (unsigned cost = 1; cost <= cfg_.max_cost; ++cost) {
                if (!library_by_cost[cost].empty()) populated_costs.push_back(cost);
            }

            const auto nearest_indexed = [&](double desired,
                                             unsigned budget,
                                             ExprId replaced,
                                             bool reject_roundoff_only) {
                ExprId best = kNoExpr;
                double best_distance = std::numeric_limits<double>::infinity();
                if (!std::isfinite(desired)) return best;
                const unsigned maximum_cost = std::min<unsigned>(budget, cfg_.max_cost);
                for (unsigned cost = 1; cost <= maximum_cost; ++cost) {
                    const auto& ids = library_by_cost[cost];
                    if (ids.empty()) continue;
                    const auto position = std::lower_bound(
                        ids.begin(), ids.end(), desired,
                        [&](ExprId id, double value) { return arena_[id].value < value; });
                    const std::size_t center = static_cast<std::size_t>(position - ids.begin());
                    for (int delta = -2; delta <= 2; ++delta) {
                        const long long candidate_index = static_cast<long long>(center) + delta;
                        if (candidate_index < 0 || candidate_index >= static_cast<long long>(ids.size())) continue;
                        const ExprId id = ids[static_cast<std::size_t>(candidate_index)];
                        if (id == replaced || (replaced != kNoExpr && arena_[id].hash == arena_[replaced].hash)) {
                            continue;
                        }
                        if (reject_roundoff_only && replaced != kNoExpr &&
                            arena_[id].cost >= arena_[replaced].cost) {
                            const double magnitude = std::max({1.0, std::abs(arena_[id].value),
                                                               std::abs(arena_[replaced].value)});
                            const double noise = 16.0 * std::numeric_limits<double>::epsilon() * magnitude;
                            if (std::abs(arena_[id].value - arena_[replaced].value) <= noise) continue;
                        }
                        const double distance = std::abs(arena_[id].value - desired);
                        if (best == kNoExpr || distance < best_distance ||
                            (distance == best_distance &&
                             genetic_score_better(id, best, score_cache))) {
                            best = id;
                            best_distance = distance;
                        }
                    }
                }
                return best;
            };

            std::vector<Trial> trials(cfg_.genetic_population);
            const auto generate_trial = [&](std::size_t trial_index) {
                std::uint64_t random = mix64(cfg_.genetic_seed ^ target_seed ^
                    (static_cast<std::uint64_t>(generation + 1) << 32U) ^ trial_index);
                auto draw = [&]() {
                    random = mix64(random);
                    return random;
                };
                const auto tournament_parent = [&](unsigned budget,
                                                   ExprId rejected = kNoExpr) {
                    ExprId best = kNoExpr;
                    ExprId random_valid = kNoExpr;
                    const unsigned draws = std::max(1U, cfg_.genetic_tournament);
                    for (unsigned sample = 0; sample < draws * 4U; ++sample) {
                        const ExprId id = population[draw() % population.size()];
                        if (id == rejected || arena_[id].cost > budget) continue;
                        if (random_valid == kNoExpr) random_valid = id;
                        if (best == kNoExpr || genetic_score_better(id, best, score_cache)) best = id;
                        if (sample + 1 >= draws && best != kNoExpr) break;
                    }
                    // A small deterministic exploration lane prevents the
                    // tournament from collapsing every island onto one elite.
                    return random_valid != kNoExpr && (draw() & 15U) == 0U
                        ? random_valid : best;
                };

                if (cfg_.genetic_repair > 0.0 &&
                    (cfg_.genetic_repair >= 1.0 ||
                     static_cast<long double>(draw()) <= repair_cutoff)) {
                    ExprId root = kNoExpr;
                    for (unsigned attempt = 0; attempt < 8; ++attempt) {
                        const ExprId id = tournament_parent(cfg_.max_cost);
                        if (id == kNoExpr) break;
                        if (arena_[id].tag != NodeTag::Atom && arena_[id].depth > 1) {
                            root = id;
                            break;
                        }
                    }
                    if (root != kNoExpr) {
                        Trial& trial = trials[trial_index];
                        RepairPlan plan;
                        ExprId current = root;
                        double desired = search_target;
                        const unsigned maximum_depth = std::min<unsigned>(
                            cfg_.genetic_repair_depth, std::max<unsigned>(1, arena_[root].depth - 1U));
                        const unsigned requested_depth = 1U + static_cast<unsigned>(draw() % maximum_depth);
                        bool invertible = true;

                        for (unsigned depth = 0; depth < requested_depth; ++depth) {
                            const Node& parent = arena_[current];
                            if (parent.tag == NodeTag::Atom || plan.frame_count == plan.frames.size()) break;
                            RepairFrame frame;
                            frame.tag = parent.tag;
                            frame.op = parent.op;
                            if (parent.tag == NodeTag::Unary) {
                                const ExprId child = parent.left;
                                if (parent.cost < arena_[child].cost) {
                                    invertible = false;
                                    break;
                                }
                                frame.op_cost = static_cast<std::uint16_t>(parent.cost - arena_[child].cost);
                                const auto inverse_values = inverse_unary_values(
                                    static_cast<UnaryKind>(parent.op), desired);
                                if (inverse_values.empty()) {
                                    invertible = false;
                                    break;
                                }
                                desired = inverse_values[draw() % inverse_values.size()];
                                current = child;
                            } else {
                                const bool hole_on_left = (draw() & 1U) == 0U;
                                const ExprId child = hole_on_left ? parent.left : parent.right;
                                frame.sibling = hole_on_left ? parent.right : parent.left;
                                frame.hole_on_left = hole_on_left;
                                const unsigned child_cost = static_cast<unsigned>(arena_[parent.left].cost) +
                                                            arena_[parent.right].cost;
                                if (child_cost > parent.cost) {
                                    invertible = false;
                                    break;
                                }
                                frame.op_cost = static_cast<std::uint16_t>(
                                    parent.cost - child_cost);
                                const auto inverse = hole_on_left
                                    ? desired_left(static_cast<BinaryKind>(parent.op),
                                                   arena_[frame.sibling].value, desired)
                                    : desired_right(static_cast<BinaryKind>(parent.op),
                                                    arena_[frame.sibling].value, desired);
                                if (!inverse || !std::isfinite(*inverse)) {
                                    invertible = false;
                                    break;
                                }
                                desired = *inverse;
                                current = child;
                            }
                            plan.frames[plan.frame_count++] = frame;
                        }

                        if (invertible && plan.frame_count > 0 && std::isfinite(desired)) {
                            if (arena_[current].cost <= arena_[root].cost) {
                                const unsigned context_cost = arena_[root].cost - arena_[current].cost;
                                if (context_cost < cfg_.max_cost) {
                                    plan.replacement = nearest_indexed(
                                        desired, cfg_.max_cost - context_cost, current, true);
                                    if (plan.replacement != kNoExpr) {
                                        trial.cost = static_cast<std::uint16_t>(
                                            context_cost + arena_[plan.replacement].cost);
                                        trial.repair_plan = plan;
                                        trial.kind = TrialKind::Repair;
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }

                if (cfg_.genetic_crossover > 0.0 &&
                    (cfg_.genetic_crossover >= 1.0 ||
                     static_cast<long double>(draw()) <= crossover_cutoff)) {
                    const ExprId root = tournament_parent(cfg_.max_cost);
                    if (root != kNoExpr && arena_[root].tag != NodeTag::Atom) {
                        RepairPlan plan;
                        ExprId current = root;
                        const unsigned maximum_depth = std::min<unsigned>(
                            cfg_.genetic_repair_depth,
                            std::max<unsigned>(1, arena_[root].depth - 1U));
                        const unsigned requested_depth =
                            1U + static_cast<unsigned>(draw() % maximum_depth);
                        bool valid_path = true;
                        for (unsigned depth = 0; depth < requested_depth; ++depth) {
                            const Node& parent = arena_[current];
                            if (parent.tag == NodeTag::Atom ||
                                plan.frame_count == plan.frames.size()) break;
                            RepairFrame frame;
                            frame.tag = parent.tag;
                            frame.op = parent.op;
                            if (parent.tag == NodeTag::Unary) {
                                const ExprId child = parent.left;
                                if (parent.cost < arena_[child].cost) {
                                    valid_path = false;
                                    break;
                                }
                                frame.op_cost = static_cast<std::uint16_t>(
                                    parent.cost - arena_[child].cost);
                                current = child;
                            } else {
                                const bool hole_on_left = (draw() & 1U) == 0U;
                                const ExprId child = hole_on_left ? parent.left : parent.right;
                                frame.sibling = hole_on_left ? parent.right : parent.left;
                                frame.hole_on_left = hole_on_left;
                                const unsigned children_cost =
                                    static_cast<unsigned>(arena_[parent.left].cost) +
                                    arena_[parent.right].cost;
                                if (children_cost > parent.cost) {
                                    valid_path = false;
                                    break;
                                }
                                frame.op_cost = static_cast<std::uint16_t>(
                                    parent.cost - children_cost);
                                current = child;
                            }
                            plan.frames[plan.frame_count++] = frame;
                        }

                        if (valid_path && plan.frame_count > 0 &&
                            arena_[current].cost <= arena_[root].cost) {
                            const unsigned context_cost = arena_[root].cost - arena_[current].cost;
                            if (context_cost < cfg_.max_cost) {
                                const unsigned replacement_budget = cfg_.max_cost - context_cost;
                                for (unsigned attempt = 0; attempt < 12; ++attempt) {
                                    ExprId replacement = tournament_parent(replacement_budget, current);
                                    if (replacement == kNoExpr ||
                                        arena_[replacement].hash == arena_[current].hash) {
                                        const auto end = std::upper_bound(
                                            populated_costs.begin(), populated_costs.end(),
                                            replacement_budget);
                                        const std::size_t available =
                                            static_cast<std::size_t>(end - populated_costs.begin());
                                        if (available == 0) break;
                                        const unsigned cost = populated_costs[draw() % available];
                                        const auto& ids = library_by_cost[cost];
                                        replacement = ids[draw() % ids.size()];
                                    }
                                    if (replacement == current ||
                                        arena_[replacement].hash == arena_[current].hash) continue;
                                    plan.replacement = replacement;
                                    Trial& trial = trials[trial_index];
                                    trial.cost = static_cast<std::uint16_t>(
                                        context_cost + arena_[replacement].cost);
                                    trial.repair_plan = plan;
                                    trial.kind = TrialKind::Crossover;
                                    return;
                                }
                            }
                        }
                    }
                }

                const bool choose_unary = !unary_ops_.empty() &&
                    (binary_ops_.empty() || draw() % 5U == 0U);
                if (choose_unary) {
                    for (unsigned attempt = 0; attempt < 8; ++attempt) {
                        const ExprId parent = tournament_parent(cfg_.max_cost);
                        if (parent == kNoExpr) break;
                        const UnarySpec& op = unary_ops_[draw() % unary_ops_.size()];
                        const unsigned total_cost = arena_[parent].cost + op.cost;
                        if (total_cost > cfg_.max_cost) continue;
                        Trial& trial = trials[trial_index];
                        trial.cost = static_cast<std::uint16_t>(total_cost);
                        trial.candidate = apply_unary(cfg_, arena_, op, parent, trial.cost);
                        if (trial.candidate) return;
                    }
                    return;
                }

                if (binary_ops_.empty()) return;
                for (unsigned attempt = 0; attempt < 12; ++attempt) {
                    const ExprId anchor = tournament_parent(cfg_.max_cost);
                    if (anchor == kNoExpr) break;
                    const BinarySpec& op = binary_ops_[draw() % binary_ops_.size()];
                    const unsigned fixed_cost = static_cast<unsigned>(arena_[anchor].cost) + op.cost;
                    if (fixed_cost >= cfg_.max_cost) continue;
                    const unsigned partner_budget = cfg_.max_cost - fixed_cost;
                    const bool anchor_on_left = is_commutative(op.kind) || (draw() & 1U) == 0;

                    ExprId partner = kNoExpr;
                    if ((draw() & 1U) == 0) {
                        const auto desired = anchor_on_left
                            ? desired_right(op.kind, arena_[anchor].value, search_target)
                            : desired_left(op.kind, arena_[anchor].value, search_target);
                        if (desired && std::isfinite(*desired)) {
                            partner = nearest_indexed(*desired, partner_budget, kNoExpr, false);
                        }
                    }
                    if (partner == kNoExpr) {
                        const auto end = std::upper_bound(populated_costs.begin(), populated_costs.end(),
                                                          partner_budget);
                        const std::size_t available = static_cast<std::size_t>(end - populated_costs.begin());
                        if (available > 0) {
                            const unsigned cost = populated_costs[draw() % available];
                            const auto& ids = library_by_cost[cost];
                            partner = ids[draw() % ids.size()];
                        }
                    }
                    if (partner == kNoExpr) continue;

                    const unsigned total_cost = arena_[anchor].cost + arena_[partner].cost + op.cost;
                    Trial& trial = trials[trial_index];
                    trial.cost = static_cast<std::uint16_t>(total_cost);
                    trial.candidate = anchor_on_left
                        ? apply_binary(cfg_, arena_, op, anchor, partner, trial.cost)
                        : apply_binary(cfg_, arena_, op, partner, anchor, trial.cost);
                    if (trial.candidate) return;
                }
            };

            const std::size_t block_count = std::min<std::size_t>(
                trials.size(), std::max<std::size_t>(1, static_cast<std::size_t>(cfg_.threads) * 4));
            const std::size_t block_size = (trials.size() + block_count - 1) / block_count;
            executor_.run(block_count, [&](std::size_t block) {
                const std::size_t begin = block * block_size;
                const std::size_t end = std::min(trials.size(), begin + block_size);
                for (std::size_t trial_index = begin; trial_index < end; ++trial_index) {
                    generate_trial(trial_index);
                }
            });

            std::vector<ExprId> new_ids;
            new_ids.reserve(trials.size());
            std::vector<unsigned char> library_touched(cfg_.max_cost + 1, 0);
            std::vector<Candidate> live_candidates;
            if (cfg_.live) live_candidates.reserve(trials.size());
            for (Trial& trial : trials) {
                if (trial.cost == 0) continue;
                if (trial.kind != TrialKind::Direct) {
                    if (trial.kind == TrialKind::Repair) ++stats_.genetic_repairs;
                    else ++stats_.genetic_crossovers;
                    trial.candidate = materialize_repair(trial.repair_plan);
                    if (trial.candidate && trial.candidate->cost != trial.cost) {
                        trial.candidate.reset();
                    }
                }
                ++stats_.attempted;
                ++stats_.by_cost[trial.cost].attempted;
                if (!trial.candidate) continue;
                ++stats_.valid;
                ++stats_.by_cost[trial.cost].valid;
                Candidate& candidate = *trial.candidate;
                if (cfg_.live) live_candidates.push_back(candidate);
                const auto key = state_bucket(candidate.value, candidate.derivative,
                                              candidate.depends_on_x, candidate.constraint_state,
                                              state_value_bits(cfg_), false);
                if (seen_.find(key) != seen_.end()) continue;
                const ExprId id = append_candidate_node(candidate, true);
                layers_[candidate.cost].push_back(id);
                seen_.emplace(key);
                touched_cost[candidate.cost] = 1;
                library_touched[candidate.cost] = 1;
                library_by_cost[candidate.cost].push_back(id);
                new_ids.push_back(id);
                if (trial.kind == TrialKind::Repair) ++stats_.genetic_repairs_kept;
                else if (trial.kind == TrialKind::Crossover) ++stats_.genetic_crossovers_kept;
            }
            if (cfg_.live) live_reporter.consider_batch(live_candidates, cfg_.max_cost);

            stats_.kept += new_ids.size();
            if (new_ids.empty()) ++stagnant_generations;
            else stagnant_generations = 0;
            population.insert(population.end(), new_ids.begin(), new_ids.end());
            for (unsigned cost = 1; cost <= cfg_.max_cost; ++cost) {
                if (!library_touched[cost]) continue;
                auto& ids = library_by_cost[cost];
                std::sort(ids.begin(), ids.end(), [&](ExprId a, ExprId b) {
                    if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                    if (arena_[a].hash != arena_[b].hash) return arena_[a].hash < arena_[b].hash;
                    return a < b;
                });
            }
            update_score_cache();
            update_shape_cache();
            population = select_genetic_population(
                std::move(population), cfg_.genetic_population, score_cache, shape_cache);
            stats_.genetic_generations = generation + 1;

            if (cfg_.verbose) {
                double best = std::numeric_limits<double>::infinity();
                for (ExprId id : population) best = std::min(best, std::abs(arena_[id].value - cfg_.target));
                std::cerr << "genetic_generation=" << (generation + 1) << " population=" << population.size()
                          << " appended=" << new_ids.size() << " best_error=" << std::scientific << best
                          << " time=" << std::fixed << std::setprecision(3)
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - generation_start).count()
                          << "s\n";
            }
            if (cfg_.live) live_reporter.snapshot(cfg_.max_cost);
            if (stagnant_generations >= 8) break;
        }

        for (unsigned cost = 1; cost < layers_.size(); ++cost) {
            if (!touched_cost[cost]) continue;
            auto& layer = layers_[cost];
            std::sort(layer.begin(), layer.end(), [&](ExprId a, ExprId b) {
                if (arena_[a].value != arena_[b].value) return arena_[a].value < arena_[b].value;
                return arena_[a].hash < arena_[b].hash;
            });
            stats_.by_cost[cost].kept = layer.size();
        }
    }

    struct Partition {
        std::size_t op_index{};
        std::uint16_t left_cost{};
        std::uint16_t right_cost{};
        bool equal_commutative{};
        std::uint64_t potential{};
        std::uint64_t quota{};
    };

    std::vector<GenTask> build_tasks(std::uint16_t total_cost) const {
        std::vector<GenTask> tasks;
        const std::size_t base_chunks = std::max<std::size_t>(1, cfg_.task_chunks);

        for (std::size_t op_index = 0; op_index < unary_ops_.size(); ++op_index) {
            const auto& op = unary_ops_[op_index];
            if (op.cost >= total_cost) continue;
            const std::uint16_t child_cost = total_cost - op.cost;
            const auto& ids = layers_[child_cost];
            if (ids.empty()) continue;
            const std::size_t chunks = std::min(ids.size(), base_chunks);
            const std::size_t chunk_size = (ids.size() + chunks - 1) / chunks;
            for (std::size_t begin = 0; begin < ids.size(); begin += chunk_size) {
                tasks.push_back({GenTask::Type::Unary, op_index, child_cost, 0, begin,
                                 std::min(ids.size(), begin + chunk_size), true, false, 1, total_cost});
            }
        }

        std::vector<Partition> partitions;
        for (std::size_t op_index = 0; op_index < binary_ops_.size(); ++op_index) {
            const auto& op = binary_ops_[op_index];
            if (op.cost + 2 > total_cost) continue;
            const std::uint16_t remainder = total_cost - op.cost;
            if (is_commutative(op.kind)) {
                for (std::uint16_t lc = 1; lc <= remainder / 2; ++lc) {
                    const std::uint16_t rc = remainder - lc;
                    if (layers_[lc].empty() || layers_[rc].empty()) continue;
                    const std::uint64_t nl = layers_[lc].size();
                    const std::uint64_t nr = layers_[rc].size();
                    const bool equal = lc == rc;
                    const std::uint64_t potential = equal ? saturating_triangle(nl)
                                                             : saturating_mul(nl, nr);
                    partitions.push_back({op_index, lc, rc, equal, potential, 0});
                }
            } else {
                for (std::uint16_t lc = 1; lc < remainder; ++lc) {
                    const std::uint16_t rc = remainder - lc;
                    if (layers_[lc].empty() || layers_[rc].empty()) continue;
                    partitions.push_back({op_index, lc, rc, false,
                                          saturating_mul(layers_[lc].size(), layers_[rc].size()), 0});
                }
            }
        }

        std::uint64_t total_potential = 0;
        long double total_weight = 0.0L;
        for (const auto& p : partitions) {
            total_potential = saturating_add(total_potential, p.potential);
            total_weight += std::sqrt(static_cast<long double>(std::max<std::uint64_t>(1, p.potential)));
        }
        const bool enumerate_all = total_potential <= cfg_.pair_budget;
        for (auto& p : partitions) {
            if (enumerate_all) {
                p.quota = p.potential;
            } else {
                const long double share = static_cast<long double>(cfg_.pair_budget) *
                                          std::sqrt(static_cast<long double>(std::max<std::uint64_t>(1, p.potential))) /
                                          std::max(1.0L, total_weight);
                p.quota = std::min<std::uint64_t>(p.potential, std::max<std::uint64_t>(128, static_cast<std::uint64_t>(share)));
            }
        }

        for (const auto& p : partitions) {
            const auto& left_ids = layers_[p.left_cost];
            const auto& right_ids = layers_[p.right_cost];
            const bool full = p.potential <= p.quota;
            const bool reverse = cfg_.bidirectional && !full &&
                                 !is_commutative(binary_ops_[p.op_index].kind);
            const std::uint64_t forward_quota = reverse ? (p.quota + 1) / 2 : p.quota;
            const std::uint64_t reverse_quota = reverse ? p.quota / 2 : 0;

            auto add_direction = [&](bool reverse_task, std::uint64_t quota) {
                const std::size_t outer_size = reverse_task ? right_ids.size() : left_ids.size();
                const std::size_t samples = full ? 0 : static_cast<std::size_t>(quota / outer_size);
                const std::size_t direction_chunks = reverse_task ? std::max<std::size_t>(1, base_chunks / 2)
                                                                   : base_chunks;
                const std::size_t chunks = std::min(outer_size, direction_chunks);
                const std::size_t chunk_size = (outer_size + chunks - 1) / chunks;
                for (std::size_t begin = 0; begin < outer_size; begin += chunk_size) {
                    tasks.push_back({GenTask::Type::Binary, p.op_index, p.left_cost, p.right_cost, begin,
                                     std::min(outer_size, begin + chunk_size), full, p.equal_commutative,
                                     samples, total_cost, reverse_task});
                }
            };

            add_direction(false, forward_quota);
            if (reverse) add_direction(true, reverse_quota);
        }
        return tasks;
    }

    TaskResult process_task(const GenTask& task) const {
        const std::size_t soft_cap = std::max<std::size_t>(cfg_.beam, 256);
        std::uint64_t candidate_upper_bound = task.end - task.begin;
        if (task.type == GenTask::Type::Binary) {
            const std::uint64_t outer_count = task.end - task.begin;
            if (task.full) {
                const auto inner_cost = task.reverse ? task.left_cost : task.right_cost;
                candidate_upper_bound = saturating_mul(outer_count, layers_[inner_cost].size());
            } else {
                std::uint64_t per_outer = saturating_add(task.samples_per_outer, 5);
                per_outer = saturating_add(per_outer, cfg_.explore_pairs);
                if (!task.reverse && binary_ops_[task.op_index].kind == BinaryKind::Pow) {
                    per_outer = saturating_add(per_outer, 8U * 5U);
                }
                candidate_upper_bound = saturating_mul(outer_count, per_outer);
            }
        }
        const std::uint64_t maximum_table_size = saturating_add(saturating_mul(soft_cap, 2), 1);
        const std::size_t reserve_hint = static_cast<std::size_t>(
            std::max<std::uint64_t>(1, std::min(candidate_upper_bound, maximum_table_size)));
        CandidateCollector collector(cfg_.target, state_value_bits(cfg_), soft_cap, cfg_.equations, reserve_hint,
                                     cfg_.pareto_slots, pareto_extra_cap());
        TaskResult result;

        if (task.type == GenTask::Type::Unary) {
            const auto& ids = layers_[task.left_cost];
            const auto& op = unary_ops_[task.op_index];
            for (std::size_t i = task.begin; i < task.end; ++i) {
                ++result.attempted;
                if (auto candidate = apply_unary(cfg_, arena_, op, ids[i], task.total_cost)) {
                    ++result.valid;
                    const auto key = state_bucket(candidate->value, candidate->derivative,
                                                  candidate->depends_on_x, candidate->constraint_state,
                                                  state_value_bits(cfg_), cfg_.equations);
                    if (seen_.find(key) == seen_.end()) {
                        collector.consider_with_key(*candidate, key);
                    } else {
                        collector.consider_extra_only(*candidate);
                    }
                }
            }
            result.candidates = collector.take(std::max<std::size_t>(cfg_.beam, 256));
            result.extra_candidates = collector.take_extras(pareto_extra_cap());
            return result;
        }

        const auto& left_ids = layers_[task.left_cost];
        const auto& right_ids = layers_[task.right_cost];
        const auto& op = binary_ops_[task.op_index];
        const std::uint64_t task_potential = saturating_mul(
            static_cast<std::uint64_t>(task.end - task.begin),
            static_cast<std::uint64_t>(task.reverse ? left_ids.size() : right_ids.size()));
        const bool equation_exhaustive = cfg_.equations &&
            cfg_.equation_search == EquationSearchMode::Exhaustive &&
            task_potential <= std::max<std::uint64_t>(1, cfg_.pair_budget);

        auto evaluate_pair = [&](ExprId left, ExprId right) {
            ++result.attempted;
            if (auto candidate = apply_binary(cfg_, arena_, op, left, right, task.total_cost)) {
                ++result.valid;
                const auto key = state_bucket(candidate->value, candidate->derivative,
                                              candidate->depends_on_x, candidate->constraint_state,
                                              state_value_bits(cfg_), cfg_.equations);
                if (seen_.find(key) == seen_.end()) {
                    collector.consider_with_key(*candidate, key);
                } else {
                    collector.consider_extra_only(*candidate);
                }
            }
        };

        const auto& outer_ids = task.reverse ? right_ids : left_ids;
        const auto& inner_ids = task.reverse ? left_ids : right_ids;
        std::vector<std::size_t> js;
        const std::size_t nearby_capacity =
            (!task.reverse && op.kind == BinaryKind::Pow) ? 5U + 8U * 5U : 5U;
        js.reserve(task.samples_per_outer + nearby_capacity + cfg_.explore_pairs);

        // These exponent neighborhoods depend only on the value-sorted inner
        // layer, yet this loop used to binary-search and rebuild them for every
        // outer expression.  Cache their exact sorted union once per task.
        std::vector<std::size_t> useful_exponent_indices;
        if (!task.full && !equation_exhaustive && !task.reverse &&
            op.kind == BinaryKind::Pow) {
            static constexpr double useful_exponents[] = {
                -3.0, -2.0, -1.0, -0.5, 0.0, 0.5, 2.0, 3.0
            };
            useful_exponent_indices.reserve(8U * cfg_.inverse_neighbors);
            for (const double exponent : useful_exponents) {
                add_window_indices(useful_exponent_indices, inner_ids, arena_, exponent, 0,
                                   cfg_.inverse_neighbors);
            }
            std::sort(useful_exponent_indices.begin(), useful_exponent_indices.end());
            useful_exponent_indices.erase(
                std::unique(useful_exponent_indices.begin(), useful_exponent_indices.end()),
                useful_exponent_indices.end());
        }
        const bool near_window_only = !cfg_.equations && task.samples_per_outer == 0 &&
                                      cfg_.explore_pairs == 0 &&
                                      useful_exponent_indices.empty();
        for (std::size_t i = task.begin; i < task.end; ++i) {
            const std::size_t lower = !task.reverse && task.equal_commutative ? i : 0;
            if (lower >= inner_ids.size()) continue;
            if (task.full || equation_exhaustive) {
                for (std::size_t j = lower; j < inner_ids.size(); ++j) {
                    evaluate_pair(outer_ids[i], inner_ids[j]);
                }
                continue;
            }

            js.clear();
            const double outer_value = arena_[outer_ids[i]].value;
            const auto desired = task.reverse ? desired_left(op.kind, outer_value, cfg_.target)
                                              : desired_right(op.kind, outer_value, cfg_.target);
            if (cfg_.equations) {
                // For an equality the useful partner is normally near the
                // outer expression's value, not near TARGET.  Keep the old
                // target-directed window too: it remains useful for equations
                // such as x^2 = 2 where one side is the requested expansion
                // point and the other side is a constant.
                add_window_indices(js, inner_ids, arena_, outer_value, lower,
                                   cfg_.equation_neighbors);
            }
            if (desired) {
                add_window_indices(js, inner_ids, arena_, *desired, lower, cfg_.inverse_neighbors);
            }
            if (!useful_exponent_indices.empty()) {
                js.insert(js.end(), useful_exponent_indices.begin(), useful_exponent_indices.end());
            }
            if (near_window_only) {
                // A single contiguous target window is already sorted and
                // unique; avoid millions of tiny sort/unique calls.
                for (const std::size_t j : js) {
                    if (task.reverse) evaluate_pair(inner_ids[j], outer_ids[i]);
                    else evaluate_pair(outer_ids[i], inner_ids[j]);
                }
                continue;
            }

            const std::size_t range = inner_ids.size() - lower;
            std::size_t samples = std::min(task.samples_per_outer, range);
            if (cfg_.equations && cfg_.equation_search != EquationSearchMode::Stable) {
                const std::size_t multiplier = cfg_.equation_search == EquationSearchMode::Exhaustive ? 8U : 4U;
                const std::size_t requested = std::max<std::size_t>(
                    32U, std::min<std::size_t>(512U,
                        cfg_.equation_neighbors * multiplier));
                samples = std::max(samples, std::min(range, requested));
            }
            std::uint64_t seed = mix64(arena_[outer_ids[i]].hash ^
                                       (static_cast<std::uint64_t>(op.kind) << 56U) ^
                                       (static_cast<std::uint64_t>(task.reverse) << 55U) ^ task.total_cost);
            for (std::size_t k = 0; k < samples; ++k) {
                const std::size_t cell_begin = (k * range) / samples;
                const std::size_t cell_end = ((k + 1) * range) / samples;
                const std::size_t width = std::max<std::size_t>(1, cell_end - cell_begin);
                seed = mix64(seed + k);
                js.push_back(lower + cell_begin + static_cast<std::size_t>(seed % width));
            }
            // Near-target sampling is excellent for precision, but it can miss
            // unusual identities whose partner is not numerically nearby. Add
            // deterministic semantic exploration samples without introducing a
            // runtime RNG or making repeated runs non-reproducible.
            for (std::size_t k = 0; k < cfg_.explore_pairs; ++k) {
                seed = mix64(seed ^ (0xd1b54a32d192ed03ULL + k));
                js.push_back(lower + static_cast<std::size_t>(seed % range));
            }
            std::sort(js.begin(), js.end());
            js.erase(std::unique(js.begin(), js.end()), js.end());
            for (const std::size_t j : js) {
                if (task.reverse) evaluate_pair(inner_ids[j], outer_ids[i]);
                else evaluate_pair(outer_ids[i], inner_ids[j]);
            }
        }

        result.candidates = collector.take(std::max<std::size_t>(cfg_.beam, 256));
        result.extra_candidates = collector.take_extras(pareto_extra_cap());
        return result;
    }

    Config cfg_;
    ParallelExecutor executor_;
    std::vector<AtomSpec> atoms_;
    std::vector<UnarySpec> unary_ops_;
    std::vector<BinarySpec> binary_ops_;
    std::vector<Node> arena_;
    std::vector<std::vector<ExprId>> layers_;
    std::vector<std::vector<ExprId>> extra_layers_;
    FastSet<std::uint64_t> seen_;
    FastSet<std::uint64_t> extra_seen_;
    SearchStats stats_;
};

struct Rendered {
    std::string text;
    int precedence{};
};

struct RenderedLatex {
    std::string text;
    int precedence{};
};

static std::string parenthesize(const Rendered& r, bool needed) {
    return needed ? "(" + r.text + ")" : r.text;
}

static Rendered render_expression_impl(const std::vector<AtomSpec>& atoms,
                                       const std::vector<Node>& arena,
                                       ExprId id);

static RenderedLatex render_expression_latex_impl(const std::vector<AtomSpec>& atoms,
                                                  const std::vector<Node>& arena,
                                                  ExprId id);

static Rendered render_node(const std::vector<AtomSpec>& atoms,
                            const std::vector<Node>& arena,
                            const Node& node) {
    if (node.tag == NodeTag::Atom) return {atoms[node.atom_index].text, 100};

    if (node.tag == NodeTag::Unary) {
        const auto kind = static_cast<UnaryKind>(node.op);
        const Rendered child = render_expression_impl(atoms, arena, node.left);
        switch (kind) {
            case UnaryKind::Neg:
                return {"-" + parenthesize(child, child.precedence < 25), 25};
            case UnaryKind::Inv:
                return {"inv(" + child.text + ")", 80};
            case UnaryKind::Sqrt:
                return {"sqrt(" + child.text + ")", 80};
            case UnaryKind::Cbrt:
                return {"cbrt(" + child.text + ")", 80};
            case UnaryKind::Sqr:
                return {"sqr(" + child.text + ")", 80};
            case UnaryKind::Cube:
                return {"cube(" + child.text + ")", 80};
            case UnaryKind::Ln:
                return {"ln(" + child.text + ")", 80};
            case UnaryKind::Log10:
                return {"log10(" + child.text + ")", 80};
            case UnaryKind::Exp:
                return {"exp(" + child.text + ")", 80};
            case UnaryKind::Sin:
                return {"sin(" + child.text + ")", 80};
            case UnaryKind::Cos:
                return {"cos(" + child.text + ")", 80};
            case UnaryKind::Tan:
                return {"tan(" + child.text + ")", 80};
            case UnaryKind::Asin:
                return {"asin(" + child.text + ")", 80};
            case UnaryKind::Acos:
                return {"acos(" + child.text + ")", 80};
            case UnaryKind::Atan:
                return {"atan(" + child.text + ")", 80};
            case UnaryKind::Sinh:
                return {"sinh(" + child.text + ")", 80};
            case UnaryKind::Cosh:
                return {"cosh(" + child.text + ")", 80};
            case UnaryKind::Tanh:
                return {"tanh(" + child.text + ")", 80};
            case UnaryKind::Asinh:
                return {"asinh(" + child.text + ")", 80};
            case UnaryKind::Acosh:
                return {"acosh(" + child.text + ")", 80};
            case UnaryKind::Atanh:
                return {"atanh(" + child.text + ")", 80};
            case UnaryKind::Gamma:
                return {"gamma(" + child.text + ")", 80};
            case UnaryKind::Abs:
                return {"abs(" + child.text + ")", 80};
            case UnaryKind::Fact:
                return {parenthesize(child, child.precedence < 80) + "!", 80};
            default:
                if (const auto custom = custom_unary_index(kind)) {
                    const auto& operation = extension_registry().unary_operations()[*custom];
                    if (operation.render_text) return {operation.render_text(child.text), 80};
                    return {operation.name + "(" + child.text + ")", 80};
                }
                break;
        }
        throw std::runtime_error("未知一元表达式节点");
    }

    const auto kind = static_cast<BinaryKind>(node.op);
    const Rendered left = render_expression_impl(atoms, arena, node.left);
    const Rendered right = render_expression_impl(atoms, arena, node.right);
    switch (kind) {
        case BinaryKind::Add:
            return {parenthesize(left, left.precedence < 10) + "+" + parenthesize(right, right.precedence < 10), 10};
        case BinaryKind::Sub:
            return {parenthesize(left, left.precedence < 10) + "-" + parenthesize(right, right.precedence <= 10), 10};
        case BinaryKind::Mul:
            return {parenthesize(left, left.precedence < 20) + "×" + parenthesize(right, right.precedence < 20), 20};
        case BinaryKind::Div:
            return {parenthesize(left, left.precedence < 20) + "/" + parenthesize(right, right.precedence <= 20), 20};
        case BinaryKind::Pow:
            return {parenthesize(left, left.precedence <= 30) + "^" + parenthesize(right, right.precedence <= 30), 30};
        default:
            if (const auto custom = custom_binary_index(kind)) {
                const auto& operation = extension_registry().binary_operations()[*custom];
                if (operation.render_text) return {operation.render_text(left.text, right.text), 80};
                return {operation.name + "(" + left.text + "," + right.text + ")", 80};
            }
            break;
    }
    throw std::runtime_error("未知表达式节点");
}

static Rendered render_expression_impl(const std::vector<AtomSpec>& atoms,
                                       const std::vector<Node>& arena,
                                       ExprId id) {
    return render_node(atoms, arena, arena[id]);
}

static Rendered render_expression(const SearchRun& run, ExprId id) {
    return render_expression_impl(run.atoms, run.arena, id);
}

static std::string latex_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\': escaped += "\\backslash{}"; break;
            case '{': escaped += "\\{"; break;
            case '}': escaped += "\\}"; break;
            case '_': escaped += "\\_"; break;
            case '^': escaped += "\\^{}"; break;
            case '#': escaped += "\\#"; break;
            case '$': escaped += "\\$"; break;
            case '%': escaped += "\\%"; break;
            case '&': escaped += "\\&"; break;
            default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

static std::string render_atom_latex(std::string_view atom) {
    if (atom == "pi") return "\\pi";
    if (atom == "phi") return "\\varphi";
    if (atom == "gamma") return "\\gamma";
    if (atom == "catalan") return "G";
    if (atom == "tau") return "\\tau";
    if (atom == "ln2") return "\\ln 2";
    if (atom == "sqrt2") return "\\sqrt{2}";
    if (atom == "e" || atom == "x") return std::string(atom);
    const bool numeric = !atom.empty() && std::all_of(atom.begin(), atom.end(), [](const char c) {
        return (c >= '0' && c <= '9') || c == '.';
    });
    if (numeric) return std::string(atom);
    return "\\mathrm{" + latex_escape(atom) + "}";
}

static std::string latex_parenthesize(const RenderedLatex& rendered, bool needed) {
    return needed ? "\\left(" + rendered.text + "\\right)" : rendered.text;
}

static RenderedLatex render_node_latex(const std::vector<AtomSpec>& atoms,
                                       const std::vector<Node>& arena,
                                       const Node& node);

static void collect_multiplicative_factors(const std::vector<AtomSpec>& atoms,
                                           const std::vector<Node>& arena,
                                           const Node& node,
                                           bool denominator,
                                           std::vector<RenderedLatex>& numerator,
                                           std::vector<RenderedLatex>& denominators) {
    if (node.tag == NodeTag::Binary) {
        const auto kind = static_cast<BinaryKind>(node.op);
        if (kind == BinaryKind::Mul) {
            collect_multiplicative_factors(
                atoms, arena, arena[node.left], denominator, numerator, denominators);
            collect_multiplicative_factors(
                atoms, arena, arena[node.right], denominator, numerator, denominators);
            return;
        }
        if (kind == BinaryKind::Div) {
            collect_multiplicative_factors(
                atoms, arena, arena[node.left], denominator, numerator, denominators);
            collect_multiplicative_factors(
                atoms, arena, arena[node.right], !denominator, numerator, denominators);
            return;
        }
    }
    (denominator ? denominators : numerator).push_back(render_node_latex(atoms, arena, node));
}

static std::string render_latex_product(std::vector<RenderedLatex> factors) {
    if (factors.empty()) return "1";
    std::sort(factors.begin(), factors.end(), [](const RenderedLatex& left, const RenderedLatex& right) {
        if (left.text != right.text) return left.text < right.text;
        return left.precedence < right.precedence;
    });
    std::string product;
    for (const RenderedLatex& rendered : factors) {
        if (!product.empty()) product += "\\,\\times\\,";
        product += latex_parenthesize(rendered, rendered.precedence < 20);
    }
    return product;
}

static RenderedLatex render_multiplicative_latex(const std::vector<AtomSpec>& atoms,
                                                  const std::vector<Node>& arena,
                                                  const Node& node) {
    std::vector<RenderedLatex> numerator;
    std::vector<RenderedLatex> denominator;
    numerator.reserve(4);
    denominator.reserve(2);
    collect_multiplicative_factors(atoms, arena, node, false, numerator, denominator);
    const std::string top = render_latex_product(std::move(numerator));
    if (denominator.empty()) return {top, 20};
    return {"\\frac{" + top + "}{" + render_latex_product(std::move(denominator)) + "}", 80};
}

static RenderedLatex render_node_latex(const std::vector<AtomSpec>& atoms,
                                       const std::vector<Node>& arena,
                                       const Node& node) {
    if (node.tag == NodeTag::Atom) return {render_atom_latex(atoms[node.atom_index].text), 100};

    if (node.tag == NodeTag::Unary) {
        const auto kind = static_cast<UnaryKind>(node.op);
        const RenderedLatex child = render_expression_latex_impl(atoms, arena, node.left);
        const std::string grouped = "\\left(" + child.text + "\\right)";
        switch (kind) {
            case UnaryKind::Neg:
                return {"-" + latex_parenthesize(child, child.precedence < 25), 25};
            case UnaryKind::Inv:
                return {"\\frac{1}{" + child.text + "}", 80};
            case UnaryKind::Sqrt:
                return {"\\sqrt{" + child.text + "}", 80};
            case UnaryKind::Cbrt:
                return {"\\operatorname{cbrt}" + grouped, 80};
            case UnaryKind::Sqr:
                return {"\\operatorname{sqr}" + grouped, 80};
            case UnaryKind::Cube:
                return {"\\operatorname{cube}" + grouped, 80};
            case UnaryKind::Ln:
                return {"\\ln" + grouped, 80};
            case UnaryKind::Log10:
                return {"\\operatorname{log10}" + grouped, 80};
            case UnaryKind::Exp:
                return {"e^{" + child.text + "}", 80};
            case UnaryKind::Sin:
                return {"\\sin" + grouped, 80};
            case UnaryKind::Cos:
                return {"\\cos" + grouped, 80};
            case UnaryKind::Tan:
                return {"\\tan" + grouped, 80};
            case UnaryKind::Asin:
                return {"\\arcsin" + grouped, 80};
            case UnaryKind::Acos:
                return {"\\arccos" + grouped, 80};
            case UnaryKind::Atan:
                return {"\\arctan" + grouped, 80};
            case UnaryKind::Sinh:
                return {"\\sinh" + grouped, 80};
            case UnaryKind::Cosh:
                return {"\\cosh" + grouped, 80};
            case UnaryKind::Tanh:
                return {"\\tanh" + grouped, 80};
            case UnaryKind::Asinh:
                return {"\\operatorname{arsinh}" + grouped, 80};
            case UnaryKind::Acosh:
                return {"\\operatorname{arcosh}" + grouped, 80};
            case UnaryKind::Atanh:
                return {"\\operatorname{artanh}" + grouped, 80};
            case UnaryKind::Gamma:
                return {"\\Gamma" + grouped, 80};
            case UnaryKind::Abs:
                return {"\\left|" + child.text + "\\right|", 80};
            case UnaryKind::Fact:
                return {latex_parenthesize(child, child.precedence < 80) + "!", 80};
            default:
                if (const auto custom = custom_unary_index(kind)) {
                    const auto& operation = extension_registry().unary_operations()[*custom];
                    if (operation.render_latex) return {operation.render_latex(child.text), 80};
                    return {"\\operatorname{" + latex_escape(operation.name) + "}" + grouped, 80};
                }
                break;
        }
        throw std::runtime_error("未知一元 LaTeX 表达式节点");
    }

    const auto kind = static_cast<BinaryKind>(node.op);
    if (kind == BinaryKind::Mul || kind == BinaryKind::Div) {
        return render_multiplicative_latex(atoms, arena, node);
    }
    const RenderedLatex left = render_expression_latex_impl(atoms, arena, node.left);
    const RenderedLatex right = render_expression_latex_impl(atoms, arena, node.right);
    switch (kind) {
        case BinaryKind::Add:
            return {latex_parenthesize(left, left.precedence < 10) + "+" +
                        latex_parenthesize(right, right.precedence < 10), 10};
        case BinaryKind::Sub:
            return {latex_parenthesize(left, left.precedence < 10) + "-" +
                        latex_parenthesize(right, right.precedence <= 10), 10};
        case BinaryKind::Pow:
            return {"{" + latex_parenthesize(left, left.precedence <= 30) + "}^{" + right.text + "}", 30};
        case BinaryKind::Mul:
        case BinaryKind::Div:
            break;
        default:
            if (const auto custom = custom_binary_index(kind)) {
                const auto& operation = extension_registry().binary_operations()[*custom];
                if (operation.render_latex) {
                    return {operation.render_latex(left.text, right.text), 80};
                }
                return {"\\operatorname{" + latex_escape(operation.name) + "}\\left(" +
                            left.text + "," + right.text + "\\right)", 80};
            }
            break;
    }
    throw std::runtime_error("未知二元 LaTeX 表达式节点");
}

static RenderedLatex render_expression_latex_impl(const std::vector<AtomSpec>& atoms,
                                                  const std::vector<Node>& arena,
                                                  ExprId id) {
    return render_node_latex(atoms, arena, arena[id]);
}

static RenderedLatex render_expression_latex(const SearchRun& run, ExprId id) {
    return render_expression_latex_impl(run.atoms, run.arena, id);
}

static std::string render_expression_text(const std::vector<AtomSpec>& atoms,
                                          const std::vector<Node>& arena,
                                          ExprId id) {
    return render_expression_impl(atoms, arena, id).text;
}

static std::string render_expression_latex_text(const std::vector<AtomSpec>& atoms,
                                                const std::vector<Node>& arena,
                                                ExprId id) {
    return render_expression_latex_impl(atoms, arena, id).text;
}

static std::string render_candidate_expression(const std::vector<AtomSpec>& atoms,
                                               const std::vector<Node>& arena,
                                               const Candidate& candidate) {
    Node node;
    node.value = candidate.value;
    node.derivative = candidate.derivative;
    node.cost = candidate.cost;
    node.nodes = candidate.nodes;
    node.depth = candidate.depth;
    node.hash = candidate.hash;
    node.tag = candidate.tag;
    node.op = candidate.op;
    node.left = candidate.left;
    node.right = candidate.right;
    node.atom_index = candidate.atom_index;
    node.constraint_state = candidate.constraint_state;
    node.depends_on_x = candidate.depends_on_x;
    return render_node(atoms, arena, node).text;
}

static std::string render_candidate_latex(const std::vector<AtomSpec>& atoms,
                                          const std::vector<Node>& arena,
                                          const Candidate& candidate) {
    Node node;
    node.value = candidate.value;
    node.derivative = candidate.derivative;
    node.cost = candidate.cost;
    node.nodes = candidate.nodes;
    node.depth = candidate.depth;
    node.hash = candidate.hash;
    node.tag = candidate.tag;
    node.op = candidate.op;
    node.left = candidate.left;
    node.right = candidate.right;
    node.atom_index = candidate.atom_index;
    node.constraint_state = candidate.constraint_state;
    node.depends_on_x = candidate.depends_on_x;
    return render_node_latex(atoms, arena, node).text;
}

// Re-evaluate a variable-dependent AST at a nearby point.  Equation mode
// stores only f(T) and f'(T) in the hot search path; this slow, bounded helper
// is called only for the small set of equation pairs that survived numeric
// matching.  Constant subtrees reuse their stored value, which keeps probes
// cheap even for large expressions.
static std::optional<double> evaluate_equation_node(const Config& cfg,
                                                    const std::vector<Node>& arena,
                                                    ExprId id,
                                                    double variable) {
    const Node& node = arena[id];
    if (!node.depends_on_x) return node.value;

    if (node.tag == NodeTag::Atom) {
        return variable;
    }

    if (node.tag == NodeTag::Unary) {
        const auto child_value = evaluate_equation_node(cfg, arena, node.left, variable);
        if (!child_value) return std::nullopt;
        const double x = *child_value;
        double value = 0.0;
        const auto kind = static_cast<UnaryKind>(node.op);
        switch (kind) {
            case UnaryKind::Neg: value = -x; break;
            case UnaryKind::Inv:
                if (x == 0.0) return std::nullopt;
                value = 1.0 / x;
                break;
            case UnaryKind::Sqrt:
                if (x < 0.0) return std::nullopt;
                value = std::sqrt(x);
                break;
            case UnaryKind::Cbrt: value = std::cbrt(x); break;
            case UnaryKind::Sqr: value = x * x; break;
            case UnaryKind::Cube: value = x * x * x; break;
            case UnaryKind::Ln:
                if (x <= 0.0) return std::nullopt;
                value = std::log(x);
                break;
            case UnaryKind::Log10:
                if (x <= 0.0) return std::nullopt;
                value = std::log10(x);
                break;
            case UnaryKind::Exp:
                if (x > std::log(cfg.max_abs)) return std::nullopt;
                value = std::exp(x);
                break;
            case UnaryKind::Sin:
                if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
                value = std::sin(x);
                break;
            case UnaryKind::Cos:
                if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
                value = std::cos(x);
                break;
            case UnaryKind::Tan:
                if (std::abs(x) > cfg.max_trig_arg || std::abs(std::cos(x)) < 1.0e-12) {
                    return std::nullopt;
                }
                value = std::tan(x);
                break;
            case UnaryKind::Asin:
                if (x < -1.0 || x > 1.0) return std::nullopt;
                value = std::asin(x);
                break;
            case UnaryKind::Acos:
                if (x < -1.0 || x > 1.0) return std::nullopt;
                value = std::acos(x);
                break;
            case UnaryKind::Atan: value = std::atan(x); break;
            case UnaryKind::Sinh:
                if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
                value = std::sinh(x);
                break;
            case UnaryKind::Cosh:
                if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
                value = std::cosh(x);
                break;
            case UnaryKind::Tanh:
                if (std::abs(x) > cfg.max_trig_arg) return std::nullopt;
                value = std::tanh(x);
                if (std::abs(value) == 1.0) return std::nullopt;
                break;
            case UnaryKind::Asinh: value = std::asinh(x); break;
            case UnaryKind::Acosh:
                if (x < 1.0) return std::nullopt;
                value = std::acosh(x);
                break;
            case UnaryKind::Atanh:
                if (x <= -1.0 || x >= 1.0) return std::nullopt;
                value = std::atanh(x);
                break;
            case UnaryKind::Gamma:
                if (gamma_input_is_pole(x)) return std::nullopt;
                value = std::tgamma(x);
                if (value == 0.0) return std::nullopt;
                break;
            case UnaryKind::Abs: value = std::abs(x); break;
            case UnaryKind::Fact: {
                const double rounded = std::nearbyint(x);
                if (x < 0.0 || rounded > 170.0 ||
                    std::abs(x - rounded) > 1.0e-12 * std::max(1.0, std::abs(x))) {
                    return std::nullopt;
                }
                value = std::tgamma(rounded + 1.0);
                break;
            }
            default: {
                const auto custom = custom_unary_index(kind);
                if (!custom) return std::nullopt;
                const auto result = extension_registry().unary_operations()[*custom].evaluate(
                    x, extension_limits(cfg));
                if (!result) return std::nullopt;
                value = *result;
                break;
            }
        }
        return valid_numeric(value, cfg) ? std::optional<double>{value} : std::nullopt;
    }

    const auto left_value = evaluate_equation_node(cfg, arena, node.left, variable);
    const auto right_value = evaluate_equation_node(cfg, arena, node.right, variable);
    if (!left_value || !right_value) return std::nullopt;
    const double a = *left_value;
    const double b = *right_value;
    double value = 0.0;
    const auto kind = static_cast<BinaryKind>(node.op);
    switch (kind) {
        case BinaryKind::Add: value = a + b; break;
        case BinaryKind::Sub: value = a - b; break;
        case BinaryKind::Mul: value = a * b; break;
        case BinaryKind::Div:
            if (b == 0.0) return std::nullopt;
            value = a / b;
            break;
        case BinaryKind::Pow: {
            if (std::abs(b) > cfg.max_exponent) return std::nullopt;
            if (a == 0.0) {
                if (b <= 0.0) return std::nullopt;
                value = 0.0;
            } else if (a < 0.0) {
                const double rounded = std::nearbyint(b);
                if (std::abs(b - rounded) > 1.0e-12 * std::max(1.0, std::abs(b))) {
                    return std::nullopt;
                }
                value = std::pow(a, rounded);
            } else {
                value = std::pow(a, b);
            }
            break;
        }
        default: {
            const auto custom = custom_binary_index(kind);
            if (!custom) return std::nullopt;
            const auto result = extension_registry().binary_operations()[*custom].evaluate(
                a, b, extension_limits(cfg));
            if (!result) return std::nullopt;
            value = *result;
            break;
        }
    }
    return valid_numeric(value, cfg) ? std::optional<double>{value} : std::nullopt;
}

static std::optional<double> evaluate_equation_residual(const Config& cfg,
                                                        const std::vector<Node>& arena,
                                                        ExprId left,
                                                        ExprId right,
                                                        double variable) {
    const auto left_value = evaluate_equation_node(cfg, arena, left, variable);
    const auto right_value = evaluate_equation_node(cfg, arena, right, variable);
    if (!left_value || !right_value) return std::nullopt;
    const double residual = *left_value - *right_value;
    return valid_numeric(residual, cfg) ? std::optional<double>{residual} : std::nullopt;
}

static bool equation_is_stable(const Config& cfg,
                               const std::vector<Node>& arena,
                               ExprId left,
                               ExprId right,
                               double residual,
                               double slope,
                               double root) {
    if (cfg.equation_quality == EquationQualityMode::Off) return true;
    if (!std::isfinite(slope) || !std::isfinite(root)) return false;
    const double slope_scale = std::max(1.0, std::abs(slope));
    if (std::abs(slope) <= 64.0 * std::numeric_limits<double>::epsilon() * slope_scale) return false;

    const double target_scale = std::max(1.0, std::abs(cfg.target));
    // Small enough to test the actual local domain, but not so small that
    // double rounding at an integer masks a pole or a logarithm boundary.
    const double h = std::clamp(1.0e-5 * target_scale, 1.0e-7, 5.0e-2);
    const auto minus = evaluate_equation_residual(cfg, arena, left, right, cfg.target - h);
    const auto plus = evaluate_equation_residual(cfg, arena, left, right, cfg.target + h);
    if (!minus || !plus) return false;

    const double finite_difference = (*plus - *minus) / (2.0 * h);
    if (!std::isfinite(finite_difference)) return false;
    const double derivative_error = std::abs(finite_difference - slope) /
                                   std::max({1.0, std::abs(finite_difference), std::abs(slope)});
    if (derivative_error > 0.35) return false;

    // A Newton estimate that lands on a singularity is not an equation root.
    // Probe it when it is reasonably close; a far-away estimate is already
    // represented by the requested error range and is not rejected solely for
    // being nonlinear.
    if (std::abs(root - cfg.target) <= 4.0 * std::max(h, 1.0e-4 * target_scale)) {
        const auto at_root = evaluate_equation_residual(cfg, arena, left, right, root);
        if (!at_root) return false;
        const double residual_scale = std::max(1.0, std::abs(residual));
        if (std::abs(*at_root) > 8.0e-3 * residual_scale && std::abs(residual) > 1.0e-9) {
            return false;
        }
    }

    if (cfg.equation_quality == EquationQualityMode::Local) return true;

    // Reject lattice identities such as x = x - tan(pi*x).  They have a
    // perfectly respectable local derivative at an integer, but the same root
    // repeats one unit away and therefore carries no information about the
    // requested target.  Also require a finite coarse neighbourhood; this
    // catches ln(tan(pi*x)) at an integer where the rounded tan value happens
    // to be a tiny positive number although the mathematical expression is
    // undefined on one side.
    const auto minus_one = evaluate_equation_residual(
        cfg, arena, left, right, cfg.target - 1.0);
    const auto plus_one = evaluate_equation_residual(
        cfg, arena, left, right, cfg.target + 1.0);
    if (!minus_one || !plus_one) return false;
    const double periodic_scale = std::max({1.0, std::abs(*minus_one), std::abs(*plus_one),
                                             std::abs(residual), std::abs(slope)});
    const double periodic_tolerance = 1.0e-8 * periodic_scale;
    if (std::abs(*minus_one) <= periodic_tolerance &&
        std::abs(*plus_one) <= periodic_tolerance) {
        return false;
    }

    return true;
}

static std::optional<double> refine_equation_root(const Config& cfg,
                                                  const std::vector<Node>& arena,
                                                  ExprId left,
                                                  ExprId right,
                                                  double initial_root) {
    if (cfg.equation_quality == EquationQualityMode::Off) return initial_root;

    const auto evaluate = [&](double x) -> std::optional<std::pair<double, double>> {
        const auto left_value = evaluate_equation_node(cfg, arena, left, x);
        const auto right_value = evaluate_equation_node(cfg, arena, right, x);
        if (!left_value || !right_value) return std::nullopt;
        const double residual = *left_value - *right_value;
        if (!valid_numeric(residual, cfg)) return std::nullopt;
        return std::pair{residual, std::max({1.0, std::abs(*left_value), std::abs(*right_value)})};
    };

    auto at_target = evaluate(cfg.target);
    if (!at_target) return std::nullopt;
    const auto converged = [](double residual, double scale) {
        const double tolerance = std::max(
            128.0 * std::numeric_limits<double>::epsilon() * scale,
            1.0e-12 * scale);
        return std::abs(residual) <= tolerance;
    };
    if (converged(at_target->first, at_target->second)) return cfg.target;

    double previous_x = cfg.target;
    double previous_residual = at_target->first;
    double current_x = initial_root;
    auto current = evaluate(current_x);
    if (!current) {
        // A raw Newton step can cross a domain boundary.  Backtrack from T to
        // find the nearest valid point before giving up on the equation.
        double fraction = 0.5;
        for (unsigned attempt = 0; attempt < 16 && !current; ++attempt) {
            current_x = cfg.target + (initial_root - cfg.target) * fraction;
            current = evaluate(current_x);
            fraction *= 0.5;
        }
        if (!current) return std::nullopt;
    }

    const double travel_limit = std::max(
        2.0, 8.0 * std::abs(initial_root - cfg.target) + 0.1 * std::max(1.0, std::abs(cfg.target)));
    for (unsigned iteration = 0; iteration < 20; ++iteration) {
        if (converged(current->first, current->second)) return current_x;

        const double h = std::clamp(
            2.0e-6 * std::max(1.0, std::abs(current_x)), 1.0e-8, 1.0e-3);
        const auto below = evaluate(current_x - h);
        const auto above = evaluate(current_x + h);
        double derivative = std::numeric_limits<double>::quiet_NaN();
        if (below && above) derivative = (above->first - below->first) / (2.0 * h);
        if (!std::isfinite(derivative) ||
            std::abs(derivative) <= 64.0 * std::numeric_limits<double>::epsilon()) {
            const double denominator = current->first - previous_residual;
            if (current_x != previous_x && denominator != 0.0) {
                derivative = denominator / (current_x - previous_x);
            }
        }
        if (!std::isfinite(derivative) ||
            std::abs(derivative) <= 64.0 * std::numeric_limits<double>::epsilon()) {
            return std::nullopt;
        }

        double step = -current->first / derivative;
        if (!std::isfinite(step)) return std::nullopt;
        step = std::clamp(step, -travel_limit, travel_limit);

        std::optional<std::pair<double, double>> next;
        double next_x = current_x;
        double fraction = 1.0;
        for (unsigned attempt = 0; attempt < 18; ++attempt) {
            const double proposed = current_x + step * fraction;
            if (std::isfinite(proposed) && std::abs(proposed) <= cfg.max_abs) {
                auto evaluated = evaluate(proposed);
                if (evaluated &&
                    (std::abs(evaluated->first) < std::abs(current->first) || fraction <= 1.0 / 64.0)) {
                    next_x = proposed;
                    next = std::move(evaluated);
                    break;
                }
            }
            fraction *= 0.5;
        }
        if (!next || next_x == current_x) return std::nullopt;

        previous_x = current_x;
        previous_residual = current->first;
        current_x = next_x;
        current = std::move(next);
    }

    return current && converged(current->first, current->second)
        ? std::optional<double>{current_x}
        : std::nullopt;
}

static std::vector<EquationMatch> collect_equation_matches(const Config& cfg,
                                                           const std::vector<Node>& arena,
                                                           std::size_t limit) {
    std::vector<ExprId> ids;
    ids.reserve(arena.size());
    for (std::size_t i = 0; i < arena.size(); ++i) {
        if (arena[i].eligible) ids.push_back(static_cast<ExprId>(i));
    }
    std::sort(ids.begin(), ids.end(), [&](ExprId a, ExprId b) {
        if (arena[a].value != arena[b].value) return arena[a].value < arena[b].value;
        if (arena[a].cost != arena[b].cost) return arena[a].cost < arena[b].cost;
        return arena[a].hash < arena[b].hash;
    });

    std::vector<EquationMatch> all;
    all.reserve(std::min<std::size_t>(ids.size() * 2, 1'000'000));
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const Node& a = arena[ids[i]];
        const std::size_t end = std::min(ids.size(), i + 1 + cfg.equation_neighbors);
        for (std::size_t j = i + 1; j < end; ++j) {
            const Node& b = arena[ids[j]];
            if (!a.depends_on_x && !b.depends_on_x) continue;
            const unsigned cost = static_cast<unsigned>(a.cost) + b.cost;
            if (cost > cfg.max_cost || a.hash == b.hash) continue;

            const double slope = a.derivative - b.derivative;
            const double slope_scale = std::max({1.0, std::abs(a.derivative), std::abs(b.derivative)});
            if (!std::isfinite(slope) || std::abs(slope) <= 16.0 * std::numeric_limits<double>::epsilon() * slope_scale) {
                continue;
            }
            const double residual = a.value - b.value;
            const double initial_correction = -residual / slope;
            const double initial_root = cfg.target + initial_correction;
            if (!std::isfinite(initial_root) || std::abs(initial_root) > cfg.max_abs) {
                continue;
            }

            ExprId left = ids[i];
            ExprId right = ids[j];
            if (!equation_is_stable(cfg, arena, left, right, residual, slope, initial_root)) {
                continue;
            }
            const auto refined_root = refine_equation_root(cfg, arena, left, right, initial_root);
            if (!refined_root) continue;
            const double root = *refined_root;
            const double correction = root - cfg.target;
            const double error = std::abs(correction);
            if (!cfg.error_range.contains(correction)) continue;

            double oriented_residual = residual;
            if (!a.depends_on_x && b.depends_on_x) {
                std::swap(left, right);
                oriented_residual = -residual;
            }
            const auto constraint_state = constraint_join_equation(
                cfg, arena[left].constraint_state, arena[right].constraint_state,
                arena[left].value, arena[right].value, cost);
            if (!constraint_state || !constraint_satisfied(cfg, *constraint_state)) continue;
            all.push_back({left, right, root, error, oriented_residual, cost,
                           static_cast<unsigned>(a.nodes) + b.nodes});
        }
    }

    std::sort(all.begin(), all.end(), [](const EquationMatch& a, const EquationMatch& b) {
        if (a.cost != b.cost) return a.cost < b.cost;
        return equation_better(a, b);
    });

    std::vector<EquationMatch> matches;
    if (cfg.mode == "nearest") {
        std::sort(all.begin(), all.end(), equation_better);
        if (all.size() > limit) all.resize(limit);
        return all;
    }

    double best = std::numeric_limits<double>::infinity();
    std::size_t begin = 0;
    while (begin < all.size()) {
        std::size_t end = begin + 1;
        while (end < all.size() && all[end].cost == all[begin].cost) ++end;
        const auto layer_best = *std::min_element(all.begin() + static_cast<std::ptrdiff_t>(begin),
                                                  all.begin() + static_cast<std::ptrdiff_t>(end),
                                                  equation_better);
        if (layer_best.abs_err < best) {
            matches.push_back(layer_best);
            best = layer_best.abs_err;
        }
        begin = end;
    }
    if (matches.size() > limit) matches.resize(limit);
    return matches;
}

struct Match {
    ExprId id{};
    double value{};
    double signed_err{};
    double abs_err{};
    double rel_err{};
    unsigned cost{};
    unsigned nodes{};
};

static bool match_better(const Match& a, const Match& b) {
    if (a.abs_err != b.abs_err) return a.abs_err < b.abs_err;
    if (a.cost != b.cost) return a.cost < b.cost;
    if (a.nodes != b.nodes) return a.nodes < b.nodes;
    return a.id < b.id;
}

static std::vector<Match> collect_matches(const SearchRun& run) {
    std::vector<Match> matches;
    if (run.cfg.mode == "nearest") {
        FastMap<std::uint64_t, Match> best_by_value;
        best_by_value.reserve(run.arena.size() * 2 + 1);
        for (ExprId id = 0; id < run.arena.size(); ++id) {
            const Node& node = run.arena[id];
            if (!node.eligible) continue;
            const double signed_error = node.value - run.cfg.target;
            const double error = abs_error(node.value, run.cfg.target);
            if (run.cfg.error_range.contains(signed_error) &&
                constraint_satisfied(run.cfg, node.constraint_state)) {
                Match candidate{id, node.value, signed_error, error,
                                rel_error(node.value, run.cfg.target), node.cost, node.nodes};
                const std::uint64_t key = value_bucket(node.value, 52);
                const auto found = best_by_value.find(key);
                if (found == best_by_value.end()) best_by_value.emplace(key, candidate);
                else if (match_better(candidate, found->second)) found->second = candidate;
            }
        }
        matches.reserve(best_by_value.size());
        for (const auto& [_, match] : best_by_value) matches.push_back(match);
        std::sort(matches.begin(), matches.end(), match_better);
        if (matches.size() > run.cfg.results) matches.resize(run.cfg.results);
        return matches;
    }

    double best = std::numeric_limits<double>::infinity();
    for (unsigned cost = 1; cost < run.layers.size(); ++cost) {
        const bool extra_empty = cost >= run.extra_layers.size() || run.extra_layers[cost].empty();
        if (run.layers[cost].empty() && extra_empty) continue;
        std::optional<Match> layer_best;
        for (const ExprId id : run.layers[cost]) {
            const Node& node = run.arena[id];
            Match m{id, node.value, node.value - run.cfg.target, abs_error(node.value, run.cfg.target),
                    rel_error(node.value, run.cfg.target), node.cost, node.nodes};
            if (!run.cfg.error_range.contains(m.signed_err) ||
                !constraint_satisfied(run.cfg, node.constraint_state)) continue;
            if (!layer_best || match_better(m, *layer_best)) layer_best = m;
        }
        if (cost < run.extra_layers.size()) {
            for (const ExprId id : run.extra_layers[cost]) {
                const Node& node = run.arena[id];
                Match m{id, node.value, node.value - run.cfg.target, abs_error(node.value, run.cfg.target),
                        rel_error(node.value, run.cfg.target), node.cost, node.nodes};
                if (!run.cfg.error_range.contains(m.signed_err) ||
                    !constraint_satisfied(run.cfg, node.constraint_state)) continue;
                if (!layer_best || match_better(m, *layer_best)) layer_best = m;
            }
        }
        if (layer_best && (layer_best->abs_err < best || (layer_best->abs_err == 0.0 && best != 0.0))) {
            matches.push_back(*layer_best);
            best = layer_best->abs_err;
        }
    }

    if (matches.size() > run.cfg.results) {
        std::vector<Match> sampled;
        sampled.reserve(run.cfg.results);
        if (run.cfg.results == 1) {
            sampled.push_back(matches.back());
        } else {
            for (std::size_t k = 0; k < run.cfg.results; ++k) {
                const std::size_t pos = (k * (matches.size() - 1)) / (run.cfg.results - 1);
                sampled.push_back(matches[pos]);
            }
        }
        matches = std::move(sampled);
    }
    return matches;
}

static void deduplicate_matches(const SearchRun& run, std::vector<Match>& matches) {
    FastSet<std::string> formulas;
    formulas.reserve(matches.size() * 2 + 1);
    matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const Match& match) {
        return !formulas.insert(render_expression_latex(run, match.id).text).second;
    }), matches.end());
}

static void deduplicate_equation_matches(const SearchRun& run,
                                         std::vector<EquationMatch>& matches) {
    FastSet<std::string> equations;
    equations.reserve(matches.size() * 2 + 1);
    matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const EquationMatch& match) {
        std::string left = render_expression_latex(run, match.left).text;
        std::string right = render_expression_latex(run, match.right).text;
        if (right < left) std::swap(left, right);
        return !equations.insert(left + "=" + right).second;
    }), matches.end());
}

static std::string json_escape(std::string_view text) {
    std::ostringstream out;
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c)
                        << std::dec << std::setfill(' ');
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

static std::string search_strategy_name(const SearchStats& stats) {
    std::string name = stats.used_mitm ? "mitm" : "layered";
    if (stats.used_portfolio) name += "+portfolio";
    if (stats.used_inverse_templates) name += "+inverse";
    if (stats.used_deep_compositions) name += "+deep";
    if (stats.used_exploration) name += "+explore";
    if (stats.used_egraph) name += "+egraph";
    if (stats.used_pslq) name += "+pslq";
    if (stats.used_mcts) name += "+mcts";
    if (stats.used_genetic) name += "+genetic";
    return name;
}

static void print_banner(std::ostream& output) {
    // Every glyph is a five-column cell joined by two spaces.  Keeping the
    // frame width explicit makes the banner stable in terminals that render
    // copied/redirected output differently.
    output << R"(+-----------------------------------+
| FFFFF  AAAAA  TTTTT  EEEEE  SSSSS |
| F      A   A    T    E      S     |
| FFFF   AAAAA    T    EEEE   SSSSS |
| F      A A      T    E          S |
| F      A  A     T    EEEEE  SSSSS |
+-----------------------------------+
)"
           << kProgramFullName << "  v" << kProgramVersion << "\n\n";
}

static void print_stats(const SearchRun& run) {
    if (!run.cfg.show_stats) return;
    std::cerr << std::defaultfloat << "[stats] strategy=" << search_strategy_name(run.stats)
              << " completed_cost=" << run.stats.completed_cost
              << " generated_cost=" << run.stats.generated_cost
              << " threads=" << run.cfg.threads << " attempted=" << run.stats.attempted
              << " valid=" << run.stats.valid << " kept=" << run.stats.kept;
    if (run.stats.pareto_extras > 0) std::cerr << " pareto_extras=" << run.stats.pareto_extras;
    if (run.stats.used_inverse_templates) {
        std::cerr << " inverse_candidates=" << run.stats.inverse_candidates;
    }
    if (run.stats.used_genetic) {
        std::cerr << " genetic_generations=" << run.stats.genetic_generations
                  << " genetic_repairs=" << run.stats.genetic_repairs
                  << " genetic_repairs_kept=" << run.stats.genetic_repairs_kept
                  << " genetic_crossovers=" << run.stats.genetic_crossovers
                  << " genetic_crossovers_kept=" << run.stats.genetic_crossovers_kept;
    }
    if (run.stats.used_egraph) {
        std::cerr << " egraph_rewrites=" << run.stats.egraph_rewrites
                  << " egraph_candidates=" << run.stats.egraph_candidates;
    }
    if (run.stats.used_pslq) {
        std::cerr << " pslq_relations=" << run.stats.pslq_relations
                  << " pslq_candidates=" << run.stats.pslq_candidates;
    }
    if (run.stats.used_mcts) {
        std::cerr << " mcts_expansions=" << run.stats.mcts_expansions
                  << " mcts_candidates=" << run.stats.mcts_candidates;
    }
    std::cerr << " stage_seconds=deterministic:" << std::fixed << std::setprecision(3)
              << run.stats.deterministic_seconds << ",mitm:" << run.stats.mitm_seconds
              << ",inverse:" << run.stats.inverse_seconds << ",deep:" << run.stats.deep_seconds
              << ",egraph:" << run.stats.egraph_seconds << ",pslq:" << run.stats.pslq_seconds
              << ",mcts:" << run.stats.mcts_seconds
              << ",genetic:" << run.stats.genetic_seconds;
    std::cerr << " time=" << std::fixed << std::setprecision(3) << run.stats.seconds << "s\n";
}

static void print_equation_results(const SearchRun& run) {
    auto matches = collect_equation_matches(run.cfg, run.arena, run.cfg.results);
    deduplicate_equation_matches(run, matches);
    if (run.cfg.json) {
        std::cout << "{\n  \"target\": " << std::setprecision(17) << run.cfg.target
                  << ",\n  \"search_mode\": \"equations\",\n"
                  << "  \"completed_cost\": " << run.stats.completed_cost << ",\n"
                  << "  \"generated_cost\": " << run.stats.generated_cost << ",\n"
                  << "  \"strategy\": \"" << search_strategy_name(run.stats) << "\",\n"
                  << "  \"value_prune\": \"" << value_prune_mode_name(run.cfg.value_prune) << "\",\n"
                  << "  \"equation_search\": \"" << equation_search_mode_name(run.cfg.equation_search) << "\",\n"
                  << "  \"equation_quality\": \"" << equation_quality_mode_name(run.cfg.equation_quality) << "\",\n"
                  << "  \"explore_pairs\": " << run.cfg.explore_pairs << ",\n"
                  << "  \"results\": [\n";
        for (std::size_t i = 0; i < matches.size(); ++i) {
            const auto& m = matches[i];
            const std::string equation = render_expression(run, m.left).text + " = " +
                                         render_expression(run, m.right).text;
            const std::string latex = render_expression_latex(run, m.left).text + " = " +
                                      render_expression_latex(run, m.right).text;
            std::cout << "    {\"equation\": \"" << json_escape(equation)
                      << "\", \"latex\": \"" << json_escape(latex)
                      << "\", \"estimated_root\": " << std::setprecision(17) << m.root
                      << ", \"cost\": " << m.cost << ", \"nodes\": " << m.nodes
                      << ", \"signed_error\": " << (m.root - run.cfg.target)
                      << ", \"absolute_error\": " << m.abs_err
                      << ", \"residual_at_target\": " << m.residual << "}";
            if (i + 1 != matches.size()) std::cout << ',';
            std::cout << '\n';
        }
        std::cout << "  ],\n  \"stats\": {\"threads\": " << run.cfg.threads
                  << ", \"attempted\": " << run.stats.attempted << ", \"valid\": " << run.stats.valid
                  << ", \"kept\": " << run.stats.kept << ", \"genetic_generations\": "
                  << run.stats.genetic_generations << ", \"genetic_repairs\": "
                  << run.stats.genetic_repairs << ", \"genetic_repairs_kept\": "
                  << run.stats.genetic_repairs_kept << ", \"genetic_crossovers\": "
                  << run.stats.genetic_crossovers << ", \"genetic_crossovers_kept\": "
                  << run.stats.genetic_crossovers_kept << ", \"egraph_rewrites\": "
                  << run.stats.egraph_rewrites << ", \"egraph_candidates\": "
                  << run.stats.egraph_candidates << ", \"pslq_relations\": "
                  << run.stats.pslq_relations << ", \"pslq_candidates\": "
                  << run.stats.pslq_candidates << ", \"mcts_expansions\": "
                  << run.stats.mcts_expansions << ", \"mcts_candidates\": "
                  << run.stats.mcts_candidates << ", \"stage_seconds\": {\"deterministic\": "
                  << run.stats.deterministic_seconds << ", \"mitm\": " << run.stats.mitm_seconds
                  << ", \"inverse\": " << run.stats.inverse_seconds << ", \"deep\": "
                  << run.stats.deep_seconds << ", \"egraph\": " << run.stats.egraph_seconds
                  << ", \"pslq\": " << run.stats.pslq_seconds
                  << ", \"mcts\": " << run.stats.mcts_seconds
                  << ", \"genetic\": " << run.stats.genetic_seconds
                  << "}, \"seconds\": " << run.stats.seconds << "}\n}\n";
        return;
    }

    print_banner(std::cout);
    std::cout << "Target   : " << std::setprecision(17) << run.cfg.target << "\n"
              << "Mode     : equations\n"
              << "Strategy : " << search_strategy_name(run.stats) << "\n"
              << "Policy   : " << equation_search_mode_name(run.cfg.equation_search)
              << " / " << equation_quality_mode_name(run.cfg.equation_quality) << "\n"
              << "Results  : " << matches.size() << "\n\n";
    if (matches.empty()) {
        std::cout << "未找到满足误差区间和符号约束的候选方程。请增大 --max-cost/--beam/--equation-neighbors，"
                     "或放宽 --error-range/--symbol-count/--symbol-order。\n";
    } else {
        std::cout << "  #  cost  estimated_root         signed_error    abs_error       residual        "
                  << (run.cfg.latex ? "equation (LaTeX)" : "equation") << "\n";
        std::cout << "---  ----  ---------------------  --------------  --------------  --------------  ------------------------------\n";
        for (std::size_t index = 0; index < matches.size(); ++index) {
            const auto& m = matches[index];
            const std::string equation = run.cfg.latex
                ? render_expression_latex(run, m.left).text + " = " +
                      render_expression_latex(run, m.right).text
                : render_expression(run, m.left).text + " = " +
                      render_expression(run, m.right).text;
            std::cout << std::setw(3) << (index + 1) << "  " << std::setw(4) << m.cost << "  "
                      << std::setw(21) << std::setprecision(15)
                      << std::defaultfloat << m.root << "  " << std::setw(14) << std::scientific
                      << std::setprecision(6) << (m.root - run.cfg.target) << "  " << std::setw(14)
                      << m.abs_err << "  " << std::setw(14) << m.residual
                      << "  " << equation << '\n';
        }
    }
    print_stats(run);
}

static void print_results(const SearchRun& run) {
    if (run.cfg.equations) {
        print_equation_results(run);
        return;
    }
    auto matches = collect_matches(run);
    deduplicate_matches(run, matches);
    if (run.cfg.json) {
        std::cout << "{\n  \"target\": " << std::setprecision(17) << run.cfg.target << ",\n"
                  << "  \"completed_cost\": " << run.stats.completed_cost << ",\n"
                  << "  \"generated_cost\": " << run.stats.generated_cost << ",\n"
                  << "  \"strategy\": \"" << search_strategy_name(run.stats) << "\",\n"
                  << "  \"value_prune\": \"" << value_prune_mode_name(run.cfg.value_prune) << "\",\n"
                  << "  \"explore_pairs\": " << run.cfg.explore_pairs << ",\n"
                  << "  \"results\": [\n";
        for (std::size_t i = 0; i < matches.size(); ++i) {
            const auto& m = matches[i];
            const std::string expression = render_expression(run, m.id).text;
            const std::string latex = render_expression_latex(run, m.id).text;
            std::cout << "    {\"expression\": \"" << json_escape(expression)
                      << "\", \"latex\": \"" << json_escape(latex) << "\", \"value\": "
                      << std::setprecision(17) << m.value << ", \"cost\": " << m.cost
                      << ", \"nodes\": " << m.nodes << ", \"signed_error\": " << m.signed_err
                      << ", \"absolute_error\": " << m.abs_err
                      << ", \"relative_error\": " << m.rel_err << "}";
            if (i + 1 != matches.size()) std::cout << ',';
            std::cout << '\n';
        }
        std::cout << "  ],\n  \"stats\": {\"threads\": " << run.cfg.threads
                  << ", \"attempted\": " << run.stats.attempted << ", \"valid\": " << run.stats.valid
                  << ", \"kept\": " << run.stats.kept << ", \"genetic_generations\": "
                  << run.stats.genetic_generations << ", \"genetic_repairs\": "
                  << run.stats.genetic_repairs << ", \"genetic_repairs_kept\": "
                  << run.stats.genetic_repairs_kept << ", \"genetic_crossovers\": "
                  << run.stats.genetic_crossovers << ", \"genetic_crossovers_kept\": "
                  << run.stats.genetic_crossovers_kept << ", \"egraph_rewrites\": "
                  << run.stats.egraph_rewrites << ", \"egraph_candidates\": "
                  << run.stats.egraph_candidates << ", \"pslq_relations\": "
                  << run.stats.pslq_relations << ", \"pslq_candidates\": "
                  << run.stats.pslq_candidates << ", \"mcts_expansions\": "
                  << run.stats.mcts_expansions << ", \"mcts_candidates\": "
                  << run.stats.mcts_candidates << ", \"stage_seconds\": {\"deterministic\": "
                  << run.stats.deterministic_seconds << ", \"mitm\": " << run.stats.mitm_seconds
                  << ", \"inverse\": " << run.stats.inverse_seconds << ", \"deep\": "
                  << run.stats.deep_seconds << ", \"egraph\": " << run.stats.egraph_seconds
                  << ", \"pslq\": " << run.stats.pslq_seconds
                  << ", \"mcts\": " << run.stats.mcts_seconds
                  << ", \"genetic\": " << run.stats.genetic_seconds
                  << "}, \"seconds\": " << run.stats.seconds << "}\n}\n";
        return;
    }

    print_banner(std::cout);
    std::cout << "Target   : " << std::setprecision(17) << run.cfg.target << "\n"
              << "Mode     : expressions / " << run.cfg.mode << "\n"
              << "Strategy : " << search_strategy_name(run.stats) << "\n"
              << "Results  : " << matches.size() << "\n\n";
    if (matches.empty()) {
        std::cout << "未找到满足误差区间和符号约束的候选表达式。请增大 --max-cost/--beam/--pairs，"
                     "或放宽符号集合与约束。\n";
    } else {
        std::cout << "  #  cost  value                  signed_error    abs_error       rel_error       "
                  << (run.cfg.latex ? "expression (LaTeX)" : "expression") << "\n";
        std::cout << "---  ----  ---------------------  --------------  --------------  --------------  ------------------------------\n";
        for (std::size_t index = 0; index < matches.size(); ++index) {
            const auto& m = matches[index];
            const std::string expression = run.cfg.latex
                ? render_expression_latex(run, m.id).text
                : render_expression(run, m.id).text;
            std::cout << std::setw(3) << (index + 1) << "  " << std::setw(4) << m.cost << "  "
                      << std::setw(21) << std::setprecision(15) << std::defaultfloat
                      << m.value << "  " << std::setw(14) << std::scientific << std::setprecision(6) << m.signed_err
                      << "  " << std::setw(14) << m.abs_err << "  " << std::setw(14) << m.rel_err
                      << "  " << expression << '\n';
        }
    }
    print_stats(run);
}

static void print_symbol_list(bool json = false) {
    static constexpr std::array<UnaryKind, 24> unary_kinds{
        UnaryKind::Neg, UnaryKind::Inv, UnaryKind::Sqrt, UnaryKind::Cbrt,
        UnaryKind::Sqr, UnaryKind::Cube, UnaryKind::Ln, UnaryKind::Log10,
        UnaryKind::Exp, UnaryKind::Sin, UnaryKind::Cos, UnaryKind::Tan,
        UnaryKind::Asin, UnaryKind::Acos, UnaryKind::Atan, UnaryKind::Sinh,
        UnaryKind::Cosh, UnaryKind::Tanh, UnaryKind::Asinh, UnaryKind::Acosh,
        UnaryKind::Atanh, UnaryKind::Gamma, UnaryKind::Abs, UnaryKind::Fact,
    };
    static constexpr std::array<BinaryKind, 5> binary_kinds{
        BinaryKind::Add, BinaryKind::Sub, BinaryKind::Mul, BinaryKind::Div,
        BinaryKind::Pow,
    };
    const auto default_unary = [](UnaryKind kind) {
        return kind == UnaryKind::Neg || kind == UnaryKind::Inv ||
               kind == UnaryKind::Sqrt || kind == UnaryKind::Ln ||
               kind == UnaryKind::Exp;
    };
    const auto& registry = extension_registry();

    if (json) {
        std::cout << "{\n  \"program\": \"Fates\",\n  \"version\": \"" << kProgramVersion
                  << "\",\n  \"constants\": [\n";
        std::size_t constant_index = 0;
        for (const auto& [name, value] : builtin_constants()) {
            const bool enabled = name == "pi" || name == "e" || name == "phi";
            std::cout << "    {\"name\": \"" << json_escape(name) << "\", \"value\": "
                      << std::setprecision(17) << value << ", \"default_cost\": 1, \"default_enabled\": "
                      << (enabled ? "true" : "false") << ", \"builtin\": true}";
            if (++constant_index != builtin_constants().size()) std::cout << ',';
            std::cout << '\n';
        }
        std::cout << "  ],\n  \"unary_operators\": [\n";
        bool first = true;
        for (UnaryKind kind : unary_kinds) {
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "    {\"name\": \"" << json_escape(unary_name(kind))
                      << "\", \"default_cost\": " << default_unary_cost(kind)
                      << ", \"default_enabled\": " << (default_unary(kind) ? "true" : "false")
                      << ", \"builtin\": true}";
        }
        for (const auto& operation : registry.unary_operations()) {
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "    {\"name\": \"" << json_escape(operation.name)
                      << "\", \"default_cost\": " << operation.default_cost
                      << ", \"default_enabled\": false, \"builtin\": false}";
        }
        std::cout << "\n  ],\n  \"binary_operators\": [\n";
        first = true;
        for (BinaryKind kind : binary_kinds) {
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "    {\"name\": \"" << json_escape(binary_name(kind))
                      << "\", \"default_cost\": " << default_binary_cost(kind)
                      << ", \"default_enabled\": true, \"builtin\": true, \"commutative\": "
                      << (is_commutative(kind) ? "true" : "false") << '}';
        }
        for (const auto& operation : registry.binary_operations()) {
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "    {\"name\": \"" << json_escape(operation.name)
                      << "\", \"default_cost\": " << operation.default_cost
                      << ", \"default_enabled\": false, \"builtin\": false, \"commutative\": "
                      << (operation.commutative ? "true" : "false") << '}';
        }
        std::cout << "\n  ],\n  \"constraints\": [";
        for (std::size_t index = 0; index < registry.constraints().size(); ++index) {
            if (index != 0) std::cout << ',';
            const auto& constraint = registry.constraints()[index];
            std::cout << "{\"name\":\"" << json_escape(constraint.name)
                      << "\",\"state_bits\":" << static_cast<unsigned>(constraint.state_bits) << '}';
        }
        std::cout << "]\n}\n";
        return;
    }

    print_banner(std::cout);
    std::cout << "二元运算符（可写符号或名称，可追加 :cost）：\n"
              << "  + (add)  - (sub)  * (mul)  / (div)  ^ (pow)\n\n"
              << "一元运算符：\n"
              << "  neg inv sqrt cbrt sqr cube ln log10 exp sin cos tan\n"
              << "  asin acos atan abs fact/!\n"
              << "  sinh cosh tanh asinh acosh atanh gamma/Γ（默认不启用）\n\n"
              << "内置常数：\n";
    for (const auto& [name, value] : builtin_constants()) {
        std::cout << "  " << std::setw(8) << std::left << name << " " << std::setprecision(17) << value << '\n';
    }
    if (!registry.unary_operations().empty() || !registry.binary_operations().empty()) {
        std::cout << "\n源码扩展运算（在 --ops 中按名称启用）：\n";
        for (const auto& operation : registry.unary_operations()) {
            std::cout << "  unary   " << std::setw(16) << std::left << operation.name
                      << " default-cost=" << operation.default_cost << '\n';
        }
        for (const auto& operation : registry.binary_operations()) {
            std::cout << "  binary  " << std::setw(16) << std::left << operation.name
                      << " default-cost=" << operation.default_cost
                      << (operation.commutative ? " commutative" : "") << '\n';
        }
    }
    if (!registry.constraints().empty()) {
        std::cout << "\n源码扩展约束：\n";
        for (const auto& constraint : registry.constraints()) {
            std::cout << "  " << constraint.name << " (" << static_cast<unsigned>(constraint.state_bits)
                      << " state bits)\n";
        }
    }
}

static void print_version() {
    std::cout << kProgramName << ' ' << kProgramVersion << " (" << kProgramFullName
              << ", containers=" << kContainerBackend << ")\n";
}

static void print_help(const char* program) {
    print_banner(std::cout);
    std::cout
        << "并行代数目标表达式搜索器（常量表达式 / 可选方程模式）\n\n"
        << "用法：\n  " << program << " TARGET [选项]\n\n"
        << "表达式与约束：\n"
        << "  --digits STR              允许的数字，默认 123456789；可传空串\n"
        << "  --max-literal-len N       最长拼接数字长度，默认 2\n"
        << "  --max-integer N           直接整数字面量上限，默认 25\n"
        << "  --digit-cost N            每个数字字符的复杂度成本，默认 1\n"
        << "  --constants LIST          内置常数列表，默认 pi,e,phi；none 表示禁用\n"
        << "  --constant NAME=VALUE[:C] 添加自定义常数，可重复\n"
        << "  --symbol-count SPEC       原子出现次数 NAME=N 或 NAME=MIN:MAX，可重复；MAX 可为 inf\n"
        << "  --symbol-order LIST       受控叶子必须按此顺序出现，如 1,1,4,5,1,4；none 清除\n"
        << "  --args-file FILE          从 UTF-8 参数文件读取选项；支持引号、# 注释和最多 8 层嵌套\n"
        << "  @FILE                     --args-file 简写；'@@TEXT' 转义为普通参数 '@TEXT'\n"
        << "  --ops LIST                运算符列表，可写 +,-,*,/,^ 或名称；支持 :cost\n"
        << "                            可选高级函数：sinh/cosh/tanh、asinh/acosh/atanh、gamma/Γ\n\n"
        << "搜索规模与性能：\n"
        << "  --max-cost N              最大总复杂度，默认 10\n"
        << "  --side-cost N             双向搜索单边生成成本，默认自动取约 max-cost/2\n"
        << "  --deep-rounds N           通用深层拼接轮数，默认 0；不能用于方程或完整逐层模式\n"
        << "  --deep-beam N             每轮每成本新增候选上限，0=自动，默认 0\n"
        << "  --deep-frontier N         每成本参与深层拼接的输入前沿，0=不限；portfolio 自动限制到最多 2048\n"
        << "  --beam N                  每个复杂度层保留的表达式数，默认 3000\n"
        << "  --pairs N                 每层二元组合预算，默认 4000000\n"
        << "  --threads N               工作线程数，默认硬件并发数\n"
        << "  --task-chunks N           每个组合分区的固定任务块数，默认 64\n"
        << "  --value-bits N            数值分桶保留的尾数位，0..52，默认 42\n"
        << "  --value-prune MODE       数值去重：bucket（按 value-bits）或 exact（严格 double 值），默认 bucket\n"
        << "  --explore-pairs N        每个非完整外层额外确定性采样的二元组合数，0=关闭，默认 0（最大 1000000）\n"
        << "  --pareto-slots N          每个数值桶结构槽数，1..4，默认 1（侧车不挤占 beam）\n"
        << "  --pareto-extra N          每成本侧车候选上限，0=自动，默认 0\n"
        << "  --inverse-neighbors N     每次逆值查询的近邻数，默认 5\n"
        << "  --inverse-depth N         递归逆模板深度，0..8，默认 0；不能用于方程或完整逐层模式\n"
        << "  --inverse-beam N          每个递归请求保留候选数，默认 64\n"
        << "  --inverse-budget N        递归逆模板独立尝试预算，默认 1000000\n"
        << "  --max-abs X               中间结果绝对值上限，默认 1e100\n"
        << "  --max-exponent X          幂运算指数绝对值上限，默认 32\n"
        << "  --max-trig-arg X          三角及双曲函数参数绝对值上限，默认 1e6\n"
        << "  --max-atoms N             原子表达式数量上限，默认 200000\n\n"
        << "高级搜索模式：\n"
        << "  --equations               启用含变量 x 的方程逼近模式\n"
        << "  --equation-neighbors N    方程配对时检查的数值近邻数，默认 64\n"
        << "  --equation-search MODE    方程配对：stable、wide 或 exhaustive，默认 stable\n"
        << "  --equation-quality MODE   方程质量：strict、local 或 off，默认 strict；strict 会拒绝奇异/周期伪根\n"
        << "  --genetic                 启用确定性搜索播种的遗传混合阶段；可与 portfolio 组合\n"
        << "  --pslq / --no-pslq       启用/排除整数关系阶段；系数必须由当前表达式库真实构造\n"
        << "  --egraph / --no-egraph   启用/排除等式饱和化简阶段\n"
        << "  --mcts / --no-mcts       启用/排除带复杂度奖励的蒙特卡洛树搜索\n"
        << "  --search-mode MODE        deterministic|genetic|hybrid|pslq|egraph|mcts|portfolio\n"
        << "                            portfolio 组合逆模板、深搜、PSLQ、e-graph、MCTS 与多样采样\n"
        << "  --genetic-population N    遗传种群大小，默认 4096；指定即自动启用遗传\n"
        << "  --genetic-generations N   遗传迭代代数，默认 64；指定即自动启用遗传\n"
        << "  --genetic-seed N          64 位随机种子，默认 2611923443488327891；指定即启用遗传\n"
        << "  --genetic-elegance X      复杂度惩罚，默认 0.35；指定即自动启用遗传\n"
        << "  --genetic-repair X        子树反求修复概率，0..1，默认 0.15；指定即启用遗传\n"
        << "  --genetic-repair-depth N  修复路径深度，1..8，默认 3；指定即自动启用遗传\n"
        << "  --genetic-crossover X     子树交叉概率，0..1，默认 0.25；指定即自动启用遗传\n"
        << "  --genetic-novelty X       结构物种保留比例，0..0.5，默认 0.20；指定即自动启用遗传\n"
        << "  --genetic-tournament N    选亲锦标赛规模，1..64，默认 4；指定即自动启用遗传\n"
        << "  --pslq-basis N            整数关系基底候选数，默认 192\n"
        << "  --pslq-pairs N            三项关系探测对数，默认 4096\n"
        << "  --pslq-coeff N            系数绝对值上限，1..4096，默认 64\n"
        << "  --pslq-steps N            每次 PSLQ 最大迭代数，默认 64\n"
        << "  --pslq-tolerance X        关系残差相对阈值，默认 1e-12\n"
        << "  --egraph-seeds N          等式饱和种子数，默认 768\n"
        << "  --egraph-rounds N         饱和轮数，默认 2\n"
        << "  --egraph-nodes N          新增等价表达式上限，默认 4096\n"
        << "  --mcts-iterations N       MCTS 迭代预算，默认 8192\n"
        << "  --mcts-depth N            树扩展深度，默认 4\n"
        << "  --mcts-branching N        渐进拓宽分支上限，默认 12\n"
        << "  --mcts-exploration X      UCT 探索系数，默认 1.25\n"
        << "  --mcts-elegance X         成本、节点数与深度惩罚，默认 0.25\n"
        << "  --mcts-seed N             64 位可复现种子，默认 1376283091369227076\n\n"
        << "误差、结果与控制：\n"
        << "  --epsilon X               常量模式达到该绝对误差后停止，默认 1e-12\n"
        << "  --error-range RANGE       value-target；方程为 estimated_root-target，默认 (-inf,inf)\n"
        << "  --no-stop                 即使达到 epsilon 仍搜索到 max-cost\n"
        << "  --no-bidirectional        禁用半表达式合并，改用完整逐层搜索\n"
        << "  --results N               输出条数，默认 20\n"
        << "  --mode pareto|nearest     复杂度-精度前沿或最接近结果，默认 nearest\n"
        << "  --live                    搜索期间在 stderr 刷新当前 top N（N 取 --results）\n"
        << "  --live-top N              搜索期间在 stderr 刷新当前 top N\n"
        << "  --live-interval SEC       实时刷新最短间隔秒数，默认 2\n"
        << "  --live-json               将实时榜输出为带 [fates-live] 前缀的单行 JSON 事件\n"
        << "  --latex                   非 JSON 结果以 LaTeX 源码显示；JSON 始终同时含纯文本和 LaTeX\n"
        << "  --json                    JSON 输出\n"
        << "  --dry-run                 只验证并打印解析后的配置，不执行搜索\n"
        << "  --verbose                 输出逐层统计\n"
        << "  --no-stats                不输出最终统计\n"
        << "  --list-symbols            列出所有运算符和内置常数\n"
        << "  --self-test               运行内置测试\n"
        << "  --version                 显示版本与容器后端\n"
        << "  -h, --help                显示帮助\n\n"
        << "示例：\n"
        << "  " << program << " 1.4142135623730951 --digits 2 --constants none --ops sqrt --max-cost 2\n"
        << "  " << program << " 3.141592653589793 --constants none --digits 123456789 --max-literal-len 3 --max-integer 999 --ops '+,-,*,/' --max-cost 9 --beam 6000 --pairs 12000000\n"
        << "  " << program << " 9.89897948556636 --digits 23 --constants none --ops '+,sqrt,^' --max-cost 8 --deep-rounds 1\n"
        << "  " << program << " 520.82418 --max-cost 16 --genetic --genetic-elegance 0.4\n"
        << "  " << program << " 520.82418 --max-cost 16 --search-mode portfolio --mcts-elegance 0.35\n"
        << "  " << program << " 2 --digits= --constants pi --ops '+,/' --max-cost 7 --symbol-count pi=4\n"
        << "  " << program << " 24 --digits 1234 --max-literal-len 1 --constants none --ops '+,-,*,/' --max-cost 7 --symbol-count 1=1 --symbol-count 2=1 --symbol-count 3=1 --symbol-count 4=1\n"
        << "  " << program << " 0.123456 --constant 'K=0.915965594177219:2' --ops '+,-,*,/,sqrt,ln'\n";
}

static bool option_matches(const std::string& arg, const std::string& name) {
    return arg == name || arg.rfind(name + "=", 0) == 0;
}

static std::string option_value(int& i, int argc, char** argv, const std::string& arg, const std::string& name) {
    const std::string prefix = name + "=";
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    if (i + 1 >= argc) throw std::runtime_error("缺少选项值: " + name);
    return argv[++i];
}

static std::vector<std::string> tokenize_response_text(std::string text, std::string_view source_name) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) {
        text.erase(0, 3);
    }

    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool token_started = false;
    bool comment = false;
    for (const char c : text) {
        if (c == '\0') {
            throw std::runtime_error("参数文件包含 NUL 字节: " + std::string(source_name));
        }
        if (comment) {
            if (c == '\n') comment = false;
            continue;
        }
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            token_started = true;
        } else if (c == '#' && !token_started) {
            comment = true;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (token_started) {
                tokens.push_back(std::move(current));
                current.clear();
                token_started = false;
            }
        } else {
            current.push_back(c);
            token_started = true;
        }
    }
    if (quote != '\0') {
        throw std::runtime_error("参数文件存在未闭合引号: " + std::string(source_name));
    }
    if (token_started) tokens.push_back(std::move(current));
    return tokens;
}

using ActiveResponseFiles = std::set<std::filesystem::path>;

class ActiveResponseFileGuard {
public:
    ActiveResponseFileGuard(ActiveResponseFiles& files, const std::filesystem::path& path)
        : files_(files), path_(path) {}

    ActiveResponseFileGuard(const ActiveResponseFileGuard&) = delete;
    ActiveResponseFileGuard& operator=(const ActiveResponseFileGuard&) = delete;

    ~ActiveResponseFileGuard() { files_.erase(path_); }

private:
    ActiveResponseFiles& files_;
    const std::filesystem::path& path_;
};

static void expand_argument_token(const std::string& token,
                                  const std::filesystem::path& base_directory,
                                  unsigned depth,
                                  ActiveResponseFiles& active_files,
                                  std::vector<std::string>& output);

static void expand_argument_file(std::string_view file_name,
                                 const std::filesystem::path& base_directory,
                                 unsigned depth,
                                 ActiveResponseFiles& active_files,
                                 std::vector<std::string>& output) {
    if (file_name.empty()) throw std::runtime_error("参数文件名不能为空");
    if (depth >= 8) throw std::runtime_error("参数文件递归层数不能超过 8");

    std::filesystem::path path = path_from_utf8(file_name);
    if (path.is_relative()) path = base_directory / path;
    path = std::filesystem::absolute(path).lexically_normal();

    std::error_code canonical_error;
    std::filesystem::path identity = std::filesystem::weakly_canonical(path, canonical_error);
    if (canonical_error) identity = path;
    if (!active_files.insert(identity).second) {
        throw std::runtime_error("参数文件存在递归引用: " + path_to_utf8(path));
    }
    const ActiveResponseFileGuard active_guard(active_files, identity);

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法读取参数文件: " + path_to_utf8(path));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source_name = path_to_utf8(path);
    const auto tokens = tokenize_response_text(buffer.str(), source_name);
    const std::filesystem::path next_base = path.has_parent_path() ? path.parent_path() : base_directory;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& nested = tokens[i];
        if (nested == "--args-file") {
            if (i + 1 >= tokens.size()) {
                throw std::runtime_error("参数文件中缺少 --args-file 的值: " + source_name);
            }
            expand_argument_file(tokens[++i], next_base, depth + 1, active_files, output);
        } else if (nested.rfind("--args-file=", 0) == 0) {
            expand_argument_file(std::string_view(nested).substr(12), next_base, depth + 1,
                                 active_files, output);
        } else {
            expand_argument_token(nested, next_base, depth + 1, active_files, output);
        }
    }
}

static void expand_argument_token(const std::string& token,
                                  const std::filesystem::path& base_directory,
                                  unsigned depth,
                                  ActiveResponseFiles& active_files,
                                  std::vector<std::string>& output) {
    if (token.empty() || token.front() != '@') {
        output.push_back(token);
        return;
    }
    if (token.rfind("@@", 0) == 0) {
        output.push_back(token.substr(1));
        return;
    }
    expand_argument_file(std::string_view(token).substr(1), base_directory, depth,
                         active_files, output);
}

static std::vector<std::string> expand_cli_arguments(int argc, char** argv) {
    std::vector<std::string> expanded;
    expanded.reserve(static_cast<std::size_t>(argc) + 16);
    expanded.emplace_back(argv[0]);
    ActiveResponseFiles active_files;
    const std::filesystem::path base = std::filesystem::current_path();
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--args-file") {
            if (i + 1 >= argc) throw std::runtime_error("缺少选项值: --args-file");
            expand_argument_file(argv[++i], base, 0, active_files, expanded);
        } else if (argument.rfind("--args-file=", 0) == 0) {
            expand_argument_file(std::string_view(argument).substr(12), base, 0,
                                 active_files, expanded);
        } else {
            expand_argument_token(argument, base, 0, active_files, expanded);
        }
    }
    return expanded;
}

static bool handle_direct_control_option(int argc, char** argv) {
    const char* program = argc > 0 && argv[0] != nullptr ? argv[0] : "fates";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--args-file") {
            if (i + 1 < argc) ++i;
            continue;
        }
        if (argument == "-h" || argument == "--help") {
            print_help(program);
            return true;
        }
        if (argument == "--version") {
            print_version();
            return true;
        }
    }
    return false;
}

static unsigned parse_unsigned(const std::string& text, const std::string& name) {
    if (text.empty() || text.front() == '-') throw std::runtime_error("无效整数 " + name + ": " + text);
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || value > std::numeric_limits<unsigned>::max()) {
        throw std::runtime_error("无效整数 " + name + ": " + text);
    }
    return static_cast<unsigned>(value);
}

static std::uint64_t parse_u64(const std::string& text, const std::string& name) {
    if (text.empty() || text.front() == '-') throw std::runtime_error("无效整数 " + name + ": " + text);
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        throw std::runtime_error("无效整数 " + name + ": " + text);
    }
    return static_cast<std::uint64_t>(value);
}

static double parse_double(const std::string& text, const std::string& name) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        throw std::runtime_error("无效数值 " + name + ": " + text);
    }
    return value;
}

static double parse_error_bound(std::string token, const std::string& name) {
    token = trim(std::move(token));
    if (token == "inf" || token == "+inf" || token == "infinity" || token == "+infinity") {
        return std::numeric_limits<double>::infinity();
    }
    if (token == "-inf" || token == "-infinity") {
        return -std::numeric_limits<double>::infinity();
    }
    return parse_double(token, name);
}

static ErrorRange make_error_range(std::string lower,
                                   std::string upper,
                                   bool include_lower,
                                   bool include_upper) {
    ErrorRange range;
    range.include_lower = include_lower;
    range.include_upper = include_upper;
    range.lower = parse_error_bound(std::move(lower), "--error-range 下界");
    range.upper = parse_error_bound(std::move(upper), "--error-range 上界");
    if (std::isnan(range.lower) || std::isnan(range.upper) ||
        range.lower == std::numeric_limits<double>::infinity() ||
        range.upper == -std::numeric_limits<double>::infinity() || range.lower > range.upper ||
        (range.lower == range.upper && (!range.include_lower || !range.include_upper))) {
        throw std::runtime_error("--error-range 必须是非空且下界不大于上界的有符号误差区间");
    }
    return range;
}

static ErrorRange parse_error_range(std::string text) {
    text = trim(std::move(text));
    if (text.size() < 5 || (text.front() != '(' && text.front() != '[') ||
        (text.back() != ')' && text.back() != ']')) {
        throw std::runtime_error(
            "--error-range 格式应为 '(MIN,MAX)'、'[MIN,MAX]'，或 PowerShell 兼容形式 MIN MAX");
    }
    const auto comma = text.find(',');
    if (comma == std::string::npos || text.find(',', comma + 1) != std::string::npos) {
        throw std::runtime_error("--error-range 必须包含且仅包含一个逗号");
    }
    return make_error_range(text.substr(1, comma - 1),
                            text.substr(comma + 1, text.size() - comma - 2),
                            text.front() == '[', text.back() == ']');
}

static std::vector<std::string> parse_symbol_order(std::string text) {
    text = trim(std::move(text));
    if (text.empty() || text == "none") return {};
    std::vector<std::string> order = split_csv(text);
    if (order.empty() || std::any_of(order.begin(), order.end(), [](const std::string& symbol) {
            return symbol.empty();
        })) {
        throw std::runtime_error("--symbol-order 应为逗号分隔的非空原子符号列表");
    }
    return order;
}

static Config parse_cli(int argc, char** argv) {
    Config cfg;
    configure_source_extensions(cfg);
    bool have_target = false;
    bool live_top_explicit = false;
    bool genetic_tuning_seen = false;
    bool pslq_tuning_seen = false;
    bool egraph_tuning_seen = false;
    bool mcts_tuning_seen = false;
    bool genetic_mode_requested = false;
    bool pslq_mode_requested = false;
    bool egraph_mode_requested = false;
    bool mcts_mode_requested = false;
    bool pslq_disabled = false;
    bool egraph_disabled = false;
    bool mcts_disabled = false;
    bool portfolio_mode_requested = false;
    bool deterministic_mode_requested = false;
    bool deep_frontier_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else if (arg == "--version") {
            print_version();
            std::exit(0);
        } else if (arg == "--list-symbols") {
            cfg.list_symbols = true;
        } else if (arg == "--self-test") {
            cfg.self_test = true;
        } else if (arg == "--json") {
            cfg.json = true;
        } else if (arg == "--live-json") {
            cfg.live_json = true;
        } else if (arg == "--latex") {
            cfg.latex = true;
        } else if (arg == "--dry-run") {
            cfg.dry_run = true;
        } else if (arg == "--equations") {
            cfg.equations = true;
        } else if (arg == "--genetic") {
            cfg.genetic = true;
            genetic_mode_requested = true;
        } else if (arg == "--pslq") {
            cfg.pslq = true;
            pslq_mode_requested = true;
            pslq_disabled = false;
        } else if (arg == "--no-pslq") {
            cfg.pslq = false;
            pslq_mode_requested = false;
            pslq_disabled = true;
        } else if (arg == "--egraph") {
            cfg.egraph = true;
            egraph_mode_requested = true;
            egraph_disabled = false;
        } else if (arg == "--no-egraph") {
            cfg.egraph = false;
            egraph_mode_requested = false;
            egraph_disabled = true;
        } else if (arg == "--mcts") {
            cfg.mcts = true;
            mcts_mode_requested = true;
            mcts_disabled = false;
        } else if (arg == "--no-mcts") {
            cfg.mcts = false;
            mcts_mode_requested = false;
            mcts_disabled = true;
        } else if (arg == "--live") {
            cfg.live = true;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else if (arg == "--no-stats") {
            cfg.show_stats = false;
        } else if (arg == "--no-stop") {
            cfg.stop_on_epsilon = false;
        } else if (arg == "--no-bidirectional") {
            cfg.bidirectional = false;
        } else if (option_matches(arg, "--digits")) {
            cfg.digits = option_value(i, argc, argv, arg, "--digits");
        } else if (option_matches(arg, "--max-literal-len")) {
            cfg.max_literal_len = parse_unsigned(option_value(i, argc, argv, arg, "--max-literal-len"), "--max-literal-len");
        } else if (option_matches(arg, "--max-integer")) {
            cfg.max_integer = parse_u64(option_value(i, argc, argv, arg, "--max-integer"), "--max-integer");
        } else if (option_matches(arg, "--digit-cost")) {
            cfg.digit_cost = parse_unsigned(option_value(i, argc, argv, arg, "--digit-cost"), "--digit-cost");
        } else if (option_matches(arg, "--constants")) {
            cfg.constants = option_value(i, argc, argv, arg, "--constants");
        } else if (option_matches(arg, "--constant")) {
            cfg.custom_constants.push_back(option_value(i, argc, argv, arg, "--constant"));
        } else if (option_matches(arg, "--symbol-count")) {
            cfg.symbol_count_specs.push_back(option_value(i, argc, argv, arg, "--symbol-count"));
        } else if (option_matches(arg, "--symbol-order")) {
            cfg.required_symbol_order = parse_symbol_order(
                option_value(i, argc, argv, arg, "--symbol-order"));
        } else if (option_matches(arg, "--ops")) {
            cfg.ops = option_value(i, argc, argv, arg, "--ops");
        } else if (option_matches(arg, "--max-cost")) {
            cfg.max_cost = parse_unsigned(option_value(i, argc, argv, arg, "--max-cost"), "--max-cost");
        } else if (option_matches(arg, "--side-cost")) {
            cfg.side_cost = parse_unsigned(option_value(i, argc, argv, arg, "--side-cost"), "--side-cost");
        } else if (option_matches(arg, "--deep-rounds")) {
            cfg.deep_rounds = parse_unsigned(option_value(i, argc, argv, arg, "--deep-rounds"), "--deep-rounds");
        } else if (option_matches(arg, "--deep-beam")) {
            cfg.deep_beam = static_cast<std::size_t>(
                parse_u64(option_value(i, argc, argv, arg, "--deep-beam"), "--deep-beam"));
        } else if (option_matches(arg, "--deep-frontier")) {
            cfg.deep_frontier = static_cast<std::size_t>(
                parse_u64(option_value(i, argc, argv, arg, "--deep-frontier"), "--deep-frontier"));
            deep_frontier_seen = true;
        } else if (option_matches(arg, "--beam")) {
            cfg.beam = static_cast<std::size_t>(parse_u64(option_value(i, argc, argv, arg, "--beam"), "--beam"));
        } else if (option_matches(arg, "--pairs")) {
            cfg.pair_budget = parse_u64(option_value(i, argc, argv, arg, "--pairs"), "--pairs");
        } else if (option_matches(arg, "--threads")) {
            cfg.threads = parse_unsigned(option_value(i, argc, argv, arg, "--threads"), "--threads");
        } else if (option_matches(arg, "--task-chunks")) {
            cfg.task_chunks = static_cast<std::size_t>(parse_u64(option_value(i, argc, argv, arg, "--task-chunks"), "--task-chunks"));
        } else if (option_matches(arg, "--value-bits")) {
            cfg.value_bits = parse_unsigned(option_value(i, argc, argv, arg, "--value-bits"), "--value-bits");
        } else if (option_matches(arg, "--value-prune")) {
            const std::string mode = option_value(i, argc, argv, arg, "--value-prune");
            if (mode == "bucket") cfg.value_prune = ValuePruneMode::Bucket;
            else if (mode == "exact") cfg.value_prune = ValuePruneMode::Exact;
            else throw std::runtime_error("--value-prune 必须是 bucket 或 exact");
        } else if (option_matches(arg, "--explore-pairs")) {
            cfg.explore_pairs = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--explore-pairs"), "--explore-pairs"));
        } else if (option_matches(arg, "--pareto-slots")) {
            cfg.pareto_slots = parse_unsigned(
                option_value(i, argc, argv, arg, "--pareto-slots"), "--pareto-slots");
        } else if (option_matches(arg, "--pareto-extra")) {
            cfg.pareto_extra = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--pareto-extra"), "--pareto-extra"));
        } else if (option_matches(arg, "--inverse-neighbors")) {
            cfg.inverse_neighbors = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--inverse-neighbors"), "--inverse-neighbors"));
        } else if (option_matches(arg, "--inverse-depth")) {
            cfg.inverse_depth = parse_unsigned(
                option_value(i, argc, argv, arg, "--inverse-depth"), "--inverse-depth");
        } else if (option_matches(arg, "--inverse-beam")) {
            cfg.inverse_beam = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--inverse-beam"), "--inverse-beam"));
        } else if (option_matches(arg, "--inverse-budget")) {
            cfg.inverse_budget = parse_u64(
                option_value(i, argc, argv, arg, "--inverse-budget"), "--inverse-budget");
        } else if (option_matches(arg, "--max-abs")) {
            cfg.max_abs = parse_double(option_value(i, argc, argv, arg, "--max-abs"), "--max-abs");
        } else if (option_matches(arg, "--max-exponent")) {
            cfg.max_exponent = parse_double(option_value(i, argc, argv, arg, "--max-exponent"), "--max-exponent");
        } else if (option_matches(arg, "--max-trig-arg")) {
            cfg.max_trig_arg = parse_double(option_value(i, argc, argv, arg, "--max-trig-arg"), "--max-trig-arg");
        } else if (option_matches(arg, "--epsilon")) {
            cfg.epsilon = parse_double(option_value(i, argc, argv, arg, "--epsilon"), "--epsilon");
        } else if (option_matches(arg, "--error-range")) {
            if (arg.rfind("--error-range=", 0) == 0) {
                cfg.error_range = parse_error_range(option_value(i, argc, argv, arg, "--error-range"));
            } else {
                const std::string first = option_value(i, argc, argv, arg, "--error-range");
                if (!first.empty() && (first.front() == '(' || first.front() == '[')) {
                    cfg.error_range = parse_error_range(first);
                } else {
                    if (i + 1 >= argc || std::string_view(argv[i + 1]).rfind("--", 0) == 0) {
                        throw std::runtime_error(
                            "--error-range 缺少上界；PowerShell 中可直接写 --error-range (0,1)，"
                            "程序会接收为两个数值");
                    }
                    cfg.error_range = make_error_range(first, argv[++i], false, false);
                }
            }
        } else if (option_matches(arg, "--equation-neighbors")) {
            cfg.equation_neighbors = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--equation-neighbors"), "--equation-neighbors"));
        } else if (option_matches(arg, "--equation-search")) {
            const std::string mode = option_value(i, argc, argv, arg, "--equation-search");
            if (mode == "stable") cfg.equation_search = EquationSearchMode::Stable;
            else if (mode == "wide") cfg.equation_search = EquationSearchMode::Wide;
            else if (mode == "exhaustive") cfg.equation_search = EquationSearchMode::Exhaustive;
            else throw std::runtime_error("--equation-search 必须是 stable、wide 或 exhaustive");
        } else if (option_matches(arg, "--equation-quality")) {
            const std::string quality = option_value(i, argc, argv, arg, "--equation-quality");
            if (quality == "strict") cfg.equation_quality = EquationQualityMode::Strict;
            else if (quality == "local") cfg.equation_quality = EquationQualityMode::Local;
            else if (quality == "off") cfg.equation_quality = EquationQualityMode::Off;
            else throw std::runtime_error("--equation-quality 必须是 strict、local 或 off");
        } else if (option_matches(arg, "--search-mode")) {
            const std::string search_mode = option_value(i, argc, argv, arg, "--search-mode");
            if (search_mode == "deterministic") deterministic_mode_requested = true;
            else if (search_mode == "genetic" || search_mode == "hybrid") genetic_mode_requested = true;
            else if (search_mode == "pslq") pslq_mode_requested = true;
            else if (search_mode == "egraph") egraph_mode_requested = true;
            else if (search_mode == "mcts") mcts_mode_requested = true;
            else if (search_mode == "portfolio") portfolio_mode_requested = true;
            else throw std::runtime_error(
                "--search-mode 必须是 deterministic、genetic、hybrid、pslq、egraph、mcts 或 portfolio");
        } else if (option_matches(arg, "--genetic-population")) {
            cfg.genetic_population = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--genetic-population"), "--genetic-population"));
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-generations")) {
            cfg.genetic_generations = parse_unsigned(
                option_value(i, argc, argv, arg, "--genetic-generations"), "--genetic-generations");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-seed")) {
            cfg.genetic_seed = parse_u64(
                option_value(i, argc, argv, arg, "--genetic-seed"), "--genetic-seed");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-elegance")) {
            cfg.genetic_elegance = parse_double(
                option_value(i, argc, argv, arg, "--genetic-elegance"), "--genetic-elegance");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-repair")) {
            cfg.genetic_repair = parse_double(
                option_value(i, argc, argv, arg, "--genetic-repair"), "--genetic-repair");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-repair-depth")) {
            cfg.genetic_repair_depth = parse_unsigned(
                option_value(i, argc, argv, arg, "--genetic-repair-depth"), "--genetic-repair-depth");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-crossover")) {
            cfg.genetic_crossover = parse_double(
                option_value(i, argc, argv, arg, "--genetic-crossover"), "--genetic-crossover");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-novelty")) {
            cfg.genetic_novelty = parse_double(
                option_value(i, argc, argv, arg, "--genetic-novelty"), "--genetic-novelty");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--genetic-tournament")) {
            cfg.genetic_tournament = parse_unsigned(
                option_value(i, argc, argv, arg, "--genetic-tournament"), "--genetic-tournament");
            genetic_tuning_seen = true;
        } else if (option_matches(arg, "--pslq-basis")) {
            cfg.pslq_basis = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--pslq-basis"), "--pslq-basis"));
            pslq_tuning_seen = true;
        } else if (option_matches(arg, "--pslq-pairs")) {
            cfg.pslq_pairs = parse_u64(
                option_value(i, argc, argv, arg, "--pslq-pairs"), "--pslq-pairs");
            pslq_tuning_seen = true;
        } else if (option_matches(arg, "--pslq-coeff")) {
            const std::uint64_t coefficient = parse_u64(
                option_value(i, argc, argv, arg, "--pslq-coeff"), "--pslq-coeff");
            if (coefficient > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("--pslq-coeff 超出 64 位有符号整数范围");
            }
            cfg.pslq_max_coefficient = static_cast<std::int64_t>(coefficient);
            pslq_tuning_seen = true;
        } else if (option_matches(arg, "--pslq-steps")) {
            cfg.pslq_steps = parse_unsigned(
                option_value(i, argc, argv, arg, "--pslq-steps"), "--pslq-steps");
            pslq_tuning_seen = true;
        } else if (option_matches(arg, "--pslq-tolerance")) {
            cfg.pslq_tolerance = parse_double(
                option_value(i, argc, argv, arg, "--pslq-tolerance"), "--pslq-tolerance");
            pslq_tuning_seen = true;
        } else if (option_matches(arg, "--egraph-seeds")) {
            cfg.egraph_seeds = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--egraph-seeds"), "--egraph-seeds"));
            egraph_tuning_seen = true;
        } else if (option_matches(arg, "--egraph-rounds")) {
            cfg.egraph_rounds = parse_unsigned(
                option_value(i, argc, argv, arg, "--egraph-rounds"), "--egraph-rounds");
            egraph_tuning_seen = true;
        } else if (option_matches(arg, "--egraph-nodes")) {
            cfg.egraph_node_limit = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--egraph-nodes"), "--egraph-nodes"));
            egraph_tuning_seen = true;
        } else if (option_matches(arg, "--mcts-iterations")) {
            cfg.mcts_iterations = static_cast<std::size_t>(parse_u64(
                option_value(i, argc, argv, arg, "--mcts-iterations"), "--mcts-iterations"));
            mcts_tuning_seen = true;
        } else if (option_matches(arg, "--mcts-depth")) {
            cfg.mcts_depth = parse_unsigned(
                option_value(i, argc, argv, arg, "--mcts-depth"), "--mcts-depth");
            mcts_tuning_seen = true;
        } else if (option_matches(arg, "--mcts-branching")) {
            cfg.mcts_branching = parse_unsigned(
                option_value(i, argc, argv, arg, "--mcts-branching"), "--mcts-branching");
            mcts_tuning_seen = true;
        } else if (option_matches(arg, "--mcts-exploration")) {
            cfg.mcts_exploration = parse_double(
                option_value(i, argc, argv, arg, "--mcts-exploration"), "--mcts-exploration");
            mcts_tuning_seen = true;
        } else if (option_matches(arg, "--mcts-elegance")) {
            cfg.mcts_elegance = parse_double(
                option_value(i, argc, argv, arg, "--mcts-elegance"), "--mcts-elegance");
            mcts_tuning_seen = true;
        } else if (option_matches(arg, "--mcts-seed")) {
            cfg.mcts_seed = parse_u64(
                option_value(i, argc, argv, arg, "--mcts-seed"), "--mcts-seed");
            mcts_tuning_seen = true;
        } else if (option_matches(arg, "--results")) {
            cfg.results = static_cast<std::size_t>(parse_u64(option_value(i, argc, argv, arg, "--results"), "--results"));
        } else if (option_matches(arg, "--live-top")) {
            cfg.live_top = static_cast<std::size_t>(parse_u64(option_value(i, argc, argv, arg, "--live-top"), "--live-top"));
            cfg.live = true;
            live_top_explicit = true;
        } else if (option_matches(arg, "--live-interval")) {
            cfg.live_interval = parse_double(option_value(i, argc, argv, arg, "--live-interval"), "--live-interval");
        } else if (option_matches(arg, "--max-atoms")) {
            cfg.max_atoms = static_cast<std::size_t>(parse_u64(option_value(i, argc, argv, arg, "--max-atoms"), "--max-atoms"));
        } else if (option_matches(arg, "--mode")) {
            cfg.mode = option_value(i, argc, argv, arg, "--mode");
        } else if (arg.rfind("--", 0) == 0) {
            throw std::runtime_error("未知选项: " + arg);
        } else if (!have_target) {
            cfg.target = parse_double(arg, "TARGET");
            have_target = true;
        } else {
            throw std::runtime_error("多余的位置参数: " + arg);
        }
    }

    if (deterministic_mode_requested) {
        cfg.genetic = false;
        cfg.pslq = false;
        cfg.egraph = false;
        cfg.mcts = false;
        cfg.portfolio = false;
    } else if (portfolio_mode_requested) {
        // Portfolio mode composes the existing deterministic engines with
        // bounded defaults.  It is opt-in, so ordinary searches retain their
        // exact performance profile.  Explicitly larger user values win.
        cfg.genetic = genetic_mode_requested || genetic_tuning_seen;
        cfg.pslq = !pslq_disabled;
        cfg.egraph = !egraph_disabled;
        cfg.mcts = !mcts_disabled;
        cfg.portfolio = true;
        cfg.stop_on_epsilon = false;
        cfg.explore_pairs = std::max<std::size_t>(cfg.explore_pairs, cfg.genetic ? 1U : 2U);
        // Genetic structure species already provide the diversity lane.  Do
        // not also allocate a Pareto sidecar unless the user explicitly asks
        // for one; running both archives duplicates most of their memory and
        // hash-table work.  Pure portfolio mode retains its structural slot.
        if (!cfg.genetic) cfg.pareto_slots = std::max(cfg.pareto_slots, 2U);
        cfg.inverse_depth = std::max(cfg.inverse_depth, 1U);
        cfg.deep_rounds = std::max(cfg.deep_rounds, 1U);
        if (!deep_frontier_seen) {
            cfg.deep_frontier = std::max<std::size_t>(
                512, std::min<std::size_t>(2'048, std::max<std::size_t>(1, cfg.beam / 4)));
        }
    } else {
        if (genetic_mode_requested || genetic_tuning_seen) cfg.genetic = true;
        if (!pslq_disabled && (pslq_mode_requested || pslq_tuning_seen)) cfg.pslq = true;
        if (!egraph_disabled && (egraph_mode_requested || egraph_tuning_seen)) cfg.egraph = true;
        if (!mcts_disabled && (mcts_mode_requested || mcts_tuning_seen)) cfg.mcts = true;
    }

    if (cfg.list_symbols) return cfg;
    if (cfg.self_test) return cfg;
    if (!have_target) throw std::runtime_error("缺少 TARGET；使用 --help 查看用法");
    if (cfg.max_cost == 0 || cfg.max_cost > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("--max-cost 必须在 1..65535 之间");
    }
    if (cfg.side_cost > cfg.max_cost) throw std::runtime_error("--side-cost 不能大于 --max-cost");
    if (cfg.max_literal_len > 18) throw std::runtime_error("--max-literal-len 不能超过 18");
    if (cfg.digit_cost == 0 || cfg.digit_cost > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("--digit-cost 必须在 1..65535 之间");
    }
    if (cfg.beam == 0) throw std::runtime_error("--beam 必须大于 0");
    if (cfg.pair_budget == 0) throw std::runtime_error("--pairs 必须大于 0");
    if (cfg.threads == 0) cfg.threads = std::max(1U, std::thread::hardware_concurrency());
    if (cfg.task_chunks == 0) throw std::runtime_error("--task-chunks 必须大于 0");
    if (cfg.value_bits > 52) throw std::runtime_error("--value-bits 必须在 0..52 之间");
    if (cfg.explore_pairs > 1'000'000) {
        throw std::runtime_error("--explore-pairs 不能超过 1000000");
    }
    if (cfg.pareto_slots == 0 || cfg.pareto_slots > 4) {
        throw std::runtime_error("--pareto-slots 必须在 1..4 之间");
    }
    if (cfg.inverse_neighbors == 0 || cfg.inverse_neighbors > 256) {
        throw std::runtime_error("--inverse-neighbors 必须在 1..256 之间");
    }
    if (cfg.inverse_depth > 8) throw std::runtime_error("--inverse-depth 不能超过 8");
    if (cfg.inverse_beam == 0) throw std::runtime_error("--inverse-beam 必须大于 0");
    if (cfg.max_abs <= 0.0) throw std::runtime_error("--max-abs 必须大于 0");
    if (cfg.max_exponent <= 0.0) throw std::runtime_error("--max-exponent 必须大于 0");
    if (cfg.max_trig_arg <= 0.0) throw std::runtime_error("--max-trig-arg 必须大于 0");
    if (cfg.epsilon < 0.0) throw std::runtime_error("--epsilon 不能为负数");
    if (cfg.equation_neighbors == 0) throw std::runtime_error("--equation-neighbors 必须大于 0");
    if (cfg.genetic_population == 0) throw std::runtime_error("--genetic-population 必须大于 0");
    if (cfg.genetic_generations == 0) throw std::runtime_error("--genetic-generations 必须大于 0");
    if (cfg.genetic_elegance < 0.0) throw std::runtime_error("--genetic-elegance 不能为负数");
    if (cfg.genetic_repair < 0.0 || cfg.genetic_repair > 1.0) {
        throw std::runtime_error("--genetic-repair 必须在 0..1 之间");
    }
    if (cfg.genetic_repair_depth == 0 || cfg.genetic_repair_depth > 8) {
        throw std::runtime_error("--genetic-repair-depth 必须在 1..8 之间");
    }
    if (cfg.genetic_crossover < 0.0 || cfg.genetic_crossover > 1.0) {
        throw std::runtime_error("--genetic-crossover 必须在 0..1 之间");
    }
    if (cfg.genetic_novelty < 0.0 || cfg.genetic_novelty > 0.5) {
        throw std::runtime_error("--genetic-novelty 必须在 0..0.5 之间");
    }
    if (cfg.genetic_tournament == 0 || cfg.genetic_tournament > 64) {
        throw std::runtime_error("--genetic-tournament 必须在 1..64 之间");
    }
    if (cfg.pslq_basis == 0) throw std::runtime_error("--pslq-basis 必须大于 0");
    if (cfg.pslq_max_coefficient <= 0 || cfg.pslq_max_coefficient > 4096) {
        throw std::runtime_error("--pslq-coeff 必须在 1..4096 之间");
    }
    if (cfg.pslq_steps == 0 || cfg.pslq_steps > 10000) {
        throw std::runtime_error("--pslq-steps 必须在 1..10000 之间");
    }
    if (cfg.pslq_tolerance <= 0.0) throw std::runtime_error("--pslq-tolerance 必须大于 0");
    if (cfg.egraph_seeds == 0) throw std::runtime_error("--egraph-seeds 必须大于 0");
    if (cfg.egraph_rounds == 0 || cfg.egraph_rounds > 64) {
        throw std::runtime_error("--egraph-rounds 必须在 1..64 之间");
    }
    if (cfg.egraph_node_limit == 0) throw std::runtime_error("--egraph-nodes 必须大于 0");
    if (cfg.mcts_iterations == 0) throw std::runtime_error("--mcts-iterations 必须大于 0");
    if (cfg.mcts_depth == 0 || cfg.mcts_depth > 32) {
        throw std::runtime_error("--mcts-depth 必须在 1..32 之间");
    }
    if (cfg.mcts_branching == 0 || cfg.mcts_branching > 256) {
        throw std::runtime_error("--mcts-branching 必须在 1..256 之间");
    }
    if (cfg.mcts_exploration < 0.0) throw std::runtime_error("--mcts-exploration 不能为负数");
    if (cfg.mcts_elegance < 0.0) throw std::runtime_error("--mcts-elegance 不能为负数");
    if (cfg.genetic && cfg.equations) {
        throw std::runtime_error("--genetic 暂不能与 --equations 同时使用");
    }
    if ((cfg.pslq || cfg.egraph || cfg.mcts) && cfg.equations) {
        throw std::runtime_error("PSLQ、e-graph 与 MCTS 当前用于常量表达式搜索，不能与 --equations 同时使用");
    }
    if (cfg.portfolio && cfg.equations) {
        throw std::runtime_error("--search-mode portfolio 用于常量搜索；方程请使用 --equation-search wide 或 exhaustive");
    }
    if (cfg.inverse_depth > 0 && cfg.equations) {
        throw std::runtime_error("--inverse-depth 暂不能与 --equations 同时使用");
    }
    if (cfg.deep_rounds > 0 && cfg.equations) {
        throw std::runtime_error("--deep-rounds 暂不能与 --equations 同时使用");
    }
    if (!cfg.bidirectional && cfg.inverse_depth > 0) {
        throw std::runtime_error("--inverse-depth 不能与 --no-bidirectional 同时使用");
    }
    if (!cfg.bidirectional && cfg.deep_rounds > 0) {
        throw std::runtime_error("--deep-rounds 不能与 --no-bidirectional 同时使用");
    }
    if (cfg.results == 0) throw std::runtime_error("--results 必须大于 0");
    if (live_top_explicit && cfg.live_top == 0) throw std::runtime_error("--live-top 必须大于 0");
    if (cfg.live_interval < 0.0) throw std::runtime_error("--live-interval 不能为负数");
    if (cfg.live && !live_top_explicit) cfg.live_top = cfg.results;
    if (cfg.mode != "pareto" && cfg.mode != "nearest") throw std::runtime_error("--mode 只能是 pareto 或 nearest");
    return cfg;
}

static bool contains_close(const SearchRun& run, double expected, double tolerance) {
    for (const auto& node : run.arena) {
        if (node.eligible && std::abs(node.value - expected) <= tolerance) return true;
    }
    return false;
}

static int run_self_test() {
    int failed = 0;
    auto check = [&](bool condition, const std::string& name) {
        if (condition) std::cerr << "[PASS] " << name << '\n';
        else {
            std::cerr << "[FAIL] " << name << '\n';
            ++failed;
        }
    };

    {
        Config cfg;
        cfg.max_cost = 2;
        cfg.constants = "none";
        cfg.ops = "none";
        const auto atoms = build_atoms(cfg);
        const bool has_25 = std::any_of(atoms.begin(), atoms.end(), [](const AtomSpec& atom) {
            return atom.text == "25";
        });
        const bool within_limit = std::all_of(atoms.begin(), atoms.end(), [](const AtomSpec& atom) {
            return atom.value <= 25.0;
        });
        check(has_25 && within_limit, "默认整数字面量上限 25");
    }
    {
        const auto sub = desired_left(BinaryKind::Sub, 2.0, 5.0);
        const auto div = desired_left(BinaryKind::Div, 2.0, 5.0);
        const auto pow = desired_left(BinaryKind::Pow, 3.0, 125.0);
        check(sub && div && pow && *sub == 7.0 && *div == 10.0 && std::abs(*pow - 5.0) < 1.0e-14,
              "二元运算反向目标求解");
    }
    {
        const ErrorRange range = parse_error_range("(0,1]");
        check(!range.contains(0.0) && range.contains(0.5) && range.contains(1.0) && !range.contains(1.1),
              "结果误差开闭区间");
    }
    {
        const ErrorRange range = make_error_range("0", "1", false, false);
        check(!range.contains(0.0) && range.contains(0.5) && !range.contains(1.0),
              "PowerShell 双参数误差开区间");
    }
    {
        const ErrorRange range = parse_error_range("[-inf,0)");
        check(range.contains(-1.0) && !range.contains(0.0) && !range.contains(1.0),
              "有符号负误差区间");
    }
    {
        std::array<long double, 4> values{2.0L, 1.0L, 0.0L, 0.0L};
        const auto relation = find_integer_relation(values, 2, 1.0e-15L, 8, 32);
        check(relation && relation->coefficients[0] == 1 && relation->coefficients[1] == -2,
              "PSLQ 有界整数关系核心");
    }
    {
        const auto tokens = tokenize_response_text(
            "\xef\xbb\xbf# preset\n520.82418 --ops \"+,/,sqrt\" --digits '' # trailing\n"
            "--error-range '(0,1)' @@literal\n",
            "self-test.args");
        check(tokens == std::vector<std::string>{"520.82418", "--ops", "+,/,sqrt", "--digits", "",
                                                  "--error-range", "(0,1)", "@@literal"},
              "UTF-8 参数文件引号、空值与注释解析");
    }
    {
        const auto parse_arguments = [](std::vector<std::string> arguments) {
            std::vector<char*> pointers;
            pointers.reserve(arguments.size());
            for (std::string& argument : arguments) pointers.push_back(argument.data());
            return parse_cli(static_cast<int>(pointers.size()), pointers.data());
        };
        const Config automatic = parse_arguments(
            {"fates", "1", "--genetic-population", "32"});
        const Config deterministic_first = parse_arguments(
            {"fates", "1", "--search-mode", "deterministic", "--genetic-population", "32"});
        const Config deterministic_last = parse_arguments(
            {"fates", "1", "--genetic-population", "32", "--search-mode", "deterministic"});
        const Config deterministic_with_flag = parse_arguments(
            {"fates", "1", "--search-mode", "deterministic", "--genetic"});
        const Config exact_and_explore = parse_arguments(
            {"fates", "1", "--value-prune", "exact", "--explore-pairs", "3"});
        const Config portfolio = parse_arguments(
            {"fates", "1", "--search-mode", "portfolio"});
        const Config genetic_portfolio = parse_arguments(
            {"fates", "1", "--genetic", "--search-mode", "portfolio"});
        const Config advanced = parse_arguments(
            {"fates", "1", "--search-mode", "pslq", "--egraph", "--mcts-iterations", "32"});
        const Config reduced_portfolio = parse_arguments(
            {"fates", "1", "--search-mode", "portfolio", "--no-mcts"});
        const Config equation_policy = parse_arguments(
            {"fates", "1", "--equations", "--equation-search", "wide",
             "--equation-quality", "local"});
        check(automatic.genetic && !deterministic_first.genetic && !deterministic_last.genetic &&
                   !deterministic_with_flag.genetic &&
                   exact_and_explore.value_prune == ValuePruneMode::Exact &&
                   exact_and_explore.explore_pairs == 3 && portfolio.portfolio &&
                   portfolio.inverse_depth >= 1 && portfolio.deep_rounds >= 1 &&
                   portfolio.pareto_slots >= 2 && !portfolio.stop_on_epsilon &&
                   genetic_portfolio.portfolio && genetic_portfolio.genetic &&
                   genetic_portfolio.pareto_slots == 1 &&
                   genetic_portfolio.explore_pairs == 1 &&
                   genetic_portfolio.deep_frontier > 0 &&
                    portfolio.pslq && portfolio.egraph && portfolio.mcts &&
                    advanced.pslq && advanced.egraph && advanced.mcts &&
                    reduced_portfolio.pslq && reduced_portfolio.egraph && !reduced_portfolio.mcts &&
                    equation_policy.equation_search == EquationSearchMode::Wide &&
                   equation_policy.equation_quality == EquationQualityMode::Local,
              "搜索模式、严格数值剪枝与方程策略解析稳定");
    }
    {
        std::vector<std::string> arguments{
            "fates", "1", "--equations", "--deep-rounds", "1"};
        std::vector<char*> pointers;
        pointers.reserve(arguments.size());
        for (std::string& argument : arguments) pointers.push_back(argument.data());
        bool rejected = false;
        try {
            (void)parse_cli(static_cast<int>(pointers.size()), pointers.data());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "方程模式拒绝无效 deep-rounds");
    }
    {
        const ParsedCountSpec exact = parse_symbol_count_spec("pi=4");
        const ParsedCountSpec ranged = parse_symbol_count_spec("pi=2:inf");
        check(exact.symbol == "pi" && exact.minimum == 4 && exact.maximum == 4 &&
                  ranged.minimum == 2 && !ranged.maximum,
              "符号次数 exact/inf 语法");
    }
    {
        Config cfg;
        cfg.digits = "";
        cfg.constants = "pi";
        cfg.max_cost = 9;
        cfg.symbol_count_specs = {"pi=4"};
        const auto atoms = build_atoms(cfg);
        const auto plan = compile_symbol_constraints(cfg, atoms);
        const auto one = plan.atom_state("pi");
        auto state = one;
        for (unsigned i = 1; i < 4 && state; ++i) state = plan.combine(*state, *one);
        const auto too_many = state ? plan.combine(*state, *one) : std::nullopt;
        check(one && state && plan.satisfied(*state) && !too_many,
              "符号次数状态在组合时执行最大值剪枝");
    }
    {
        Config cfg;
        cfg.digits = "";
        cfg.constants = "pi";
        cfg.max_cost = 7;
        cfg.symbol_count_specs = {"pi=2:inf"};
        const auto atoms = build_atoms(cfg);
        const auto plan = compile_symbol_constraints(cfg, atoms);
        const auto one = plan.atom_state("pi");
        const auto two = one ? plan.combine(*one, *one) : std::nullopt;
        const auto four = two ? plan.combine(*two, *two) : std::nullopt;

        cfg.symbol_count_specs = {"pi=0"};
        const auto forbidden = compile_symbol_constraints(cfg, atoms).atom_state("pi");
        check(two && four && plan.satisfied(*two) && plan.satisfied(*four) && !forbidden,
              "符号次数无限上限饱和与零上限禁用");
    }
    {
        Config cfg;
        cfg.digits = "145";
        cfg.max_literal_len = 1;
        cfg.constants = "none";
        cfg.max_cost = 16;
        cfg.required_symbol_order = {"1", "1", "4", "5", "1", "4"};
        const auto atoms = build_atoms(cfg);
        const auto plan = compile_symbol_constraints(cfg, atoms);
        std::optional<std::uint32_t> state = 0U;
        for (const std::string& symbol : cfg.required_symbol_order) {
            const auto atom = plan.atom_state(symbol);
            state = state && atom ? plan.combine(*state, *atom) : std::nullopt;
        }
        std::optional<std::uint32_t> wrong = 0U;
        for (const std::string_view symbol : {"1", "4", "1", "5", "1", "4"}) {
            const auto atom = plan.atom_state(symbol);
            wrong = wrong && atom ? plan.combine(*wrong, *atom) : std::nullopt;
        }
        check(state && plan.satisfied(*state) && (!wrong || !plan.satisfied(*wrong)),
              "重复符号叶子顺序自动机");
    }
    {
        ConstraintExtension constraint;
        constraint.name = "two-leaves";
        constraint.state_bits = 2;
        constraint.atom = [](const ExtensionAtomContext&) {
            return std::optional<std::uint32_t>{1U};
        };
        constraint.binary = [](std::uint32_t left,
                               std::uint32_t right,
                               const ExtensionBinaryContext&) -> std::optional<std::uint32_t> {
            const std::uint32_t total = left + right;
            if (total > 2) return std::nullopt;
            return total;
        };
        constraint.satisfied = [](std::uint32_t state) { return state == 2; };
        ExtensionConstraintPlan plan{{{&constraint, 0, 3}}, true};
        const AtomSpec atom{"a", 1.0, 1};
        const auto one = plan.atom_state(atom);
        const auto two = one ? plan.apply_binary(*one, *one, "+", 1.0, 1.0, 2.0, 3, 4)
                             : std::nullopt;
        const auto three = two ? plan.apply_binary(*two, *one, "+", 2.0, 1.0, 3.0, 5, 6)
                               : std::nullopt;
        check(one && two && plan.satisfied(*two) && !three,
              "自定义状态约束支持原子、运算转移与提前剪枝");
    }
    {
        const auto sinh_kind = parse_unary_name("sinh");
        const auto gamma_kind = parse_unary_name("Γ");
        Config cfg;
        cfg.max_abs = 100.0;
        cfg.symbol_constraints = compile_symbol_constraints(cfg, {});
        cfg.extension_constraints = compile_extension_constraints();
        std::vector<Node> arena(1);
        arena[0].value = 1.0;
        arena[0].cost = 1;
        arena[0].nodes = 1;
        arena[0].depth = 1;
        arena[0].hash = 1;
        arena[0].derivative = 1.0;
        arena[0].depends_on_x = true;
        const auto sinh_result = sinh_kind
            ? apply_unary(cfg, arena,
                          {*sinh_kind, static_cast<std::uint16_t>(default_unary_cost(*sinh_kind))},
                          0, 3)
            : std::nullopt;
        arena[0].value = 0.5;
        const auto gamma_result = gamma_kind
            ? apply_unary(cfg, arena,
                          {*gamma_kind, static_cast<std::uint16_t>(default_unary_cost(*gamma_kind))},
                          0, 3)
            : std::nullopt;
        struct UnarySample {
            const char* name;
            double input;
            double expected;
        };
        const std::array<UnarySample, 5> hyperbolic_samples{{
            {"cosh", 1.0, std::cosh(1.0)},
            {"tanh", 1.0, std::tanh(1.0)},
            {"asinh", 1.0, std::asinh(1.0)},
            {"acosh", 2.0, std::acosh(2.0)},
            {"atanh", 0.5, std::atanh(0.5)},
        }};
        bool hyperbolic_ok = true;
        for (const UnarySample& sample : hyperbolic_samples) {
            arena[0].value = sample.input;
            const auto kind = parse_unary_name(sample.name);
            const auto result = kind
                ? apply_unary(cfg, arena,
                              {*kind, static_cast<std::uint16_t>(default_unary_cost(*kind))}, 0, 3)
                : std::nullopt;
            hyperbolic_ok = hyperbolic_ok && result &&
                            std::abs(result->value - sample.expected) < 1.0e-15;
        }
        const auto [default_unary, default_binary] = parse_ops(Config{}.ops);
        constexpr double derivative_step = 1.0e-6;
        const double gamma_derivative_reference =
            (std::tgamma(0.5 + derivative_step) - std::tgamma(0.5 - derivative_step)) /
            (2.0 * derivative_step);
        const bool advanced_default_off = std::none_of(
            default_unary.begin(), default_unary.end(), [](const UnarySpec& spec) {
                return spec.kind == UnaryKind::Sinh || spec.kind == UnaryKind::Cosh ||
                       spec.kind == UnaryKind::Tanh || spec.kind == UnaryKind::Asinh ||
                       spec.kind == UnaryKind::Acosh || spec.kind == UnaryKind::Atanh ||
                       spec.kind == UnaryKind::Gamma;
            });
        check(sinh_result && gamma_result && hyperbolic_ok && gamma_kind == UnaryKind::Gamma &&
                  std::abs(sinh_result->value - std::sinh(1.0)) < 1.0e-15 &&
                  std::abs(gamma_result->value - std::sqrt(std::numbers::pi_v<double>)) < 1.0e-14 &&
                  std::abs(gamma_result->derivative - gamma_derivative_reference) < 1.0e-8 &&
                  advanced_default_off && !default_binary.empty(),
              "双曲函数与 Gamma 原生求值且默认关闭");
    }
    {
        const auto kind = parse_unary_name("sigmoid");
        Config cfg;
        cfg.max_abs = 100.0;
        cfg.symbol_constraints = compile_symbol_constraints(cfg, {});
        cfg.extension_constraints = compile_extension_constraints();
        std::vector<Node> arena(1);
        arena[0].value = 1.0;
        arena[0].cost = 1;
        arena[0].nodes = 1;
        arena[0].depth = 1;
        arena[0].hash = 1;
        const auto result = kind
            ? apply_unary(cfg, arena,
                          {*kind, static_cast<std::uint16_t>(default_unary_cost(*kind))}, 0, 3)
            : std::nullopt;
        check(result && std::abs(result->value - 1.0 / (1.0 + std::exp(-1.0))) < 1.0e-15,
              "源码扩展一元运算参与统一求值路径");
    }
    {
        std::vector<AtomSpec> atoms{{"pi", std::numbers::pi_v<double>, 1}, {"q", 0.5, 1}};
        std::vector<Node> arena(6);
        for (std::uint32_t index = 0; index < 2; ++index) {
            arena[index].tag = NodeTag::Atom;
            arena[index].atom_index = index;
        }
        arena[2].tag = NodeTag::Binary;
        arena[2].op = static_cast<std::uint8_t>(BinaryKind::Mul);
        arena[2].left = 0;
        arena[2].right = 0;
        arena[3].tag = NodeTag::Binary;
        arena[3].op = static_cast<std::uint8_t>(BinaryKind::Div);
        arena[3].left = 2;
        arena[3].right = 1;
        arena[4].tag = NodeTag::Binary;
        arena[4].op = static_cast<std::uint8_t>(BinaryKind::Div);
        arena[4].left = 1;
        arena[4].right = 0;
        arena[5].tag = NodeTag::Binary;
        arena[5].op = static_cast<std::uint8_t>(BinaryKind::Div);
        arena[5].left = 0;
        arena[5].right = 4;
        arena.resize(8);
        arena[6].tag = NodeTag::Unary;
        arena[6].op = static_cast<std::uint8_t>(UnaryKind::Inv);
        arena[6].left = 0;
        arena[7].tag = NodeTag::Unary;
        arena[7].op = static_cast<std::uint8_t>(UnaryKind::Exp);
        arena[7].left = 0;
        const std::string first = render_expression_latex_impl(atoms, arena, 3).text;
        const std::string second = render_expression_latex_impl(atoms, arena, 5).text;
        const std::string text = render_expression_impl(atoms, arena, 3).text;
        const std::string inverse = render_expression_latex_impl(atoms, arena, 6).text;
        const std::string exponential = render_expression_latex_impl(atoms, arena, 7).text;
        check(first == second && first.find("\\times") != std::string::npos &&
                  first.find("^{2}") == std::string::npos && text == "pi×pi/q" &&
                  inverse == "\\frac{1}{\\pi}" && exponential == "e^{\\pi}",
              "乘除等价式显式叉乘、直观 LaTeX 且不凭空引入幂");
    }
    {
        std::vector<AtomSpec> atoms{{"gamma", 0.5772156649015329, 2},
                                    {"catalan", 0.915965594177219, 2}};
        std::vector<Node> arena(2);
        arena[0].tag = NodeTag::Atom;
        arena[0].atom_index = 0;
        arena[1].tag = NodeTag::Atom;
        arena[1].atom_index = 1;
        check(render_expression_latex_impl(atoms, arena, 0).text == "\\gamma" &&
                  render_expression_latex_impl(atoms, arena, 1).text == "G",
              "Euler-Mascheroni 与 Catalan 常数 LaTeX 符号");
    }
    {
        Config cfg;
        cfg.symbol_constraints = compile_symbol_constraints(cfg, {});
        cfg.extension_constraints = compile_extension_constraints();
        std::vector<Node> arena(1);
        arena[0].value = 0.5;
        arena[0].cost = 1;
        arena[0].nodes = 1;
        arena[0].depth = 1;
        arena[0].hash = 1;
        const auto asin_candidate = apply_unary(cfg, arena, {UnaryKind::Asin, 2}, 0, 3);
        Node asin_node;
        if (asin_candidate) {
            asin_node.value = asin_candidate->value;
            asin_node.cost = asin_candidate->cost;
            asin_node.nodes = asin_candidate->nodes;
            asin_node.depth = asin_candidate->depth;
            asin_node.hash = asin_candidate->hash;
            asin_node.tag = asin_candidate->tag;
            asin_node.op = asin_candidate->op;
            asin_node.left = asin_candidate->left;
            arena.push_back(asin_node);
        }
        const auto eliminated = asin_candidate
            ? apply_unary(cfg, arena, {UnaryKind::Sin, 2}, 1, 5)
            : std::nullopt;
        const auto sin_candidate = apply_unary(cfg, arena, {UnaryKind::Sin, 2}, 0, 3);
        Node sin_node;
        if (sin_candidate) {
            sin_node.value = sin_candidate->value;
            sin_node.cost = sin_candidate->cost;
            sin_node.nodes = sin_candidate->nodes;
            sin_node.depth = sin_candidate->depth;
            sin_node.hash = sin_candidate->hash;
            sin_node.tag = sin_candidate->tag;
            sin_node.op = sin_candidate->op;
            sin_node.left = sin_candidate->left;
            arena.push_back(sin_node);
        }
        const auto retained = sin_candidate
            ? apply_unary(cfg, arena, {UnaryKind::Asin, 2}, static_cast<ExprId>(arena.size() - 1), 5)
            : std::nullopt;
        check(!eliminated && retained,
              "仅消去严格逆函数方向并保留 asin(sin(x))");
    }

    {
        Config cfg;
        cfg.target = std::numbers::pi_v<double>;
        cfg.digits = "";
        cfg.constants = "pi";
        cfg.ops = "none";
        cfg.max_cost = 1;
        cfg.beam = 32;
        cfg.pair_budget = 1000;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, std::numbers::pi_v<double>, 0.0), "内置常数 pi");
    }
    {
        Config cfg;
        cfg.target = 2.0;
        cfg.digits = "";
        cfg.constants = "pi";
        cfg.ops = "+,/";
        cfg.max_cost = 7;
        cfg.beam = 512;
        cfg.pair_budget = 100'000;
        cfg.value_bits = 52;
        cfg.mode = "nearest";
        cfg.results = 10;
        cfg.symbol_count_specs = {"pi=4"};
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        const auto matches = collect_matches(run);
        const bool found = std::any_of(matches.begin(), matches.end(), [&](const Match& match) {
            return render_expression(run, match.id).text == "pi/pi+pi/pi";
        });
        check(found, "恰好四个 pi 构造 pi/pi+pi/pi");
    }
    {
        Config cfg;
        cfg.target = std::sqrt(2.0);
        cfg.digits = "2";
        cfg.constants = "none";
        cfg.ops = "sqrt";
        cfg.max_cost = 2;
        cfg.beam = 64;
        cfg.pair_budget = 1000;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, std::sqrt(2.0), 1.0e-15), "sqrt(2) 构造");
    }
    {
        Config cfg;
        cfg.target = 22.0 / 7.0;
        cfg.digits = "27";
        cfg.max_literal_len = 2;
        cfg.constants = "none";
        cfg.ops = "/";
        cfg.max_cost = 4;
        cfg.beam = 256;
        cfg.pair_budget = 100000;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, 22.0 / 7.0, 1.0e-15), "22/7 构造");
    }
    {
        Config cfg;
        cfg.target = 24.0;
        cfg.digits = "1234";
        cfg.max_literal_len = 1;
        cfg.constants = "none";
        cfg.ops = "+,-,*,/";
        cfg.max_cost = 7;
        cfg.beam = 1024;
        cfg.pair_budget = 500'000;
        cfg.value_bits = 52;
        cfg.mode = "nearest";
        cfg.results = 10;
        cfg.symbol_count_specs = {"1=1", "2=1", "3=1", "4=1"};
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        const auto matches = collect_matches(run);
        check(!matches.empty() && matches.front().value == 24.0 && matches.front().cost == 7,
              "24 点中 1、2、3、4 各使用一次");
    }
    {
        Config cfg;
        cfg.target = 625.0;
        cfg.digits = "25";
        cfg.constants = "none";
        cfg.ops = "^";
        cfg.max_cost = 5;
        cfg.beam = 256;
        cfg.pair_budget = 100'000;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, 625.0, 0.0) && run.stats.used_mitm && run.stats.generated_cost == 4,
              "半表达式合并构造 25^2");
    }
    {
        Config cfg;
        cfg.target = 3.0;
        cfg.digits = "2";
        cfg.constants = "none";
        cfg.ops = "+,-,/";
        cfg.max_cost = 6;
        cfg.beam = 256;
        cfg.pair_budget = 100000;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, 3.0, 1.0e-15), "受限数字下构造 3");
    }
    {
        Config cfg;
        cfg.target = std::sqrt(2.0);
        cfg.digits = "2";
        cfg.constants = "none";
        cfg.ops = "^";
        cfg.max_cost = 5;
        cfg.beam = 512;
        cfg.pair_budget = 100'000;
        cfg.equations = true;
        cfg.mode = "nearest";
        cfg.results = 10;
        cfg.symbol_count_specs = {"2=2"};
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        const auto equations = collect_equation_matches(run.cfg, run.arena, run.cfg.results);
        const bool found = std::any_of(equations.begin(), equations.end(), [&](const EquationMatch& match) {
            const std::string text = render_expression(run, match.left).text + " = " +
                                     render_expression(run, match.right).text;
            return text == "x^2 = 2";
        });
        check(found, "方程模式按等号两侧合计两次数字 2");
    }
    {
        Config cfg;
        cfg.target = 21.0;
        cfg.equations = true;
        std::vector<Node> arena(5);

        arena[0].value = cfg.target;
        arena[0].derivative = 1.0;
        arena[0].tag = NodeTag::Atom;
        arena[0].depends_on_x = true;

        arena[1].value = std::numbers::pi_v<double>;
        arena[1].tag = NodeTag::Atom;

        arena[2].value = cfg.target * std::numbers::pi_v<double>;
        arena[2].derivative = std::numbers::pi_v<double>;
        arena[2].tag = NodeTag::Binary;
        arena[2].op = static_cast<std::uint8_t>(BinaryKind::Mul);
        arena[2].left = 0;
        arena[2].right = 1;
        arena[2].depends_on_x = true;

        arena[3].value = std::tan(arena[2].value);
        const double cosine = std::cos(arena[2].value);
        arena[3].derivative = arena[2].derivative / (cosine * cosine);
        arena[3].tag = NodeTag::Unary;
        arena[3].op = static_cast<std::uint8_t>(UnaryKind::Tan);
        arena[3].left = 2;
        arena[3].depends_on_x = true;

        arena[4].value = arena[0].value - arena[3].value;
        arena[4].derivative = arena[0].derivative - arena[3].derivative;
        arena[4].tag = NodeTag::Binary;
        arena[4].op = static_cast<std::uint8_t>(BinaryKind::Sub);
        arena[4].left = 0;
        arena[4].right = 3;
        arena[4].depends_on_x = true;

        const double residual = arena[0].value - arena[4].value;
        const double slope = arena[0].derivative - arena[4].derivative;
        const double root = cfg.target - residual / slope;
        const bool strict_rejects = !equation_is_stable(cfg, arena, 0, 4, residual, slope, root);
        cfg.equation_quality = EquationQualityMode::Off;
        const bool legacy_accepts = equation_is_stable(cfg, arena, 0, 4, residual, slope, root);
        check(strict_rejects && legacy_accepts,
              "严格方程质量过滤单位周期伪根且保留兼容开关");
    }
    {
        Config cfg;
        cfg.target = std::sqrt(2.0);
        cfg.equations = true;
        std::vector<Node> arena(2);
        arena[0].value = cfg.target;
        arena[0].derivative = 1.0;
        arena[0].tag = NodeTag::Atom;
        arena[0].depends_on_x = true;
        arena[1].value = std::pow(cfg.target, cfg.target);
        arena[1].derivative = arena[1].value * (std::log(cfg.target) + 1.0);
        arena[1].tag = NodeTag::Binary;
        arena[1].op = static_cast<std::uint8_t>(BinaryKind::Pow);
        arena[1].left = 0;
        arena[1].right = 0;
        arena[1].depends_on_x = true;
        const double residual = arena[0].value - arena[1].value;
        const double slope = arena[0].derivative - arena[1].derivative;
        const double one_step = cfg.target - residual / slope;
        const auto root = refine_equation_root(cfg, arena, 0, 1, one_step);
        check(root && std::abs(*root - 1.0) < 1.0e-5 && std::abs(one_step - 1.0) > 0.1,
              "方程结果迭代到实际根而非一次 Newton 估计");
    }
    {
        Config cfg;
        const double inner = std::log(2.0) + 1.0 / std::numbers::pi_v<double>;
        cfg.target = std::pow(8.0 * std::sqrt(inner), 3.0);
        cfg.digits = "238";
        cfg.constants = "pi";
        cfg.ops = "+,*,^,inv,sqrt,ln";
        // Keep the required unary/binary child exactly at the generated-side limit.
        // This exercises terminal repair rather than ordinary half-tree generation.
        cfg.max_cost = 14;
        cfg.beam = 768;
        cfg.pair_budget = 300'000;
        cfg.value_bits = 48;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        const bool found = std::any_of(run.arena.begin(), run.arena.end(), [&](const Node& node) {
            return std::abs(node.value - cfg.target) <= 1.0e-12 && node.cost <= 12;
        });
        check(found && run.stats.used_mitm && run.stats.generated_cost == 7,
              "边界成本不平衡整数幂的多级反推");
    }
    {
        Config cfg;
        cfg.target = std::pow(std::sqrt(2.0) + std::sqrt(3.0), 2.0);
        cfg.digits = "23";
        cfg.constants = "none";
        cfg.ops = "+,sqrt,^";
        cfg.max_cost = 8;
        cfg.side_cost = 4;
        cfg.beam = 50;
        cfg.pair_budget = 1000;
        cfg.value_bits = 48;
        cfg.deep_rounds = 1;
        cfg.deep_beam = 100;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, cfg.target, 2.0e-15) && run.stats.used_deep_compositions,
              "通用深层拼接构造 (sqrt(2)+sqrt(3))^2");
    }
    {
        Config cfg;
        cfg.target = 2.0;
        for (unsigned i = 0; i < 5; ++i) cfg.target = std::sqrt(cfg.target);
        cfg.digits = "2";
        cfg.constants = "none";
        cfg.ops = "+:5,sqrt";
        cfg.max_cost = 6;
        cfg.side_cost = 4;
        cfg.beam = 50;
        cfg.pair_budget = 1'000;
        cfg.inverse_depth = 2;
        cfg.inverse_beam = 32;
        cfg.inverse_budget = 10'000;
        cfg.show_stats = false;
        const SearchRun run = SearchEngine(cfg).run();
        const auto matches = collect_matches(run);
        const bool found = std::any_of(matches.begin(), matches.end(), [&](const Match& match) {
            return match.cost == 6 && std::abs(match.value - cfg.target) <= 1.0e-15;
        });
        check(found && run.stats.used_inverse_templates && run.stats.inverse_candidates > 0,
              "Pareto 输出递归 inverse 独占成本层");
    }
    {
        Config cfg;
        cfg.target = 5.2;
        cfg.digits = "12345";
        cfg.constants = "pi,e";
        cfg.ops = "+,-,*,/,sqrt,ln,exp,neg,inv";
        cfg.max_cost = 8;
        cfg.beam = 300;
        cfg.pair_budget = 50'000;
        cfg.stop_on_epsilon = false;
        cfg.pslq = true;
        cfg.pslq_basis = 32;
        cfg.pslq_pairs = 64;
        cfg.pslq_tolerance = 1.0e-5;
        cfg.egraph = true;
        cfg.egraph_seeds = 64;
        cfg.egraph_node_limit = 64;
        cfg.mcts = true;
        cfg.mcts_iterations = 128;
        cfg.mcts_depth = 3;
        cfg.mcts_branching = 8;
        cfg.show_stats = false;
        const SearchRun run = SearchEngine(cfg).run();
        check(run.stats.used_pslq && run.stats.pslq_relations > 0 &&
                  run.stats.pslq_candidates > 0 && run.stats.used_egraph &&
                  run.stats.egraph_rewrites > 0 && run.stats.egraph_candidates > 0 &&
                  run.stats.used_mcts && run.stats.mcts_expansions > 0 &&
                  run.stats.mcts_candidates > 0,
              "PSLQ、e-graph 与 MCTS 组合阶段产出合法候选");
    }
    {
        Config base;
        base.target = 5.2;
        base.digits = "12345";
        base.constants = "pi,e";
        base.ops = "+,-,*,/,sqrt,ln,exp,neg,inv";
        base.max_cost = 8;
        base.beam = 200;
        base.pair_budget = 20'000;
        base.stop_on_epsilon = false;
        base.mcts = true;
        base.mcts_iterations = 96;
        base.mcts_depth = 3;
        base.mcts_branching = 8;
        base.mcts_seed = 0x5eed1234ULL;
        base.show_stats = false;
        Config single_cfg = base;
        single_cfg.threads = 1;
        Config parallel_cfg = base;
        parallel_cfg.threads = 4;
        const SearchRun single = SearchEngine(single_cfg).run();
        const SearchRun parallel = SearchEngine(parallel_cfg).run();
        const auto hashes = [](const SearchRun& run) {
            std::vector<std::uint64_t> result;
            for (const Node& node : run.arena) {
                if (node.eligible) result.push_back(node.hash);
            }
            return result;
        };
        check(single.stats.mcts_candidates > 0 && hashes(single) == hashes(parallel) &&
                  single.stats.mcts_expansions == parallel.stats.mcts_expansions &&
                  single.stats.mcts_candidates == parallel.stats.mcts_candidates,
              "MCTS 固定种子跨线程确定性");
    }
    {
        Config cfg;
        cfg.target = -4.0;
        cfg.digits = "145";
        cfg.max_literal_len = 1;
        cfg.constants = "none";
        cfg.ops = "-";
        cfg.max_cost = 11;
        cfg.beam = 1'000;
        cfg.pair_budget = 500'000;
        cfg.mode = "nearest";
        cfg.results = 10;
        cfg.symbol_count_specs = {"1=3", "4=2", "5=1"};
        cfg.required_symbol_order = {"1", "1", "4", "5", "1", "4"};
        cfg.pslq = true;
        cfg.pslq_basis = 32;
        cfg.pslq_pairs = 64;
        cfg.egraph = true;
        cfg.egraph_seeds = 64;
        cfg.egraph_rounds = 1;
        cfg.egraph_node_limit = 64;
        cfg.mcts = true;
        cfg.mcts_iterations = 128;
        cfg.mcts_depth = 3;
        cfg.mcts_branching = 8;
        cfg.show_stats = false;
        const SearchRun run = SearchEngine(cfg).run();
        const auto matches = collect_matches(run);
        const bool found = std::any_of(matches.begin(), matches.end(), [&](const Match& match) {
            return match.value == cfg.target &&
                   render_expression(run, match.id).text == "1-(1-4)-(5-(1-4))";
        });
        check(found && run.stats.used_pslq && run.stats.used_egraph && run.stats.used_mcts,
              "高级阶段遵守符号次数与叶子顺序约束");
    }
    {
        Config cfg;
        cfg.target = std::pow(std::sqrt(2.0) + std::sqrt(3.0), 2.0);
        cfg.digits = "23";
        cfg.constants = "none";
        cfg.ops = "+,sqrt,^";
        cfg.max_cost = 8;
        cfg.side_cost = 4;
        cfg.beam = 50;
        cfg.pair_budget = 1000;
        cfg.value_bits = 48;
        cfg.genetic = true;
        cfg.genetic_population = 4096;
        cfg.genetic_generations = 64;
        cfg.genetic_repair = 0.5;
        cfg.genetic_repair_depth = 3;
        cfg.show_stats = false;
        SearchRun run = SearchEngine(cfg).run();
        check(contains_close(run, cfg.target, 2.0e-15) && run.stats.used_genetic &&
                  run.stats.genetic_generations > 0 && run.stats.genetic_repairs > 0 &&
                  run.stats.genetic_repairs_kept > 0 && run.stats.genetic_crossovers > 0 &&
                  run.stats.genetic_crossovers_kept > 0,
              "遗传模式构造优雅多层表达式");
    }
    {
        Config base;
        base.target = std::pow(std::sqrt(2.0) + std::sqrt(3.0), 2.0);
        base.digits = "23";
        base.constants = "none";
        base.ops = "+,sqrt,^";
        base.max_cost = 7;
        base.side_cost = 4;
        base.beam = 64;
        base.pair_budget = 2'000;
        base.value_bits = 48;
        base.genetic = true;
        base.genetic_population = 512;
        base.genetic_generations = 8;
        base.genetic_repair = 0.5;
        base.genetic_repair_depth = 3;
        base.genetic_seed = 0x5eed1234ULL;
        base.show_stats = false;

        Config single_cfg = base;
        single_cfg.threads = 1;
        Config parallel_cfg = base;
        parallel_cfg.threads = 4;
        const SearchRun single = SearchEngine(single_cfg).run();
        const SearchRun parallel = SearchEngine(parallel_cfg).run();
        const auto eligible_hashes = [](const SearchRun& run) {
            std::vector<std::uint64_t> hashes;
            hashes.reserve(run.arena.size());
            for (const Node& node : run.arena) {
                if (node.eligible) hashes.push_back(node.hash);
            }
            return hashes;
        };
        check(eligible_hashes(single) == eligible_hashes(parallel) &&
                  single.stats.attempted == parallel.stats.attempted &&
                  single.stats.valid == parallel.stats.valid &&
                  single.stats.genetic_repairs == parallel.stats.genetic_repairs &&
                  single.stats.genetic_repairs_kept == parallel.stats.genetic_repairs_kept &&
                  single.stats.genetic_crossovers == parallel.stats.genetic_crossovers &&
                  single.stats.genetic_crossovers_kept == parallel.stats.genetic_crossovers_kept,
              "遗传模式固定种子跨线程确定性");
    }

    if (failed == 0) std::cerr << "全部自测通过。\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace fates

static void configure_utf8_console() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

static int run_fates(int argc, char** argv) {
    if (fates::handle_direct_control_option(argc, argv)) return 0;

    std::vector<std::string> expanded_arguments = fates::expand_cli_arguments(argc, argv);
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(expanded_arguments.size());
    for (std::string& argument : expanded_arguments) argument_pointers.push_back(argument.data());
    fates::Config cfg = fates::parse_cli(
        static_cast<int>(argument_pointers.size()), argument_pointers.data());
    if (cfg.list_symbols) {
        fates::print_symbol_list(cfg.json);
        return 0;
    }
    if (cfg.self_test) return fates::run_self_test();
    fates::SearchEngine engine(std::move(cfg));
    if (engine.dry_run_requested()) {
        engine.print_configuration();
        return 0;
    }
    fates::SearchRun run = engine.run();
    fates::print_results(run);
    return 0;
}

static int report_fates_error(const std::exception& error) {
    std::cerr << "错误: " << error.what() << '\n';
    return 2;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    configure_utf8_console();
    try {
        std::vector<std::string> utf8_arguments;
        utf8_arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            utf8_arguments.push_back(fates::utf16_to_utf8(argv[i]));
        }
        std::vector<char*> argument_pointers;
        argument_pointers.reserve(utf8_arguments.size());
        for (std::string& argument : utf8_arguments) argument_pointers.push_back(argument.data());
        return run_fates(static_cast<int>(argument_pointers.size()), argument_pointers.data());
    } catch (const std::exception& error) {
        return report_fates_error(error);
    }
}
#else
int main(int argc, char** argv) {
    configure_utf8_console();
    try {
        return run_fates(argc, argv);
    } catch (const std::exception& error) {
        return report_fates_error(error);
    }
}
#endif
