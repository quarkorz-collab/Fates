#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fates {

struct ExtensionLimits {
    double max_abs{};
    double max_exponent{};
    double max_trig_arg{};
};

struct UnaryOperationExtension {
    using Evaluator = std::function<std::optional<double>(double, const ExtensionLimits&)>;
    using Derivative = std::function<double(double, double, double)>;
    using Inverse = std::function<std::vector<double>(double, const ExtensionLimits&)>;
    using Renderer = std::function<std::string(std::string_view)>;

    std::string name;
    std::vector<std::string> aliases;
    std::uint16_t default_cost{2};
    Evaluator evaluate;
    Derivative derivative;
    Inverse inverse;
    Renderer render_text;
    Renderer render_latex;
};

struct BinaryOperationExtension {
    using Evaluator = std::function<std::optional<double>(double, double, const ExtensionLimits&)>;
    using Derivative = std::function<double(double, double, double, double, double)>;
    using DesiredOperand = std::function<std::optional<double>(double, double)>;
    using Renderer = std::function<std::string(std::string_view, std::string_view)>;

    std::string name;
    std::vector<std::string> aliases;
    std::uint16_t default_cost{1};
    bool commutative{};
    Evaluator evaluate;
    Derivative derivative;
    DesiredOperand desired_left;
    DesiredOperand desired_right;
    Renderer render_text;
    Renderer render_latex;
};

struct ExtensionAtomContext {
    std::string_view symbol;
    double value{};
    unsigned cost{};
    bool variable{};
};

struct ExtensionUnaryContext {
    std::string_view operation;
    double input{};
    double value{};
    unsigned total_cost{};
};

struct ExtensionBinaryContext {
    std::string_view operation;
    double left{};
    double right{};
    double value{};
    unsigned total_cost{};
};

struct ConstraintExtension {
    using State = std::uint32_t;
    using AtomTransition = std::function<std::optional<State>(const ExtensionAtomContext&)>;
    using UnaryTransition =
        std::function<std::optional<State>(State, const ExtensionUnaryContext&)>;
    using BinaryTransition =
        std::function<std::optional<State>(State, State, const ExtensionBinaryContext&)>;
    using FinalPredicate = std::function<bool(State)>;
    using FeasibilityPredicate = std::function<bool(State, unsigned, unsigned)>;

    std::string name;
    std::uint8_t state_bits{};
    AtomTransition atom;
    UnaryTransition unary;
    BinaryTransition binary;
    FinalPredicate satisfied;
    FeasibilityPredicate can_finish;
};

class ExtensionRegistry {
public:
    void add_unary(UnaryOperationExtension operation) {
        validate_operation(operation.name, operation.default_cost, static_cast<bool>(operation.evaluate));
        validate_aliases(operation.name, operation.aliases);
        ensure_name_available(operation.name);
        for (const std::string& alias : operation.aliases) ensure_name_available(alias);
        unary_.push_back(std::move(operation));
    }

    void add_binary(BinaryOperationExtension operation) {
        validate_operation(operation.name, operation.default_cost, static_cast<bool>(operation.evaluate));
        validate_aliases(operation.name, operation.aliases);
        ensure_name_available(operation.name);
        for (const std::string& alias : operation.aliases) ensure_name_available(alias);
        binary_.push_back(std::move(operation));
    }

    void add_constraint(ConstraintExtension constraint) {
        if (constraint.name.empty()) throw std::runtime_error("自定义约束名称不能为空");
        if (constraint.state_bits == 0 || constraint.state_bits > 32) {
            throw std::runtime_error("自定义约束 '" + constraint.name + "' 的 state_bits 必须在 1..32");
        }
        for (const ConstraintExtension& existing : constraints_) {
            if (existing.name == constraint.name) {
                throw std::runtime_error("重复的自定义约束名称: " + constraint.name);
            }
        }
        constraints_.push_back(std::move(constraint));
    }

    const std::vector<UnaryOperationExtension>& unary_operations() const { return unary_; }
    const std::vector<BinaryOperationExtension>& binary_operations() const { return binary_; }
    const std::vector<ConstraintExtension>& constraints() const { return constraints_; }

private:
    static void validate_operation(const std::string& name, std::uint16_t cost, bool has_evaluator) {
        if (name.empty()) throw std::runtime_error("自定义运算名称不能为空");
        if (cost == 0) throw std::runtime_error("自定义运算 '" + name + "' 的默认成本必须大于 0");
        if (!has_evaluator) throw std::runtime_error("自定义运算 '" + name + "' 缺少 evaluate 回调");
    }

    static void validate_aliases(const std::string& name, const std::vector<std::string>& aliases) {
        for (std::size_t index = 0; index < aliases.size(); ++index) {
            const std::string& alias = aliases[index];
            if (alias.empty()) throw std::runtime_error("自定义运算 '" + name + "' 含有空别名");
            if (alias == name) throw std::runtime_error("自定义运算 '" + name + "' 的别名与名称重复");
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (aliases[previous] == alias) {
                    throw std::runtime_error("自定义运算 '" + name + "' 含有重复别名: " + alias);
                }
            }
        }
    }

    void ensure_name_available(std::string_view name) const {
        const auto matches = [&](const auto& operation) {
            if (operation.name == name) return true;
            for (const std::string& alias : operation.aliases) {
                if (alias == name) return true;
            }
            return false;
        };
        for (const auto& operation : unary_) {
            if (matches(operation)) throw std::runtime_error("重复的自定义运算名称或别名: " + std::string(name));
        }
        for (const auto& operation : binary_) {
            if (matches(operation)) throw std::runtime_error("重复的自定义运算名称或别名: " + std::string(name));
        }
    }

    std::vector<UnaryOperationExtension> unary_;
    std::vector<BinaryOperationExtension> binary_;
    std::vector<ConstraintExtension> constraints_;
};

}  // namespace fates
