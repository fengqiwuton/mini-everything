# 项目函数与文件职责说明

本文覆盖仓库维护的源码、测试、脚本、构建配置和现有文档。函数前的中文注释说明用途、关键约定及异常或资源释放行为；构造/析构函数、被禁用的复制接口和局部辅助函数也有说明。

## 推荐阅读顺序

先读 [README](../README.md) 了解构建与使用，再读 [公开接口](../include/mini/index.hpp)、[命令行入口](../src/main.cpp) 和 [扫描与查询实现](../src/index.cpp)。随后按需要阅读路径、数据库、Windows 文件系统辅助模块及测试。

扫描调用关系大致为：`main → run → scan → 路径检查 → Database/Transaction → 元数据枚举 → 节点插入 → 提交`。查询调用关系为：`main → run → search → 只读 Database → 参数化 SQL → Node 结果`。

## 源码与公开接口

| 文件 | 大致作用 |
| --- | --- |
| [include/mini/index.hpp](../include/mini/index.hpp) | 核心公开接口与数据结构。定义 `Node`、扫描数量/耗时/缓存统计、`ScanOptions`、`SearchOptions`，声明 `scan` 与 `search`。调用者传入原生路径，数据库中的文本使用 UTF-8。 |
| [src/main.cpp](../src/main.cpp) | 命令行入口。处理 Windows 宽字符参数、帮助与版本输出、参数范围及重复选项，调度扫描/查询，格式化 TSV 和诊断，并把异常转换为退出码。 |
| [src/index.cpp](../src/index.cpp) | 核心业务实现。校验或创建 schema 1，管理根目录和重叠检查，以迭代 DFS 扫描，在一个事务中清除并替换目标根节点，累计诊断信息；实现带字面转义、扩展名过滤、稳定排序和条数限制的查询。 |
| [src/paths.hpp](../src/paths.hpp) | 跨平台路径辅助函数。负责 UTF-8 与原生路径转换、绝对路径规范化与别名解析、路径分量比较、祖先范围判断、链接识别及 ASCII 扩展名归一化。Windows 分支使用原生 API 支持扩展路径。 |
| [src/sqlite.hpp](../src/sqlite.hpp) | SQLite 的 RAII 包装。`Database` 管理连接、连接缓存与错误；`Statement` 管理预编译语句、参数绑定和结果读取；`Transaction` 开启/提交事务并在未提交时尽力回滚。 |
| [src/windows_fs.hpp](../src/windows_fs.hpp) | Windows 原生文件系统辅助模块。`FileHandle` 管理身份查询句柄；`IndexFileGuard` 检查索引硬链接及链接数变化；`DirectoryCursor` 逐项枚举目录并直接提供元数据；另提供本地盘判定与 FILETIME 转换。仅 Windows 编译该实现。 |

树结构由 `nodes.parent_id` 表示，完整路径同时保存在 `nodes.full_path`。磁盘负责保存文件，SQLite 负责保存可查询的索引；搜索不会再次扫描磁盘，文件变化需要重扫后才反映到结果中。

## 测试

| 文件 | 大致作用 |
| --- | --- |
| [tests/index_tests.cpp](../tests/index_tests.cpp) | C++ 核心集成测试与夹具。创建真实临时目录、文件和 SQLite 数据库，验证树关系、持久化、重扫、查询、范围检查、格式兼容、硬链接、回滚和缓存策略。Windows 分支另测扩展/长路径、卷根、UNC 别名及目录读取故障；入口根据名称执行单个用例。 |
| [tests/cli_test.cmake](../tests/cli_test.cmake) | 基础命令行集成测试。调用可执行文件，验证扫描、诊断、搜索、帮助和非法查询条数对应的退出码。脚本按顺序执行，没有自行定义函数。 |
| [tests/cache_cli_test.cmake](../tests/cache_cli_test.cmake) | 缓存参数与细分诊断的命令行回归测试。验证默认值、自定义值、越界/格式错误、缺少或重复参数，以及扫描专用选项在搜索时被拒绝；确认错误参数不会破坏旧索引。脚本没有自行定义函数。 |
| [tests/windows_paths.ps1](../tests/windows_paths.ps1) | Windows 路径与链接集成测试。验证中文、空格、根目录路径别名和链接相关行为。`Invoke-App` 收集程序输出并保存原生退出码，让 Windows PowerShell 的 stderr 表示方式不干扰断言。 |

测试资源放在测试夹具或构建目录中。UNC 管理共享不可用时，相关 C++ 用例返回 77，由 CTest 标记跳过。

## 构建、脚本与仓库配置

| 文件 | 大致作用 |
| --- | --- |
| [CMakeLists.txt](../CMakeLists.txt) | CMake 构建入口。指定项目版本与 C++20，下载并校验固定 SQLite 源码，定义数据库库、核心库、命令行和测试目标，配置 Windows/Unix 链接、编译警告、CTest 及安装规则。没有自行定义 CMake 函数。 |
| [scripts/build.ps1](../scripts/build.ps1) | Windows 构建入口脚本。下载并校验 SQLite 压缩包，配置 Debug/Release 构建，编译并运行 CTest；命令失败时终止。按顺序执行，没有自行定义函数。 |
| [scripts/benchmark.py](../scripts/benchmark.py) | 两版本性能与一致性基准。`main` 安排交替测量及 JSON 汇总；局部 `run_scan` 调用对应程序；局部 `verify` 校验格式、完整性、外键、元数据、父路径和根归属。支持新建、重扫或两者分组测量。 |
| [.github/workflows/build.yml](../.github/workflows/build.yml) | GitHub Actions 工作流。在 push/pull_request 时分别于 Windows MSVC 和 Ubuntu 环境配置、构建并运行测试，提供跨平台检查。 |
| [.gitignore](../.gitignore) | Git 忽略规则。排除构建产物、数据库、日志、本机配置、IDE 状态、Python 缓存和本地代码索引，减少提交生成文件及本机数据的风险。 |
| [.gitattributes](../.gitattributes) | Git 文本与换行规则。让源码、Markdown 和 CMake 文件使用 LF，PowerShell 文件使用 CRLF，保持跨平台提交的一致性。 |

`build/` 保存可执行文件、下载的 SQLite 依赖、测试夹具、索引、版本归档和本机基准结果。这些是生成或本机使用的文件，不属于本文逐文件列出的仓库手写代码；SQLite 源码由构建流程获取与校验。

## 使用、计划与历史报告

| 文件 | 大致作用 |
| --- | --- |
| [README.md](../README.md) | 项目首页。说明当前能力、环境要求、构建/测试命令、扫描/查询用法、诊断口径、数据库一致性、限制、后续路线以及依赖来源。 |
| [docs/code-guide.md](code-guide.md) | 本文。集中说明各文件职责、主要模块调用关系和推荐阅读顺序，配合源码中的函数注释阅读。 |
| [docs/v0.1-plan.md](v0.1-plan.md) | 第一版的范围、设计决策和实施记录，解释基础扫描、树形持久化及文件名查询的起点。 |
| [docs/reviews/v0.1-review.md](reviews/v0.1-review.md) | 第一版独立审查归档，记录测试结果、已发现的路径问题以及最初的扫描性能基线。 |
| [docs/v0.2-spec.md](v0.2-spec.md) | 第二版需求与约束。定义可靠 Windows 路径处理、原生枚举、数据库身份保护、诊断和验证范围。 |
| [docs/v0.2-plan.md](v0.2-plan.md) | 第二版实施计划与完成记录，说明路径/枚举辅助模块、扫描重构、测试和审查修复过程。 |
| [docs/v0.2-benchmark.md](v0.2-benchmark.md) | 第一版与第二版的受控性能对比、内容一致性证据、复现命令及测量限制。 |
| [docs/v0.3-plan.md](v0.3-plan.md) | 第三版重扫优化的原因、接口设计和实施记录，包含缓存目标、准备阶段计时及审查事项。 |
| [docs/v0.3-benchmark.md](v0.3-benchmark.md) | 第三版性能报告。分开记录大索引副本的数据库实验和受控完整扫描/重扫结果，说明缓存代价、兼容性、复现方法及测量限制。 |

历史计划和报告记录对应版本的状态；当前用法以 README 与公开接口为准。
