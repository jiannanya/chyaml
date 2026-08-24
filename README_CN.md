[English README](README.md)

# chyaml

`chyaml` 是一个自包含的 C++20 YAML 1.2.2 解析器与写入器。生产库不依赖任何第三方 YAML 库，优化优先级依次为速度、内存、链接后空间。

同一套 API 内部组合了两条路径：

- 面向常见配置 YAML 的可移植纯标量快路径，使用每事件 8 字节的事件带和借用字符串视图；
- 完整路径，支持 YAML 1.2.2 指令、文档流、块/流集合、复杂键、标签、锚点、别名、引号与块标量、Unicode 转义、注释和语法校验。

快路径无法处理的输入会自动使用完整路径。chyaml 不使用 SSE、AVX、NEON、目标属性、运行时 CPU 分派、`-march=native` 或 `/arch:*` 选项。

## 环境要求与构建

- 支持 C++20 的编译器
- CMake 3.21 或更高版本

```sh
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

多配置生成器需要给构建和测试命令添加 `--config Release`。嵌入项目：

```cmake
add_subdirectory(path/to/chyaml)
target_link_libraries(my_app PRIVATE chyaml::chyaml)
```

只需要 `chyaml.cpp` 和 `chyaml.hpp`。通过 `add_subdirectory()` 引入时，测试和基准默认关闭。

## 快速事件解析

顺序处理、吞吐和保留内存最重要时使用 `event_parser`：

```cpp
#include "chyaml.hpp"

bool consume(std::string_view yaml) {
    chyaml::event_parser parser;
    if (!parser.reset_borrowed(yaml)) return false;

    chyaml::event event;
    for (;;) {
        const auto status = parser.next(event);
        if (status == chyaml::event_status::end) return true;
        if (status == chyaml::event_status::error) return false;
        if (event.type == chyaml::event_type::scalar) {
            // 使用 event.value。
        }
    }
}
```

快速档位识别单文档块映射和序列、紧凑序列映射、普通标量、简单双引号标量、标量流式序列、注释、空行和文档标记。每个事件保存两个 32 位字；标量字节保留在输入中，遍历时通过一次单调扫描恢复行列位置。复用 `event_parser` 会复用事件带容量。

`buffered()` 可判断是否选择了 8 字节事件带。返回 false 时仍会通过原生完整路径支持全部语法。

`reset_borrowed()` 不拥有输入；解析与事件消费结束前，输入必须存活且地址稳定。需要解析器拥有源数据时使用 `reset_copy()`。

## DOM 解析

随机查找、编辑、别名解析和输出使用 `document`：

```cpp
constexpr std::string_view source = R"(
defaults: &base
  enabled: true
devices:
  - name: sensor-a
    settings: *base
)";

chyaml::document doc;
if (!doc.parse_borrowed(source)) {
    const auto& error = doc.error();
    return 1;
}

const auto device = doc.root()["devices"][0];
const auto name = device["name"].scalar();
bool enabled = false;
device["settings"].resolve_alias()["enabled"].as_bool(enabled);
```

DOM 在 arena 中保存稳定节点地址，字符串使用压缩的源/池引用，未修改标量直接借用输入；快路径会精确预分配节点数。主要 API 包括 `find()`、`find_yaml_key()`、`at()`、`pair_at()`、`by_path()`、`tag()`、`anchor()`、`resolve_alias()` 和标量类型转换。

`parse_copy()` 拥有输入副本，`parse_file()` 读取文件。节点句柄不能比所属文档活得更久。

## 文档流

```cpp
chyaml::stream_parser stream;
if (!stream.reset_borrowed(source)) return false;

chyaml::document doc;
for (;;) {
    const auto status = stream.next(doc);
    if (status == chyaml::stream_status::end) break;
    if (status == chyaml::stream_status::error) return false;
    // 处理一个文档。
}
```

## 写入 YAML 与 JSON

```cpp
chyaml::emit_options options;
options.style = chyaml::emit_style::block;
options.indent = 2;
options.explicit_document_start = true;

std::string output;
if (!doc.emit(output, options)) return false;
```

可用风格包括 `original`、`block`、`flow`、`flow_one_line`、`pretty`、`json`、`json_one_line` 和 `json_type_preserving`。`emit_to_buffer()` 可写入调用方缓冲区。也可以使用 `create()`、`make_scalar()`、`make_sequence()`、`make_mapping()`、`append()` 和 `set_root()` 直接构造文档。

## 性能快照

测量日期为 2026-08-24，环境是 AMD Ryzen 9 9950X、Windows x64、MSVC 19.44 Release；输入为 4,627,797 字节、包含 50,000 条记录。每个解析器运行在独立进程中；结果取 15 组交替配对运行的中位数，每组执行 25 次稳态解析并复用解析器容量。

| 模式 | 解析/物化速度 | 观测私有增量内存 | 单元 |
|---|---:|---:|---:|
| chyaml 可移植 8 字节事件带 | 约 391 MB/s | 约 6.18 MB | 750,009 个事件 |
| rapidyaml 0.16.0 arena 树 | 约 184 MB/s | 约 81.49 MB | 450,003 个节点 |

在该工作负载上，chyaml 事件带约快 2.13 倍，增量内存约小 13.18 倍。Release 链接空间探针中，完整 chyaml DOM API 约 110 KB，对比树 API 约 127 KB。两者的数据结构和访问模式不同，因此这些结果描述的是该工作负载，不代表语义完全相同或所有输入上的普遍性能。

上述原始负载继续作为内置单一工作负载基准保留。新增的第二套测试覆盖 8 种输入形态。下表仍使用相同机器和工具链；每个场景执行 9 组交替配对运行，每个进程样本进行 15 次稳态解析。每个样本都启动新进程，两个可执行文件编译并使用同一份共享输入生成器。

| 工作负载 | 输入 MiB | chyaml MB/s | rapidyaml MB/s | 速度比 | chyaml 内存 MiB | rapidyaml 内存 MiB | 内存比 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 混合记录 | 4.41 | 396.1 | 184.9 | 2.14x | 5.90 | 77.15 | 13.07x |
| 扁平映射 | 4.77 | 682.7 | 316.1 | 2.16x | 6.37 | 42.01 | 6.59x |
| 标量序列 | 6.82 | 882.6 | 458.5 | 1.92x | 9.11 | 44.05 | 4.84x |
| 嵌套映射 | 3.27 | 316.4 | 272.8 | 1.16x | 4.38 | 39.92 | 9.12x |
| 流式序列 | 5.14 | 310.0 | 152.7 | 2.03x | 10.31 | 150.01 | 14.55x |
| 引号字符串 | 6.50 | 1436.1 | 733.2 | 1.96x | 8.69 | 25.71 | 2.96x |
| 稀疏值/注释 | 7.81 | 638.9 | 314.3 | 2.03x | 10.44 | 81.12 | 7.77x |
| 长标量 | 4.79 | 1549.0 | 772.6 | 2.00x | 6.40 | 10.46 | 1.63x |

整个矩阵按输入字节加权后的综合吞吐为 chyaml 599.2 MB/s、rapidyaml 312.2 MB/s，速度比 1.92 倍。各工作负载速度比的几何平均值为 1.90 倍，增量内存比的几何平均值为 chyaml 小 6.16 倍。矩阵也明确保留最弱结果：嵌套映射的速度比为 1.16 倍。

这些性能场景针对可移植 8 字节快速事件带。完整 YAML 语法由独立的 402 项规范套件验证；体积很小且差异很大的规范样例不被包装成吞吐数据。内存值是从输入生成完成后的基线到预热解析后的观测私有内存增量，不是进程全生命周期峰值。

复现内置基准：

```sh
build/release/chyaml_benchmark events 50000 20
```

可选对比目标仅在基准可执行文件中需要 rapidyaml 0.16.0 源码，不会链接到 `chyaml`：

```sh
cmake -S . -B build/compare \
  -DCHYAML_BUILD_COMPARISON=ON \
  -DCHYAML_RAPIDYAML_SOURCE_DIR=/path/to/rapidyaml \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/compare --config Release -j
build/compare/Release/chyaml_compare 50000 25
build/compare/Release/rapidyaml_compare 50000 25
```

以上数字参数形式继续用于原来的单一工作负载。列出并单独运行新增场景：

```sh
build/compare/Release/chyaml_compare --list
build/compare/Release/chyaml_compare nested_maps 0 25
build/compare/Release/rapidyaml_compare nested_maps 0 25
```

用独立进程运行交替配对综合套件，并打印逐场景表格与综合统计：

```sh
python tests/run_comparison.py \
  --chyaml build/compare/Release/chyaml_compare.exe \
  --rapidyaml build/compare/Release/rapidyaml_compare.exe \
  --runs 9 --iterations 15
```

配置阶段找到 Python 3 时，还会提供 `chyaml_comparison_suite` 构建目标，以较短的默认 7 组运行执行同一驱动器。

## SIMD 说明

chyaml 使用可移植纯标量 C++20，主动避免平台专用 SIMD/目标调优。rapidyaml 0.16.0 的 YAML 结构解析器也主要是可移植状态机；其捆绑的数值转换代码包含可选 SSE2/NEON 分支，但它们不是主要的 YAML 结构扫描器。

## 验证

功能测试覆盖 DOM 访问、文档流、事件位置、标签、锚点、别名、复杂键、块标量、流集合、构造、YAML 输出和 JSON 输出。

规范运行器通过固定版本官方 YAML Test Suite 的全部 402 项：接受 308 个有效输入，拒绝 94 个无效输入。

```sh
build/release/chyaml_conformance /path/to/yaml-test-suite
```

当前版本已使用 MSVC 19.44 和 GCC 12.2 验证。

## CMake 选项

| 选项 | 默认值 | 作用 |
|---|---:|---|
| `CHYAML_OPTIMIZE_FOR` | `BALANCED` | `SPEED`、`BALANCED` 或 `SIZE` |
| `CHYAML_ENABLE_IPO` | `ON` | 工具链支持时启用 IPO/LTO |
| `CHYAML_BUILD_TESTS` | 仅顶层构建开启 | 构建功能测试 |
| `CHYAML_BUILD_BENCHMARKS` | 仅顶层构建开启 | 构建速度/内存基准 |
| `CHYAML_BUILD_CONFORMANCE` | 仅顶层构建开启 | 构建规范运行器 |
| `CHYAML_BUILD_SIZE_PROBE` | 仅顶层构建开启 | 构建链接空间探针 |
| `CHYAML_BUILD_COMPARISON` | `OFF` | 构建可选对比可执行文件 |

## 许可证

chyaml 使用 MIT 许可证。生产库完全自包含，不链接任何第三方 YAML 库。
