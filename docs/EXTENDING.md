# 扩展 Fates

扩展接口位于 `src/extension_api.h`，默认配置和源码扩展位于 `src/user_extensions.h`。扩展会静态编译进程序，不需要插件 DLL 或运行时加载器。

## 选择扩展方式

按修改范围，可以使用以下入口：

1. 命令行参数：`--constant`、`--symbol-count`、`--symbol-order` 和运算成本覆盖；
2. `configure_user_extensions()`：固定默认值、常数和约束；
3. `register_user_extensions()`：注册一元运算、二元运算和约束；
4. 修改核心枚举和求值器：只适用于需要内置运算性能或核心语义的情况。

未在 `--ops` 中启用的扩展运算不会进入搜索。约束只有注册后才会参与候选状态转移。

## 默认值和常数

`configure_user_extensions()` 是模板函数，可以修改公开配置字段：

```cpp
template <typename ConfigType>
inline void configure_user_extensions(ConfigType& cfg) {
    cfg.custom_constants.push_back("G=0.915965594177219:2");
    cfg.symbol_count_specs.push_back("pi=4");
    cfg.required_symbol_order = {"1", "1", "4", "5", "1", "4"};
    cfg.max_integer = 25;
    cfg.beam = 6000;
}
```

命令行常数使用相同格式：

```powershell
.\fates.exe TARGET --constant 'G=0.915965594177219:2'
```

名称会成为 AST 叶子文本，也可以用于 `--symbol-count G=1`。数值必须有限，成本必须在 `1..65535`。

## 一元运算

最小的一元扩展需要名称、正成本和 `evaluate`：

```cpp
UnaryOperationExtension sigmoid;
sigmoid.name = "sigmoid";
sigmoid.aliases = {"logistic"};
sigmoid.default_cost = 2;
sigmoid.evaluate = [](double x, const ExtensionLimits&) -> std::optional<double> {
    const double z = std::exp(x < 0.0 ? x : -x);
    const double y = x < 0.0 ? z / (1.0 + z) : 1.0 / (1.0 + z);
    return std::isfinite(y) ? std::optional<double>{y} : std::nullopt;
};
extensions.add_unary(std::move(sigmoid));
```

回调字段：

| 字段 | 作用 | 是否必须 |
|---|---|---|
| `evaluate(x, limits)` | 计算运算结果；返回 `nullopt` 表示拒绝候选 | 是 |
| `derivative(x, y, dx)` | 方程模式传播导数 | 否 |
| `inverse(target, limits)` | 为逆向搜索提供实数原像 | 否 |
| `render_text(child)` | 自定义纯文本输出 | 否 |
| `render_latex(child)` | 自定义 LaTeX 输出 | 否 |
| `aliases` | `--ops` 接受的别名 | 否 |

`ExtensionLimits` 提供 `max_abs`、`max_exponent` 和 `max_trig_arg`。回调应在可能溢出或进入非法定义域前返回 `nullopt`。

## 二元运算

```cpp
BinaryOperationExtension average;
average.name = "avg";
average.default_cost = 1;
average.commutative = true;
average.evaluate = [](double a, double b, const ExtensionLimits&) {
    const double y = std::midpoint(a, b);
    return std::isfinite(y) ? std::optional<double>{y} : std::nullopt;
};
average.derivative = [](double, double, double, double da, double db) {
    return (da + db) / 2.0;
};
average.desired_left = [](double right, double target) {
    return std::optional<double>{2.0 * target - right};
};
average.desired_right = [](double left, double target) {
    return std::optional<double>{2.0 * target - left};
};
extensions.add_binary(std::move(average));
```

`desired_left` 和 `desired_right` 为双向、深层和遗传搜索提供目标值反推。省略它们不影响普通分层求值，但会减少目标导向候选。只有数学上满足交换律时才设置 `commutative = true`。

## 结构约束

每个 AST 节点有 32 位扩展约束状态。多个 `ConstraintExtension` 的 `state_bits` 总和不能超过 32。

```cpp
ConstraintExtension rule;
rule.name = "example";
rule.state_bits = 2;
rule.atom = [](const ExtensionAtomContext& atom)
    -> std::optional<ConstraintExtension::State> {
    return atom.symbol == "pi" ? 1U : 0U;
};
rule.unary = [](auto state, const ExtensionUnaryContext& context)
    -> std::optional<ConstraintExtension::State> {
    if (context.operation == "gamma" && state != 0) return std::nullopt;
    return state;
};
rule.binary = [](auto left, auto right, const ExtensionBinaryContext&)
    -> std::optional<ConstraintExtension::State> {
    return std::min(3U, left + right);
};
rule.can_finish = [](auto state, unsigned current_cost, unsigned max_cost) {
    return state != 0 || current_cost < max_cost;
};
rule.satisfied = [](auto state) { return state == 1; };
extensions.add_constraint(std::move(rule));
```

- `atom`、`unary`、`binary` 返回 `nullopt` 时，候选会被丢弃；
- `can_finish` 可以根据剩余成本排除不可行候选；
- `satisfied` 决定候选是否能进入最终结果；
- 没有提供的回调使用默认状态转移。

约束状态也参与数值去重。仅需要限制符号次数或叶子顺序时，优先使用 `--symbol-count` 和 `--symbol-order`。

## 并发和性能要求

- 回调会从多个工作线程并发调用，必须可重入；
- 不要在回调中修改未加锁的全局状态；
- 回调捕获的数据必须在进程结束前保持有效；
- `evaluate` 位于候选生成路径，避免 I/O、锁、动态分配和异常；
- `inverse` 返回的分支越多，逆向搜索的工作量越大；
- 未启用的扩展不进入候选循环；
- 一元、二元扩展各最多 192 项；名称和别名不能重复。

## 添加核心内置运算

如果运算需要核心级性能或语义，除了扩展注册外，还要同步修改：

1. `UnaryKind` 或 `BinaryKind` 枚举；
2. 名称、别名和默认成本；
3. 求值、定义域和导数；
4. 逆目标函数；
5. 纯文本和 LaTeX 渲染；
6. 恒等式、交换律和剪枝逻辑；
7. `--list-symbols --json` 元数据和自测。

原生枚举值必须小于 64，`64..255` 保留给源码扩展。新增原生运算默认不要加入 `Config::ops`，除非这是有意的兼容性改变。

## 前端目录发现

Web 服务启动时执行：

```powershell
.\fates.exe --list-symbols --json
```

返回值包含常数、运算符、默认成本、默认启用状态、交换律和内置/扩展标记。网页据此生成选项列表。

## 验证

新增扩展后至少运行：

```powershell
.\fates.exe --list-symbols --json
.\fates.exe TARGET --ops '...,your_op' --dry-run --json
.\fates.exe --self-test
```

另外准备一个正常值、一个定义域边界、一个逆目标值和一个方程导数样例进行回归。性能变更应固定目标、参数、随机种子和线程数，并比较结果及 `attempted`、`valid`、`kept` 等统计值。
