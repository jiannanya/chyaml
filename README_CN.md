# chyaml

`chyaml` 是一个面向嵌入式与资源受限程序的极简 C++20 YAML 子集解析器/写入器。整个库只有一个头文件 `chyaml.hpp`，没有第三方依赖，也不需要单独编译或链接。

设计目标：

- 快：单次线性扫描，解析期间不创建字符串副本；
- 省内存：每个解析节点固定占 20 字节，字符串直接引用输入；
- 小：单头文件、API 精简、未使用的模板写入代码不会实例化；
- 可控：默认最大嵌套深度为 32，可在编译期调整；
- 适合 MCU：写入器可直接写入固定缓冲区，全程不分配堆内存。

## 支持的 YAML 子集

支持：

- 基于空格缩进的块映射；
- 块序列，包括标量序列和对象序列；
- `- key: value` 紧凑对象写法；
- 单引号、双引号、常见双引号转义；
- 行尾注释与整行注释；
- `null`、布尔值、整数、浮点数和字符串读取；
- `---`、`...` 与 `%...` 行的跳过。

有意不支持：

- 锚点、别名、标签与复杂键；
- `|`、`>` 多行标量；
- flow collection 的结构化解析，`[a, b]` 和 `{a: b}` 仅作为普通标量保留；
- 多文档语义、隐式类型系统和完整 YAML 规范兼容；
- Tab 缩进、同一容器内混合映射项与序列项。

输入应由本库的写入器生成，或遵循上述简单格式。缩进宽度可以变化，但同级节点必须对齐；建议始终使用两个空格。

## 引入

```cpp
#include "chyaml.hpp"
```

编译时启用 C++20：

```sh
g++ -std=c++20 app.cpp
clang++ -std=c++20 app.cpp
```

MSVC 使用 `/std:c++20`。

## 快速读取

```cpp
#include "chyaml.hpp"
#include <string_view>

int main() {
    constexpr std::string_view text = R"(
device:
  name: "pump-a"
  rate: 120
  enabled: true
  pins:
    - 3
    - 5
)";

    chyaml::document doc;
    if (!doc.parse(text)) {
        const auto e = doc.error();
        // chyaml::message(e.code), e.line, e.column
        return 1;
    }

    const auto device = doc.root()["device"];
    const auto name = device["name"].value_or<std::string_view>({});
    const int rate = device["rate"].value_or(0);
    const bool enabled = device["enabled"].value_or(false);

    const auto pins = device["pins"];
    for (std::size_t i = 0; i < pins.size(); ++i) {
        const int pin = pins[i].value_or(-1);
        (void)pin;
    }
    (void)name;
    (void)rate;
    (void)enabled;
}
```

`operator[]("key")` 查找直属映射子节点，`operator[](index)` 取得直属子节点。查找失败会返回无效节点，可直接用于条件判断。

```cpp
if (auto port = doc.root()["network"]["port"]) {
    int value = 0;
    if (port.read(value)) {
        // 使用 value
    }
}
```

### 输入生命周期

`parse()` 是最快、最省内存的借用模式。节点中的字符串视图直接指向传入文本，因此文本必须在 `document` 及其节点使用期间保持有效且地址不变。

```cpp
std::string yaml = load_file();
chyaml::document doc;
doc.parse(yaml);            // doc 借用 yaml
```

如果需要让文档拥有输入，使用 `parse_copy()`：

```cpp
chyaml::document doc;
doc.parse_copy(load_file());
```

两种模式都不会修改输入。`doc.owns_source()` 可用于区分模式，`shrink_to_fit()` 可在解析完成后尽量回收多余容量。

### 节点类型与字符串

```cpp
auto n = doc.root()["items"];

n.type();        // chyaml::kind
n.is_mapping();
n.is_sequence();
n.is_scalar();
n.is_null();
n.key();         // std::string_view
n.scalar();      // 去掉外层引号，但尚未处理转义
```

读取到 `std::string_view` 不分配内存，也不展开转义。读取到 `std::string` 会展开写入器使用的常见转义：

```cpp
std::string decoded;
if (doc.root()["message"].read(decoded)) {
    // decoded 可包含换行、Tab 等字符
}
```

数值转换使用 `std::from_chars`，不会改动全局 locale，也不分配内存。转换必须消费整个标量，否则失败。

## 写入到字符串

字符串参数始终按字符串加双引号，数值和布尔重载按标量输出。需要自行提供原始标量时使用 `raw()`。

```cpp
chyaml::writer out;

out.begin_mapping();
out.value("name", "pump-a");
out.value("rate", 120);
out.value("enabled", true);

out.begin_sequence("pins");
out.value(3);
out.value(5);
out.end();

out.begin_sequence("peers");
out.begin_mapping();
out.value("host", "10.0.0.2");
out.value("port", 9000);
out.end();
out.end();

out.end();

if (!out.complete()) return 1;
const std::string_view yaml = out.view();
```

结果：

```yaml
name: "pump-a"
rate: 120
enabled: true
pins:
  - 3
  - 5
peers:
  -
    host: "10.0.0.2"
    port: 9000
```

每个 `begin_mapping()` / `begin_sequence()` 都必须有对应的 `end()`。根节点也需要显式开始和结束。任何调用次序错误或写入失败都会让 `ok()` 变为 `false`。

## 无堆分配写入

`buffer_sink` 将结果直接写到调用者提供的内存，不追加 `\0`。缓冲区不足时，当前及后续操作返回 `false`。

```cpp
char storage[256];
using fixed_writer = chyaml::basic_writer<chyaml::buffer_sink, 8>;
fixed_writer out{chyaml::buffer_sink(storage, sizeof storage)};

out.begin_mapping();
out.value("id", 7);
out.value("state", "ready");
out.end();

if (out.complete()) {
    std::string_view yaml = out.view();
    // 发送 yaml.data(), yaml.size()
}
```

模板参数 `8` 是这个写入器实例允许的最大容器深度。较小的值能进一步减小对象占用。

## 对象序列

对象序列中的每一项是一个没有键的映射容器：

```cpp
out.begin_sequence("sensors");

out.begin_mapping();
out.value("id", 1);
out.value("unit", "C");
out.end();

out.begin_mapping();
out.value("id", 2);
out.value("unit", "%");
out.end();

out.end();
```

读取时，序列的每个直属子节点代表一项：

```cpp
auto sensors = doc.root()["sensors"];
for (std::size_t i = 0; i < sensors.size(); ++i) {
    auto item = sensors[i];
    int id = item["id"].value_or(-1);
    (void)id;
}
```

## 资源与性能说明

- 解析复杂度为 `O(输入字节数)`，没有递归；
- 每个节点是 20 字节紧凑记录，键和值仍保留在原输入中；
- `document` 只持有一个节点向量；借用模式不保存输入副本；
- 按键和按下标访问是当前容器内的线性扫描，不建立哈希表；
- 解析时的深度栈位于调用栈上，默认最多 32 层；
- 写入数值使用 `std::to_chars`，不使用 iostream 或 locale；
- 固定缓冲区写入模式不进行动态内存分配。

小配置文件通常更受益于紧凑结构和零初始化成本。如果同一个大型映射需要反复随机查询，建议在应用层缓存所需节点，不要重复从根节点查找。

## 编译期配置

在包含头文件前定义最大解析/默认写入深度：

```cpp
#define CHYAML_MAX_DEPTH 16
#include "chyaml.hpp"
```

值必须大于零。降低它会减小解析临时栈和默认写入器对象；超过限制的输入会返回 `chyaml::error_code::depth_limit`。

## 错误处理

```cpp
chyaml::document doc;
if (!doc.parse(text)) {
    const chyaml::parse_error e = doc.error();
    std::string_view reason = chyaml::message(e.code);
    // e.line 和 e.column 从 1 开始；全局大小错误可能为 0
}
```

解析失败后，错误位置之前的节点可能仍保留在文档中，只应用于诊断，不应当作完整配置使用。重新调用 `parse()`、`parse_copy()` 或 `clear()` 即可复用对象。

## 建议

- 对可信配置优先使用 `parse()`；
- 已知节点数量时先调用 `reserve()`，可避免节点向量扩容；
- 配置生成端优先使用本库写入器，可确保落在受支持子集内；
- 对来自不可信来源的配置，在业务层继续检查字段范围、必填项和序列长度。
