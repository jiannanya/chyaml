[English README](README.md)

# chyaml

`chyaml` 是一个编译型 C++20 [YAML 1.2.2](https://yaml.org/spec/1.2.2/) 解析器与写入器，首要优化目标是吞吐量和内存占用。它使用两条互补路径：

- 可移植的纯标量快路径：把常见 YAML 物化为每事件 8 字节的事件带，字符串直接引用输入；
- 完整 YAML 1.2.2 路径：支持指令、多文档、锚点、别名、标签、复杂键、块标量、流式集合、注释及其他标准语法。

默认构建不会添加 SIMD intrinsic、`-march=native`、`/arch:*` 或函数目标属性。`CHYAML_PORTABLE=ON` 还会关闭完整解析依赖中的可选 CPU 专用目标。

## 环境要求

- 支持 C++20 的编译器；
- CMake 3.21 或更高版本；
- 用于编译完整解析核心的 C17 编译器。

默认构建会获取固定版本的 [libfyaml 0.9.6](https://github.com/pantoniou/libfyaml/releases/tag/v0.9.6)。应用只需包含 `chyaml.hpp`；依赖头文件不会泄漏到公开 API。

## 构建与链接

```sh
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHYAML_OPTIMIZE_FOR=SPEED \
  -DCHYAML_PORTABLE=ON
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

使用多配置生成器时，请给构建和测试命令增加 `--config Release`。

作为子目录使用：

```cmake
add_subdirectory(path/to/chyaml)
target_link_libraries(my_app PRIVATE chyaml::chyaml)
target_compile_features(my_app PRIVATE cxx_std_20)
```

通过 `add_subdirectory()` 引入时，测试与基准默认不构建。

## 快速事件解析

对于配置型 YAML，使用 `event_parser` 可获得最高吞吐量和最低保留内存：

```cpp
#include "chyaml.hpp"

#include <string_view>

bool consume(std::string_view yaml) {
    chyaml::parse_options options;
    options.profile = chyaml::parse_profile::fast;

    chyaml::event_parser parser;
    if (!parser.reset_borrowed(yaml, options)) return false;

    chyaml::event event;
    while (parser.next(event) == chyaml::event_status::event) {
        if (event.type == chyaml::event_type::scalar) {
            // 在下一次 next() 调用前使用 event.value。
        }
    }
    return !parser.error();
}
```

当前可移植快路径接受包含以下内容的单个文档：

- 块映射和块序列；
- `- id: 7` 形式的紧凑序列映射；
- 普通标量，以及不含转义的简单双引号标量；
- `[1, 2, 3]` 形式的标量流式序列；
- 空行、注释，以及可选的 `---` / `...` 标记。

这是一个优化档位，不是缩减后的公开语法。快路径不支持的语法会自动回退到完整事件解析器。`preserve_comments=true`、`resolve_aliases=true`、文件输入和 `parse_profile::compact` 会直接使用完整路径。诊断或基准程序如需知道实际选择了哪条路径，可在 reset 后调用 `buffered()`。

快速事件带为每个事件保存两个 32 位字。标量文本留在调用方缓冲区中；消费事件时，通过一次单调扫描恢复行列位置。复用同一个 `event_parser` 还会复用事件带容量。

### 输入生命周期

`reset_borrowed()` 不拥有输入。解析和事件消费结束前，源数据必须保持存活且地址稳定。解析器需要拥有输入时请使用 `reset_copy()`。事件中的字符串视图只保证有效到下一次 `next()` 调用或 `clear()`。

完整事件路径可能在 `next()` 期间才报告语法错误，因此始终要检查最终状态和 `error()`。

## 最低内存的完整流式解析

`parse_profile::compact` 会关闭解析器缓冲与加速器。需要完整 YAML 支持和有界工作内存、但不追求最高吞吐量时，优先使用此模式：

```cpp
chyaml::parse_options options;
options.profile = chyaml::parse_profile::compact;

chyaml::event_parser parser;
if (!parser.reset_borrowed(yaml, options)) return false;

chyaml::event event;
while (parser.next(event) == chyaml::event_status::event) {
    // 逐个处理并丢弃事件。
}
return !parser.error();
```

## DOM 解析

需要随机查找、别名解析、编辑和输出时使用 `document`：

```cpp
constexpr std::string_view yaml = R"(
defaults: &base
  enabled: true
devices:
  - name: sensor-a
    settings: *base
)";

chyaml::document document;
if (!document.parse_borrowed(yaml)) {
    const auto& error = document.error();
    // error.message、error.line、error.column
    return 1;
}

const auto first = document.root()["devices"][0];
const auto name = first["name"].scalar();

bool enabled = false;
first["settings"].resolve_alias()["enabled"].as_bool(enabled);
```

主要节点操作包括：

- `find()` / `operator[]`：查找简单标量键；
- `find_yaml_key()`：查找复杂 YAML 键；
- `at()` 和 `pair_at()`：支持负数索引；
- `by_path()`：按斜杠分隔路径查找；
- `scalar()`、`tag()`、`anchor()` 和 `resolve_alias()`；
- `as_bool()`、`as_int64()`、`as_uint64()` 和 `as_double()`。

`parse_copy()` 拥有输入副本，`parse_file()` 从文件读取。节点是不拥有资源的句柄，不能比所属文档活得更久。

## 多文档

```cpp
chyaml::stream_parser stream;
if (!stream.reset_borrowed(yaml_stream)) return false;

chyaml::document document;
for (;;) {
    const auto status = stream.next(document);
    if (status == chyaml::stream_status::end) break;
    if (status == chyaml::stream_status::error) return false;
    // 使用 document，然后读取下一个文档。
}
```

## 写入 YAML 和 JSON

解析或构造出的文档可写入字符串：

```cpp
chyaml::emit_options options;
options.style = chyaml::emit_style::block;
options.indent = 2;
options.explicit_document_start = true;

std::string output;
if (!document.emit(output, options)) return false;
```

可用风格包括 `original`、`block`、`flow`、`flow_one_line`、`pretty`、`json`、`json_one_line` 和 `json_type_preserving`。写入器还可保留注释、排序映射键、控制文档标记，并通过 `emit_to_buffer()` 直接写入调用方缓冲区。

也可以不经过解析直接构造文档：

```cpp
chyaml::document document;
document.create();

auto root = document.make_mapping();
auto values = document.make_sequence();
values.append(document.make_scalar("10"));
values.append(document.make_scalar("20"));
root.append(document.make_scalar("values"), values);
document.set_root(root);
```

## 性能快照

以下结果测于 2026-08-24，环境为 AMD Ryzen 9 9950X、Windows x64、MSVC 19.44 Release。输入包含 50,000 条传感器记录、共 4,627,797 字节。对比吞吐取 9 组交替配对运行的中位数，每组执行 50 次稳态解析。解析器对象及已分配容量均会复用。每个实现运行于独立进程；私有内存基线不包含输入分配。

| 模式 | 解析/物化速度 | 观测增量内存 | 输出单元 |
|---|---:|---:|---:|
| chyaml 可移植快速事件带 | 约 330 MB/s | 约 6.18 MB（输入的 1.34 倍） | 750,009 个事件 |
| [rapidyaml 0.16.0](https://github.com/biojppm/rapidyaml/releases/tag/v0.16.0) arena 树，复用 parser/tree | 约 178 MB/s | 约 81.5 MB（输入的 17.61 倍） | 450,003 个节点 |
| chyaml 完整紧凑事件流 | 约 53 MB/s | 多轮观测为 0–12 KB | 750,009 个事件 |

在这个工作负载中，快速事件带的物化速度约为 rapidyaml arena 树的 1.86 倍，增量内存约小 13.2 倍。快速事件带的遍历速度另行测得约 5,700 万事件/秒。紧凑模式一行来自内置的 10 次迭代基准，其微小内存增量会随操作系统页记账粒度波动。

这里比较的是不同数据结构：事件带不是可随机访问的 DOM 树。结果只代表此配置型工作负载，不是对所有 YAML 文档、API、编译器或机器的普遍结论。超出快路径范围的输入会使用完整路径，性能也会不同。

复现内置测量：

```sh
build/release/chyaml_benchmark events 50000 10
build/release/chyaml_benchmark events-compact 50000 10
```

可选对比目标需要 rapidyaml 0.16.0 源码目录：

```sh
cmake -S . -B build/compare \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHYAML_BUILD_COMPARISON=ON \
  -DCHYAML_RAPIDYAML_SOURCE_DIR=/path/to/rapidyaml \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/compare -j
build/compare/chyaml_compare 50000 10
build/compare/rapidyaml_compare 50000 10
```

## SIMD 与目标指令优化

chyaml 快速事件扫描器使用普通标量 C++20，不包含 SSE、AVX、AVX-512、NEON、目标属性、运行时 CPU 分派或架构编译参数。默认 `CHYAML_PORTABLE=ON` 时，完整解析核心的可选 SSE2、SSE4.1、AVX2、AVX-512 和 NEON 目标也会关闭。编译器在正常优化过程中仍可自由使用平台基线指令。

在 rapidyaml 0.16.0 中，主要 YAML 结构解析器同样是可移植状态机，而不是显式 SIMD 解析器。它捆绑的 c4core 数值转换头文件含有可选 SSE2/NEON 路径，但这不是 YAML 结构扫描的主算法。其性能主要来自原地/arena 字符串、扁平索引树、解析器复用以及非递归状态机。

## CMake 选项

| 选项 | 默认值 | 作用 |
|---|---:|---|
| `CHYAML_PORTABLE` | `ON` | 关闭依赖中的可选 CPU 专用目标 |
| `CHYAML_OPTIMIZE_FOR` | `BALANCED` | `SPEED`、`BALANCED` 或 `SIZE` |
| `CHYAML_ENABLE_IPO` | `ON` | 在工具链支持时启用 IPO/LTO |
| `CHYAML_USE_SYSTEM_LIBFYAML` | `OFF` | 使用已安装的 libfyaml 0.9.6 包 |
| `CHYAML_FAST_EVENTS_ONLY` | `OFF` | 只从 `event_parser` 中移除完整回退 |
| `CHYAML_BUILD_TESTS` | 仅顶层构建开启 | 构建功能测试 |
| `CHYAML_BUILD_BENCHMARKS` | 仅顶层构建开启 | 构建速度/内存基准 |
| `CHYAML_BUILD_CONFORMANCE` | 仅顶层构建开启 | 构建 YAML Test Suite 运行器 |
| `CHYAML_BUILD_COMPARISON` | `OFF` | 构建可选 rapidyaml 对比 |

`CHYAML_FAST_EVENTS_ONLY=ON` 是专用部署选项。它会减少事件解析器链接代码，但超出快路径范围的输入将直接报错，不再回退。DOM 与多文档 API 仍使用完整核心。

## 验证

当前测试矩阵覆盖 Release 模式的 MSVC 19.44 和 GCC 12.2。两者均通过功能测试以及固定版本官方 [YAML Test Suite](https://github.com/yaml/yaml-test-suite) 的全部 402 项：接受 308 个有效输入，拒绝 94 个无效输入。

克隆测试数据目录后运行规范测试：

```sh
build/release/chyaml_conformance /path/to/yaml-test-suite
```

## 许可证

chyaml 使用 MIT 许可证。完整解析依赖的声明见 `THIRD_PARTY_NOTICES.md`。rapidyaml 只用于可选的本地对比目标，不属于 chyaml 生产库。
