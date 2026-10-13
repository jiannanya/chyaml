# Performance and optimization history / 性能与优化记录

[English](#english) | [中文](#中文)

[English README](README.md) | [中文 README](README_CN.md)

## English

### Performance snapshot

Measured on 2026-08-24 using an AMD Ryzen 9 9950X, Windows x64, MSVC 19.44
Release, and a 4,627,797-byte input containing 50,000 records. Each parser runs
in a separate process. Values are medians of 15 alternating paired runs with
25 steady-state parses per run; parser capacity is reused.

| Mode | Parse/materialize | Observed private-memory delta | Units |
|---|---:|---:|---:|
| chyaml portable 8-byte event tape | about 391 MB/s | about 6.18 MB | 750,009 events |
| rapidyaml 0.16.0 arena tree | about 184 MB/s | about 81.49 MB | 450,003 nodes |

On this workload, the chyaml tape is about 2.13x faster and uses about 13.18x less
incremental memory. A Release size probe linked to the full chyaml DOM API was
about 110 KB versus about 127 KB for the comparison tree API. These structures
serve different access patterns, so the numbers describe this workload and do
not imply identical semantics or universal performance.

The original workload above remains the built-in single-workload benchmark. A
second suite covers eight input shapes. The following results use the same
machine and toolchain, with 9 alternating paired runs per scenario and 15
steady-state parses per process sample. Every sample starts a fresh process;
both executables compile the same shared input generators.

| Workload | Input MiB | chyaml MB/s | rapidyaml MB/s | Speed ratio | chyaml memory MiB | rapidyaml memory MiB | Memory ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mixed records | 4.41 | 396.1 | 184.9 | 2.14x | 5.90 | 77.15 | 13.07x |
| Flat mapping | 4.77 | 682.7 | 316.1 | 2.16x | 6.37 | 42.01 | 6.59x |
| Scalar sequence | 6.82 | 882.6 | 458.5 | 1.92x | 9.11 | 44.05 | 4.84x |
| Nested mappings | 3.27 | 316.4 | 272.8 | 1.16x | 4.38 | 39.92 | 9.12x |
| Flow sequences | 5.14 | 310.0 | 152.7 | 2.03x | 10.31 | 150.01 | 14.55x |
| Quoted strings | 6.50 | 1436.1 | 733.2 | 1.96x | 8.69 | 25.71 | 2.96x |
| Sparse values/comments | 7.81 | 638.9 | 314.3 | 2.03x | 10.44 | 81.12 | 7.77x |
| Long scalars | 4.79 | 1549.0 | 772.6 | 2.00x | 6.40 | 10.46 | 1.63x |

Across the whole matrix, byte-weighted aggregate throughput is 599.2 MB/s for
chyaml and 312.2 MB/s for rapidyaml, a 1.92x ratio. The per-workload speed-ratio
geometric mean is 1.90x; the incremental-memory-ratio geometric mean is 6.16x
in chyaml's favor. The matrix intentionally reports the weakest result too:
nested mappings show a 1.16x speed ratio.

These performance scenarios target the portable 8-byte fast event tape. Full
YAML grammar coverage is tested separately by the 402-case conformance suite;
small heterogeneous conformance fixtures are not presented as throughput
measurements. Memory is the observed private-memory increase from the
post-input baseline through the warm parse, not a process-wide peak.

Reproduce the built-in test:

```sh
build/release/chyaml_benchmark events 50000 20
```

Optional comparison targets require a rapidyaml 0.16.0 source checkout only
for the benchmark executables; it is never linked into `chyaml`:

```sh
cmake -S . -B build/compare \
  -DCHYAML_BUILD_COMPARISON=ON \
  -DCHYAML_RAPIDYAML_SOURCE_DIR=/path/to/rapidyaml \
  -DCHYAML_OPTIMIZE_FOR=SPEED
cmake --build build/compare --config Release -j
build/compare/Release/chyaml_compare 50000 25
build/compare/Release/rapidyaml_compare 50000 25
```

The numeric form above is retained for the original single workload. List and
run an individual added scenario with:

```sh
build/compare/Release/chyaml_compare --list
build/compare/Release/chyaml_compare nested_maps 0 25
build/compare/Release/rapidyaml_compare nested_maps 0 25
```

Run the isolated paired suite and print both the per-workload table and
aggregate statistics with:

```sh
python tests/run_comparison.py \
  --chyaml build/compare/Release/chyaml_compare.exe \
  --rapidyaml build/compare/Release/rapidyaml_compare.exe \
  --runs 9 --iterations 15
```

When Python 3 is found during configuration, the
`chyaml_comparison_suite` build target runs the same driver with its shorter
7-run default.

### Optimization matrix

`chyaml_bench_suite` sweeps eight input shapes across the DOM and event APIs,
separates parse from emit timing, reports retained memory per scenario in a
fresh process, and measures concurrent node access. `tests/run_bench_matrix.py`
runs every mode for one or two binaries and prints the ratios:

```sh
build/release/chyaml_bench_suite --mode dom --profile fast --csv
build/release/chyaml_bench_suite --mode parallel --threads 8
python tests/run_bench_matrix.py \
  --a build/baseline/chyaml_bench_suite \
  --b build/release/chyaml_bench_suite --scale 1 --repeats 3 --iterations 3
```

The comparison below used an Apple M4 Max (macOS 26.1, Apple clang 17,
Release, `CHYAML_OPTIMIZE_FOR=SPEED`), full-size inputs, and the previous
revision as the baseline:

| Workload family | Parse speedup | Retained-memory reduction |
|---|---:|---:|
| DOM, portable 8-byte event tape | 1.03x - 1.31x | 1.20x - 1.22x |
| DOM, complete path | 1.33x - 7.22x | 1.19x - 1.86x |
| Event tape | 1.03x - 1.20x | about 1.00x |

Over the 32 timing cells the geometric-mean speedup is 1.46x; over the 24
memory cells the geometric-mean reduction is 1.18x. Emission throughput on the
same inputs improved by about 1.6x - 1.8x. A 3.95 MB stream of 20,000 plain
documents parses about 2.4x faster with `--mode parallel` and 8 workers, and
its sequential path also benefits from the per-document arena layout.

### Incremental optimization, 2026-10-13

This round uses `bdf05a6` as its baseline, preserves the public API and portable
C++20 design, and adds no platform-specific instructions:

- Complete-path event records shrink from 80 to 8 bytes on 64-bit platforms;
  that path still retains its DOM.
- Single-line quoted strings that need no decoding borrow the source. Quoted
  flow parsing reuses the line position and returns the closing offset directly,
  removing repeated scans from the first line and across the scalar.
- Wide irregular collections maintain child indices. Strict scalar-key checking
  uses hashing and now applies to fast parse entry points as well.
- Fast event traversal locates each newline once. Emission reserves space for
  the current document and reuses initialized string storage.

Measurements used an Apple M4 Max, macOS 26.1, Apple clang 17, ARM64 Release /
SPEED / IPO. Eight full-size inputs across four parse modes produce 32 timing
cells. Three runs alternate binary order; each process takes the best of five
measurements of five parses, then the driver reports the median across runs.
The [complete data](tests/performance/2026-10-13-matrix.csv) includes every result:

| Metric | Result for this round |
|---|---:|
| Geometric mean over 32 parse timing cells | 1.11x |
| Fast DOM, eight scenarios | 1.01x–1.06x |
| Complete DOM, quoted strings | 2.84x |
| Complete events, quoted strings | 2.50x |
| Complete events, observed retained-memory reduction | 1.81x–2.45x; geometric mean 2.00x |

In the first pass, some parse scenarios regress by about 6%; ordinary
single-document emission ranges from 0.95x to 1.03x baseline throughput. The
follow-up below addresses these cases. First-pass DOM / event linked probe
files grow by approximately 0.7% / 0.8%, respectively.

The new `chyaml_bench_hotpaths` isolates complexity bottlenecks. The same driver
was linked against both library revisions. Results are medians of three
alternating runs, each using the median of five measurements. Inputs contain
5,000 entries or documents, with 50,000 random-access operations. Generation,
parsing before access, and parsing before emission are outside their respective
timed regions. [Focused data](tests/performance/2026-10-13-hotpaths.csv):

| Scenario | Before | After |
|---|---:|---:|
| Quoted strings in multiline flow collections | 7.233 ms | 0.513 ms |
| Strict duplicate-key checking | 44.351 ms | 0.499 ms |
| Irregular collection random access | 749.238 ms | 0.066 ms |
| Multi-document stream emission | 8.175 ms | 0.699 ms |

These focused inputs expose repeated traversal in the previous implementation;
they do not replace the general matrix above. Reproduce with:

```sh
python tests/run_bench_matrix.py \
  --a build/baseline/chyaml_bench_suite \
  --b build/release/chyaml_bench_suite \
  --scale 1 --repeats 5 --iterations 5 --threads 4 --runs 3 --csv matrix.csv
build/release/chyaml_bench_hotpaths 5000 5
```

### Follow-up on regression scenarios

The follow-up retains the compact event records, collection indices, strict-key
hashing, and buffer reuse while reducing additional work:

- Event consumption groups frequently used state and assigns the output once,
  avoiding a clear followed by individual field writes.
- Ordinary nodes skip tag/anchor parsing, and scalar creation reuses known
  locations. Flow scalars track line breaks during delimiter scanning instead
  of scanning again, preserving bare CR, CRLF, and trailing-whitespace behavior.
- Collection-index allocation and exception handling move out of the regular
  node-append path, retaining both regular strides and irregular indices.
- Plain-text validation and quoted emission share a four-byte table scan, with
  special bytes still handled by the existing rules. It uses standard C++20
  byte accesses with no alignment, endian, or platform-instruction dependency.

`chyaml.hpp` is unchanged. Added regression cases cover event field replacement,
empty/end/error state clearing, flow properties, line folding, and YAML/JSON
escapes at scanner boundaries.

The three-way comparison uses original `bdf05a6`, the first-pass snapshot, and
the candidate with identical ARM64 Release / SPEED / IPO settings. Each cell
rotates through all six binary orderings over six rounds. Each process takes
the best of five measurements of five iterations; the report takes the median
over rounds. Parsing and emission are timed separately. The driver saves
[per-cell results](tests/performance/2026-10-13-regressions.csv),
[all samples](tests/performance/2026-10-13-regressions-samples.csv), and
[binary hashes and parameters](tests/performance/2026-10-13-regressions-metadata.json).
[Source fingerprints](tests/performance/2026-10-13-provenance.json) identify the
three source snapshots:

```sh
python tests/check_performance.py \
  --baseline build/baseline/chyaml_bench_suite \
  --previous build/previous/chyaml_bench_suite \
  --candidate build/release/chyaml_bench_suite \
  --runs 6 --repeats 5 --iterations 5 --tolerance 0 \
  --csv regressions.csv
```

This command requires no regression in any measured median and returns nonzero
on failure. Omitting `--tolerance` allows 3% timing noise. Results apply to the
recorded workloads and machine, rather than every input or hardware platform.

The final six-round run passes with zero tolerance: all 96 comparisons of the
32 parse and 16 emission cells against both references improve.

| Metric | Versus original baseline | Versus first pass |
|---|---:|---:|
| Geometric mean parse speedup, 32 cells | 1.240x | 1.106x |
| Individual parse speedup range | 1.037x–3.067x | 1.025x–1.567x |
| Geometric mean emission speedup, 16 cells | 1.253x | 1.260x |
| Individual emission speedup range | 1.078x–2.002x | 1.065x–2.013x |

DOM / event linked probes each shrink by 80 bytes from the first pass, remaining
approximately 0.62% / 0.77% larger than the original baseline.

Focused workloads use the same 5,000-entry inputs and median of five
measurements per process, interleaving all three versions over six rounds.
[Preserved-gain results](tests/performance/2026-10-13-preserved.csv) and
[raw samples](tests/performance/2026-10-13-preserved-samples.csv):

| Scenario | Original baseline | First pass | Current |
|---|---:|---:|---:|
| Quoted flow parsing | 7.683 ms | 0.487 ms | 0.423 ms |
| Strict duplicate-key checking | 45.916 ms | 0.512 ms | 0.490 ms |
| Irregular random access, 50,000 operations | 845.947 ms | 0.0673 ms | 0.0677 ms |
| Multi-document emission | 8.341 ms | 0.764 ms | 0.747 ms |

The random-access difference from the first pass is about 0.6%, less than
0.5 microseconds per 50,000 accesses. Independent-process complete-event memory
probes use `--scale 2` and medians over three rounds: the eight workloads still
retain 1.81x–2.45x less memory than the original baseline, close to the first pass.

All five CTest checks pass on ARM64 Release, ARM64 ASan/UBSan Debug, and x86_64
SIZE builds, including 402/402 pinned conformance cases. Differential checks on
402 conformance inputs plus 1,696 generated inputs match the original baseline
for event fields, errors, and multiple output formats. Remote Windows/Linux CI
has not been run locally.

## 中文

### 性能快照

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

### 优化对照矩阵

`chyaml_bench_suite` 覆盖八种输入形态，分别测量 DOM 与事件 API，把解析与输出计时分离，在独立进程中报告每个场景的增量内存，并测量并发节点访问。`tests/run_bench_matrix.py` 可对一两个二进制运行全部模式并打印比值：

```sh
build/release/chyaml_bench_suite --mode dom --profile fast --csv
build/release/chyaml_bench_suite --mode parallel --threads 8
python tests/run_bench_matrix.py \
  --a build/baseline/chyaml_bench_suite \
  --b build/release/chyaml_bench_suite --scale 1 --repeats 3 --iterations 3
```

下表的对照环境为 Apple M4 Max（macOS 26.1、Apple clang 17、Release、`CHYAML_OPTIMIZE_FOR=SPEED`），输入为完整规模，基线为上一版本：

| 工作负载类别 | 解析加速 | 增量内存降低 |
|---|---:|---:|
| DOM，可移植 8 字节事件带 | 1.03x - 1.31x | 1.20x - 1.22x |
| DOM，完整路径 | 1.33x - 7.22x | 1.19x - 1.86x |
| 事件带 | 1.03x - 1.20x | 约 1.00x |

32 个计时单元的几何平均加速为 1.46x；24 个内存单元的几何平均降低为 1.18x。同一批输入的输出吞吐提升约 1.6x - 1.8x。3.95 MB、20,000 个纯量文档组成的流在 `--mode parallel`、8 个工作线程下约快 2.4 倍，其顺序路径也从按文档的 arena 布局中获益。

### 2026-10-13 增量优化

本轮以 `bdf05a6` 为基线，保持公共 API、C++20 和无平台专用指令的设计，改进了以下路径：

- 完整事件记录在 64 位平台从 80 字节降为 8 字节；完整解析仍保留 DOM。
- 完整路径直接借用无需解码的单行引号字符串；流式引号解析复用行位置，消除从首行重复扫描和结束位置重扫。
- 宽且不规则的集合维护子节点索引；严格重复标量键检查使用哈希，并修复快速解析绕过该选项的问题。
- 快速事件遍历每行只定位一次换行；输出缓冲区按当前文档大小预分配并复用已初始化的空间。

对照环境为 Apple M4 Max、macOS 26.1、Apple clang 17、ARM64 Release / SPEED / IPO。八种完整规模输入 × 四种解析模式共 32 个计时单元，进行三轮交替顺序对照；每个进程取五次测量的最短耗时，每次执行五遍，最终取三轮中位数。[完整数据](tests/performance/2026-10-13-matrix.csv)包含全部结果：

| 项目 | 本轮结果 |
|---|---:|
| 32 个解析单元的几何平均加速 | 1.11x |
| 快速 DOM，八种场景 | 1.01x–1.06x |
| 完整 DOM，引号字符串 | 2.84x |
| 完整事件，引号字符串 | 2.50x |
| 完整事件，观测增量内存降低 | 1.81x–2.45x，几何平均约 2.00x |

第一轮中，部分解析场景有约 6% 的回退，常规单文档输出约为基线的 0.95x–1.03x；后续处理见下节。该轮 DOM / 事件链接探针文件分别增加约 0.7% / 0.8%。

新增 `chyaml_bench_hotpaths` 单独检查算法复杂度；以下是同一份驱动器分别链接两版库后的三轮交替运行中位数，每轮内部取五次测量的中位数。输入包含 5,000 个条目或文档，随机访问测量执行 50,000 次；输入生成、访问前解析和输出前解析不在相应计时区间。[专项数据](tests/performance/2026-10-13-hotpaths.csv)：

| 场景 | 基线耗时 | 优化后耗时 |
|---|---:|---:|
| 多行流集合中的引号字符串解析 | 7.233 ms | 0.513 ms |
| 严格重复键检查 | 44.351 ms | 0.499 ms |
| 不规则集合随机访问 | 749.238 ms | 0.066 ms |
| 多文档流输出 | 8.175 ms | 0.699 ms |

这些专项输入刻意覆盖旧实现的重复遍历，不能替代上述通用矩阵。复现命令：

```sh
python tests/run_bench_matrix.py \
  --a build/baseline/chyaml_bench_suite \
  --b build/release/chyaml_bench_suite \
  --scale 1 --repeats 5 --iterations 5 --threads 4 --runs 3 --csv matrix.csv
build/release/chyaml_bench_hotpaths 5000 5
```

### 回退场景优化

后续优化保留第一轮的事件记录压缩、集合索引、严格键哈希检查与缓冲区复用，进一步减少以下开销：

- 事件消费集中存放常用状态，一次赋值生成输出，避免先清空再逐字段填写。
- 普通节点跳过标签和锚点解析；纯量创建复用已知行列位置，流式纯量在扫描分隔符时记录换行，避免重复扫描。裸 CR、CRLF 和尾部空白仍按原有规则处理。
- 将集合索引的分配和异常处理移出常规节点追加路径，同时保留规则步长及不规则索引。
- 普通文本和引号输出共用四字节查表扫描，特殊字符继续走原有规则。实现只使用标准 C++20 字节访问，不要求对齐、字节序或平台专用指令。

`chyaml.hpp` 未改动。回归用例新增事件输出字段覆盖、空解析器和结束/错误状态清理、流式属性、换行折叠以及扫描边界上的 YAML/JSON 转义检查。

三方对照同时使用原始 `bdf05a6`、第一轮快照和当前版本。相同 ARM64 Release / SPEED / IPO 配置下，每个场景轮换三个二进制的全部六种运行顺序，运行六轮，每轮内部取五次测量的最短耗时，每次五遍，最后取六轮中位数。解析计时与输出计时分离。驱动器保存[逐项结果](tests/performance/2026-10-13-regressions.csv)、[所有采样](tests/performance/2026-10-13-regressions-samples.csv)、[二进制哈希及运行参数](tests/performance/2026-10-13-regressions-metadata.json)和[源码指纹](tests/performance/2026-10-13-provenance.json)：

```sh
python tests/check_performance.py \
  --baseline build/baseline/chyaml_bench_suite \
  --previous build/previous/chyaml_bench_suite \
  --candidate build/release/chyaml_bench_suite \
  --runs 6 --repeats 5 --iterations 5 --tolerance 0 \
  --csv regressions.csv
```

该命令要求每项测量中位数均无回退，失败会返回非零状态；省略 `--tolerance` 时允许 3% 的计时波动。结果适用于记录的工作负载和本机环境，不代表所有硬件或输入。

最终六轮结果通过零回退检查：32 个解析项和 16 个输出项分别对两个参考版本的全部 96 项比较均提升。

| 指标 | 相对原始基线 | 相对第一轮 |
|---|---:|---:|
| 32 项解析几何平均加速 | 1.240x | 1.106x |
| 解析单项加速范围 | 1.037x–3.067x | 1.025x–1.567x |
| 16 项输出几何平均加速 | 1.253x | 1.260x |
| 输出单项加速范围 | 1.078x–2.002x | 1.065x–2.013x |

DOM / 事件链接探针比第一轮各减少 80 字节，相对原始基线分别增加约 0.62% / 0.77%。

专项工作负载按前述 5,000 条输入和每轮五次中位数测量，三版本交错运行六轮。[保留收益复测](tests/performance/2026-10-13-preserved.csv)与[原始采样](tests/performance/2026-10-13-preserved-samples.csv)：

| 场景 | 原始基线 | 第一轮 | 当前版本 |
|---|---:|---:|---:|
| 引号流集合解析 | 7.683 ms | 0.487 ms | 0.423 ms |
| 严格重复键检查 | 45.916 ms | 0.512 ms | 0.490 ms |
| 不规则集合随机访问，50,000 次 | 845.947 ms | 0.0673 ms | 0.0677 ms |
| 多文档流输出 | 8.341 ms | 0.764 ms | 0.747 ms |

随机访问与第一轮的差异约 0.6%，小于 0.5 微秒/50,000 次访问。完整事件的独立进程内存探针使用 `--scale 2`、三轮中位数，八种输入仍比原始基线降低 1.81x–2.45x，与第一轮接近。

本轮在 ARM64 Release、ARM64 ASan/UBSan Debug、x86_64 SIZE 上均通过五项 CTest；固定版官方规范用例 402/402 通过。另对 402 项规范输入和 1,696 项生成输入逐字段比较事件、错误及多种格式输出，与原始基线一致。Windows/Linux 的远程 CI 尚未执行。
