# 项目函数与文件职责说明

本文覆盖仓库维护的源码、测试、脚本、构建配置和现有文档。函数前的中文注释说明用途、关键约定及异常或资源释放行为；构造/析构函数、被禁用的复制接口和局部辅助函数也有说明。

## 推荐阅读顺序

先读 [README](../README.md) 和 [命令行使用手册](cli-usage.md)，再读 [扫描接口](../include/mini/index.hpp)、[查询接口](../include/mini/query.hpp)、[管理接口](../include/mini/catalog.hpp)、[监听接口](../include/mini/watch.hpp) 与 [命令行入口](../src/main.cpp)。随后阅读对应实现、共享读事务、路径/SQLite/Windows 辅助模块及测试；当前范围和不变量集中在 [GUI 前规格](pre-gui-spec.md)。

扫描调用关系为：`main → parse/run_maintenance → scan → 路径和根身份检查 → Database/Transaction → 元数据枚举 → 节点插入或增量更新 → 失效节点清理 → 提交`。进度与取消仅在提交前参与扫描，性能/结果输出在成功返回后进行。

查询和管理读取为：`main → query/get_node/children/ancestors/tree/summarize 或 list_roots/index_stats/check_index → ReadIndex → 参数化 SQL → 持久化结果 → cli_output`。`ReadIndex` 在只读连接上持有同一事务，`require_index` 检查 schema 1 应用/版本标记；完整健康检查由 catalog 模块负责。

监听调用关系为：`main → watch → 注册根/文件系统身份 → ChangeFeed 先捕获变化 → scan(incremental, expected_root_id) → 每轮提交后 on_update`。Windows 后台线程只捕获事件，不访问 SQLite；维护线程串行执行校准。`forget_root` 则单独持有写事务，先删除该根节点，再删除 roots 行。

## 源码与公开接口

| 文件 | 大致作用 |
| --- | --- |
| [include/mini/index.hpp](../include/mini/index.hpp) | 扫描接口与基础结构。定义 `Node`、扫描数量/变化/耗时/缓存统计、进度阶段、`ScanCancelled`、取消/进度/增量配置，保留旧 `SearchOptions` 与 `search` 接口。调用者传入原生路径，数据库文本使用 UTF-8。 |
| [include/mini/query.hpp](../include/mini/query.hpp) | 高级查询接口。定义匹配模式、节点类型、排序、分页、大小/时间/根/子树过滤，以及节点详情、直接子项、祖先、有限树和子树统计接口。 |
| [include/mini/catalog.hpp](../include/mini/catalog.hpp) | 索引管理接口。定义根列表、全库统计、健康结果和 forget；根 ID 与节点 ID 独立，统计字节只累计文件。 |
| [include/mini/watch.hpp](../include/mini/watch.hpp) | 监听维护接口。定义校准间隔、有限轮次、轮询选择、成功提交回调，以及实际事件模式和更新次数结果。 |
| [src/main.cpp](../src/main.cpp) | 命令行入口。转换 Windows 宽字符参数，按命令白名单验证位置参数/重复/范围，调度全部核心服务，管理信号处理器与限时取消，映射退出码 0/1/2/130。 |
| [src/cli_output.hpp](../src/cli_output.hpp) | 命令行展示辅助。转义 TSV 和 JSON 文本，输出节点、树、根列表、统计、健康结果和扫描概要，保留原五列搜索与全部性能诊断字段；watch 逐轮输出 NDJSON。 |
| [src/index.cpp](../src/index.cpp) | 扫描业务。校验/创建 schema 1、重叠范围与 expected_root_id；迭代 DFS 支持重建或复用相同路径 ID 的增量写入，临时 scan_seen 表记录本轮节点并清理失效行。进度/取消和 SQLite 进度处理器使提交前故障回滚；另保留兼容搜索。 |
| [src/query.cpp](../src/query.cpp) | 查询业务。验证枚举与边界，以参数绑定组合高级筛选和稳定排序；只读查询节点与父链，用去重递归 SQL 获取子树并限制树扩展，检测异常祖先关系和统计溢出。 |
| [src/catalog.cpp](../src/catalog.cpp) | 管理业务。一次聚合返回根列表或统计；IMMEDIATE 写事务执行 forget 并完整回滚故障。健康检查检查 SQLite、外键、元数据类型、根身份、跨根父关系及从无父节点不可达的断链/循环，只输出有限诊断类别。 |
| [src/watch.cpp](../src/watch.cpp) | 维护业务。检查根仍注册与根目录身份，Windows 用 ReadDirectoryChangesW 异步捕获并合并变化/溢出，非 Windows 或显式选择时轮询；先捕获再校准，每轮串行增量事务，提交后回调，取消保留先前轮次。 |
| [src/read_index.hpp](../src/read_index.hpp) | 共享读事务。`require_index` 校验 application_id/user_version；`ReadIndex` 规范化 UTF-8 数据库路径，打开只读连接、BEGIN 建立同一读快照，析构结束事务而不写入索引。 |
| [src/paths.hpp](../src/paths.hpp) | 跨平台路径辅助函数。负责 UTF-8 与原生路径转换、绝对路径规范化与别名解析、路径分量比较、祖先范围判断、链接识别及 ASCII 扩展名归一化。Windows 分支使用原生 API 支持扩展路径。 |
| [src/sqlite.hpp](../src/sqlite.hpp) | SQLite 的 RAII 包装。`Database` 管理连接、连接缓存与错误；`Statement` 管理预编译语句、参数绑定和结果读取；`Transaction` 开启/提交事务并在未提交时尽力回滚。 |
| [src/windows_fs.hpp](../src/windows_fs.hpp) | Windows 原生文件系统辅助模块。`FileHandle` 管理身份查询句柄；`IndexFileGuard` 检查索引硬链接及链接数变化；`DirectoryCursor` 逐项枚举目录并直接提供元数据；另提供本地盘判定与 FILETIME 转换。仅 Windows 编译该实现。 |

树结构由 `nodes.parent_id` 表示，`nodes.root_id` 指向 `roots.id`，完整路径缓存保存在 `nodes.full_path`。根记录 ID 与根节点 ID 不同。搜索和目录浏览只查询 SQLite；磁盘变化需要重建/增量扫描、refresh 或 watch 校准后反映到结果中，forget 只移除索引记录。

## 测试

| 文件 | 大致作用 |
| --- | --- |
| [tests/index_tests.cpp](../tests/index_tests.cpp) | C++ 核心集成测试与夹具。创建真实临时目录、文件和 SQLite 数据库，验证树关系、持久化、重扫、查询、范围检查、格式兼容、硬链接、回滚和缓存策略。Windows 分支另测扩展/长路径、卷根、UNC 别名及目录读取故障；入口根据名称执行单个用例。 |
| [tests/read_index_tests.cpp](../tests/read_index_tests.cpp) | 共享只读事务测试。用随机临时数据库验证标记检查、拒绝写操作、连接资源释放与不兼容版本拒绝。 |
| [tests/query_tests.cpp](../tests/query_tests.cpp) | 高级查询/目录集成测试。验证字面与 Unicode 匹配、筛选组合、排序分页、ID 查询、祖先/有限树/统计、离线数据、参数边界、损坏父链和溢出。无参数入口执行全部用例。 |
| [tests/catalog_tests.cpp](../tests/catalog_tests.cpp) | 索引管理集成测试。验证空/多根、路径排序、独立根/节点 ID、文件字节与含根目录计数、forget 保留磁盘文件、节点/根删除阶段 SQL 故障回滚、格式拒绝、溢出及损坏根/外键/跨根/循环。 |
| [tests/maintenance_tests.cpp](../tests/maintenance_tests.cpp) | 扫描维护集成测试。验证增量无变化/插入/更新/删除/类型变化与稳定 ID、取消/进度回调/SQL 故障回滚、根 ID 复核等扫描不变量。 |
| [tests/watch_tests.cpp](../tests/watch_tests.cpp) | 监听集成测试。只在随机临时根修改文件，验证初次与后续校准、有限轮次、原生事件/轮询、取消和根失效处理；由 CTest 设置限时防止测试永久监听。 |
| [tests/cli_test.cmake](../tests/cli_test.cmake) | 基础命令行集成测试。调用可执行文件，验证扫描、诊断、搜索、帮助和非法查询条数对应的退出码。脚本按顺序执行，没有自行定义函数。 |
| [tests/cache_cli_test.cmake](../tests/cache_cli_test.cmake) | 缓存参数与细分诊断的命令行回归测试。验证默认值、自定义值、越界/格式错误、缺少或重复参数，以及扫描专用选项在搜索时被拒绝；确认错误参数不会破坏旧索引。脚本没有自行定义函数。 |
| [tests/pre_gui_cli.cmake](../tests/pre_gui_cli.cmake) | GUI 前 CLI 全流程回归。随机根保证重复运行互不污染；验证高级搜索/默认五列/ID、JSON、目录浏览、根管理、refresh、watch NDJSON、限时取消与旧索引保留，以及重复/缺值/白名单/整数边界、--all 全部搜索和以 -- 开头的字面文本。辅助函数区分成功、参数错误和取消。 |
| [tests/windows_paths.ps1](../tests/windows_paths.ps1) | Windows 路径、链接与 PowerShell 全部搜索集成测试。文件使用 UTF-8 BOM，供 Windows PowerShell 5.1 正确读取中文注释。验证中文、空格、根目录路径别名和链接相关行为。`Invoke-App` 收集程序输出并保存原生退出码，让 Windows PowerShell 的 stderr 表示方式不干扰断言。 |

测试资源放在测试夹具或构建目录中。UNC 管理共享不可用时，相关 C++ 用例返回 77，由 CTest 标记跳过。

## 构建、脚本与仓库配置

| 文件 | 大致作用 |
| --- | --- |
| [CMakeLists.txt](../CMakeLists.txt) | CMake 构建入口。指定项目版本与 C++20，下载并校验固定 SQLite 源码，定义数据库库、核心库、命令行和测试目标，配置 Windows/Unix 链接、编译警告、CTest 及安装规则。没有自行定义 CMake 函数。 |
| [scripts/build.ps1](../scripts/build.ps1) | Windows 构建入口脚本。下载并校验 SQLite 压缩包，配置 Debug/Release 构建，编译并运行 CTest；命令失败时终止。按顺序执行，没有自行定义函数。 |
| [scripts/benchmark.py](../scripts/benchmark.py) | 两版本性能与一致性基准。`main` 安排交替测量及 JSON 汇总；局部 `run_scan` 调用对应程序；局部 `verify` 校验格式、完整性、外键、元数据、父路径和根归属。支持新建/重扫分组，以及 `--candidate-incremental` 对候选版本的增量重扫测量。 |
| [scripts/query_benchmark.py](../scripts/query_benchmark.py) | 只读 CLI 查询基准。交替测量 substring/exact/prefix 三种模式，记录 CLI 内部耗时、包含进程启动的 wall ms、JSON 结果行数并汇总，不扫描或修改被测索引。 |
| [.github/workflows/build.yml](../.github/workflows/build.yml) | GitHub Actions 工作流。在 push/pull_request 时分别于 Windows MSVC 和 Ubuntu 环境配置、构建并运行测试，提供跨平台检查。 |
| [.gitignore](../.gitignore) | Git 忽略规则。排除构建产物、数据库、日志、本机配置、IDE 状态、Python 缓存和本地代码索引，减少提交生成文件及本机数据的风险。 |
| [.gitattributes](../.gitattributes) | Git 文本与换行规则。让源码、Markdown 和 CMake 文件使用 LF，PowerShell 文件使用 CRLF，保持跨平台提交的一致性。 |

`build/` 保存可执行文件、下载的 SQLite 依赖、测试夹具、索引、版本归档和本机基准结果。这些是生成或本机使用的文件，不属于本文逐文件列出的仓库手写代码；SQLite 源码由构建流程获取与校验。

## 使用、计划与历史报告

| 文件 | 大致作用 |
| --- | --- |
| [README.md](../README.md) | 项目首页。说明当前能力、环境要求、构建/测试命令、扫描/查询用法、诊断口径、数据库一致性、限制、后续路线以及依赖来源。 |
| [docs/cli-usage.md](cli-usage.md) | v0.5 完整命令行手册。说明全部命令和参数范围、根/节点 ID 区别、增量/监听/取消语义、TSV/JSON/NDJSON 格式、诊断口径和退出码。 |
| [docs/code-guide.md](code-guide.md) | 本文。集中说明各文件职责、主要模块调用关系和推荐阅读顺序，配合源码中的函数注释阅读。 |
| [docs/pre-gui-spec.md](pre-gui-spec.md) | GUI 前需求与不变量。定义高级搜索/目录/管理/维护范围、schema 1 与旧输出兼容、事务/取消/监听约束及隔离真实用户数据的验收要求。 |
| [docs/pre-gui-plan.md](pre-gui-plan.md) | GUI 前实施计划与接口合同。记录模块边界、文件所有权、任务划分、先失败后实现的验证顺序、接口字段与整合验收安排。 |
| [docs/pre-gui-validation.md](pre-gui-validation.md) | GUI 前验证报告。记录本版构建/测试、独立审查、跨平台检查、实际基准与索引一致性证据及验证限制；性能结论以报告中的具体测量为准。 |
| [docs/v0.1-plan.md](v0.1-plan.md) | 第一版的范围、设计决策和实施记录，解释基础扫描、树形持久化及文件名查询的起点。 |
| [docs/reviews/v0.1-review.md](reviews/v0.1-review.md) | 第一版独立审查归档，记录测试结果、已发现的路径问题以及最初的扫描性能基线。 |
| [docs/v0.2-spec.md](v0.2-spec.md) | 第二版需求与约束。定义可靠 Windows 路径处理、原生枚举、数据库身份保护、诊断和验证范围。 |
| [docs/v0.2-plan.md](v0.2-plan.md) | 第二版实施计划与完成记录，说明路径/枚举辅助模块、扫描重构、测试和审查修复过程。 |
| [docs/v0.2-benchmark.md](v0.2-benchmark.md) | 第一版与第二版的受控性能对比、内容一致性证据、复现命令及测量限制。 |
| [docs/v0.3-plan.md](v0.3-plan.md) | 第三版重扫优化的原因、接口设计和实施记录，包含缓存目标、准备阶段计时及审查事项。 |
| [docs/v0.3-benchmark.md](v0.3-benchmark.md) | 第三版性能报告。分开记录大索引副本的数据库实验和受控完整扫描/重扫结果，说明缓存代价、兼容性、复现方法及测量限制。 |

历史计划和报告记录对应版本的状态；当前用法以 README、命令行使用手册与公开接口为准。
