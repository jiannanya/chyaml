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

`buffered()` 可判断是否选择了直接引用输入的快速事件带。返回 false 时仍会通过完整路径支持全部语法；完整路径保留 DOM，并使用每条 8 字节的节点引用记录事件，在消费时展开为公共 `event`。

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

宽集合使用等距节点步长或显式子节点索引，使 `at()` / `pair_at()` 的随机访问保持常数时间。索引在解析和追加时维护，不在只读访问中创建，支持多个线程同时读取同一个不再修改的文档。

`parse_copy()` 拥有输入副本，`parse_file()` 读取文件。节点句柄不能比所属文档活得更久。设置 `allow_duplicate_keys = false` 时，所有解析入口都会检查重复标量键；该检查使用稳定的字符串偏移进行哈希，避免大映射上的反复遍历。

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

长多文档流可以并行解析：

```cpp
chyaml::parse_options options;
options.parallel_documents = true;  // 按需开启
options.max_worker_threads = 8;     // 0 表示使用硬件并发度

chyaml::stream_parser stream;
if (!stream.reset_borrowed(large_stream, options)) return false;
```

切分器只在所有文档边界都无需真正解析即可确定时才启用并行：流中不得出现跨行引号标量、块标量、流集合或指令。它把文档按至少 128 KiB 一组打包，使线程启动开销可以忽略；任一分块失败都会回退到顺序解析。因此文档顺序、内容、行号和错误位置在任何输入上都与顺序路径完全一致。

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

## 性能

基准结果、性能优化记录、原始测量数据和复现命令统一记录在 [PERFORMANCE.md](PERFORMANCE.md#中文)。

## SIMD 说明

chyaml 使用可移植纯标量 C++20，主动避免平台专用 SIMD/目标调优。rapidyaml 0.16.0 的 YAML 结构解析器也主要是可移植状态机；其捆绑的数值转换代码包含可选 SSE2/NEON 分支，但它们不是主要的 YAML 结构扫描器。

## 验证

功能测试覆盖 DOM 访问、文档流、事件位置、标签、锚点、别名、复杂键、块标量、流集合、构造、YAML 输出和 JSON 输出。

规范运行器通过固定版本官方 YAML Test Suite 的全部 402 项：接受 308 个有效输入，拒绝 94 个无效输入。更高的上游版本会新增用例：对当前版本（406 项）通过 405 项，唯一例外是上游新增的、期望接受 `%YAML 1.1 1.2` 的用例。

```sh
build/release/chyaml_conformance /path/to/yaml-test-suite
```

此前版本已使用 MSVC 19.44、GCC 12.2 和 Apple clang 17 验证。本次改动在 macOS 的 ARM64 Release、ARM64 ASan/UBSan Debug 和 x86_64 SIZE 构建上通过测试；x86_64 测试通过 Rosetta 执行。新增 CI 覆盖 Windows/MSVC、Linux/GCC、Linux/Clang 和 macOS/AppleClang，远程 CI 尚未在本地执行。

可通过 `-DCHYAML_TEST_SUITE_DIR=/path/to/yaml-test-suite` 将规范套件加入 CTest。CI 固定使用数据版本 `6ad3d2c62885d82fc349026c136ef560838fdf3d`，包含 402 项。

## CMake 选项

| 选项 | 默认值 | 作用 |
|---|---:|---|
| `CHYAML_OPTIMIZE_FOR` | `BALANCED` | `SPEED`、`BALANCED` 或 `SIZE` |
| `CHYAML_ENABLE_IPO` | `ON` | 工具链支持时启用 IPO/LTO |
| `CHYAML_BUILD_TESTS` | 仅顶层构建开启 | 构建功能测试 |
| `CHYAML_BUILD_BENCHMARKS` | 仅顶层构建开启 | 构建速度/内存基准 |
| `CHYAML_BUILD_CONFORMANCE` | 仅顶层构建开启 | 构建规范运行器 |
| `CHYAML_TEST_SUITE_DIR` | 空 | 可选规范数据目录；在测试和规范运行器开启时加入 CTest |
| `CHYAML_BUILD_SIZE_PROBE` | 仅顶层构建开启 | 构建链接空间探针 |
| `CHYAML_BUILD_COMPARISON` | `OFF` | 构建可选对比可执行文件 |

## 许可证

chyaml 使用 MIT 许可证。生产库完全自包含，不链接任何第三方 YAML 库。
