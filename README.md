# MiniEverything

基于 C++20 和 SQLite 的 Windows 文件快速查找学习项目，重点练习树结构、数据库、并发与性能优化。

当前为 **v0.5 GUI 前命令行版本**：目录扫描与 SQLite 持久化、高级名称/路径搜索、树浏览、索引管理、扫描进度与取消、增量校准，以及 Windows 目录变化监听和跨平台轮询。提供 TSV、JSON 和 watch 的逐行 JSON，公开核心接口可供后续 GUI 复用。桌面界面、内容全文搜索和 NTFS MFT/USN 加速尚未实现。

## 构建与测试

需要 CMake 3.24+、支持 C++20 的编译器和构建工具。本机使用 MinGW-w64 GCC 14.2 + Ninja。Windows CI 使用 MSVC，另有 Linux 核心测试任务。

在项目根目录运行：

```powershell
.\scripts\build.ps1 -Configuration Release
```

该脚本用 Windows 证书存储下载固定版本的 SQLite 3.53.4，核验 SHA256 后编译并运行 CTest。首次构建需要联网，后续可复用下载缓存；不会关闭 TLS 校验。MinGW 编译器、Ninja 及运行时 DLL 需在 PATH 中。

MSVC 用户可从 Visual Studio 开发者 PowerShell 使用 Ninja 构建；也可用下方通用命令并省略 `-G Ninja`，由 CMake 选择 Visual Studio 生成器。不同编译器请使用不同构建目录。

```powershell
cmake -S . -B build/manual -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/manual --config Release --parallel
ctest --test-dir build/manual -C Release --output-on-failure
```

离线或 CMake 证书配置有问题时，可传 `-DMINI_SQLITE_ARCHIVE=<官方zip的绝对路径>`；仍会校验 SHA256。不需要单独安装 SQLite。

## 使用

```powershell
# 先确认 D:\Documents 存在；数据库必须放在被扫描目录之外。
.\build\Release\mini-everything.exe scan "D:\Documents" --db ".\build\index.db"

# 额外输出扫描阶段耗时，便于定位大目录扫描瓶颈。
.\build\Release\mini-everything.exe scan "D:\Documents" --db ".\build\index.db" --profile

# 默认缓存目标为 64 MiB；可按内存条件调整，仅影响本次扫描连接。
.\build\Release\mini-everything.exe scan "D:\Documents" --db ".\build\index.db" --profile --db-cache-mib 128

# 按名称包含关系搜索；中文与空格路径均支持。
.\build\Release\mini-everything.exe search "报告" --db ".\build\index.db"
.\build\Release\mini-everything.exe search "report" --db ".\build\index.db" --ext pdf --limit 50

# 高级筛选、排序和结构化输出。
.\build\Release\mini-everything.exe search "report" --db ".\build\index.db" --mode prefix --type file --sort size --desc --format json

# 先取得 ROOT_ID 和 NODE_ID；后续命令分别使用对应的 ID。
.\build\Release\mini-everything.exe roots --db ".\build\index.db"
.\build\Release\mini-everything.exe stats --db ".\build\index.db"
.\build\Release\mini-everything.exe check --db ".\build\index.db"

# 对同一个目录增量校准，保留仍存在的相同路径节点 ID，显示进度。
.\build\Release\mini-everything.exe scan "D:\Documents" --db ".\build\index.db" --incremental --progress
.\build\Release\mini-everything.exe --help
```

- 完整命令、筛选参数、树浏览、`refresh`/`watch` 和脚本用法见 [命令行使用手册](docs/cli-usage.md)。
- 搜索与目录浏览只访问已有索引；离线文件仍可查询。磁盘变化通过 `scan --incremental`、`refresh <root-id>` 或 `watch <root-id>` 校准。
- `scan`/`refresh`/`watch` 的 `--db-cache-mib` 支持整数 1～1024，默认 64。这是 SQLite 页缓存的近似目标、按需分配，进程还需其他内存；不是总内存硬上限。配置仅对当前扫描连接有效，不改变格式或查询连接。
- 默认最多返回 100 条，`--limit` 支持 1～10000；稳定按名称和完整路径排序。
- `%`、`_`、引号等作为字面字符处理；ASCII 英文大小写不敏感，其他 Unicode 字符按原字符匹配。
- 无扩展名筛选时结果包括文件和目录；`--ext pdf` 与 `--ext .PDF` 等价，仅匹配文件。
- 默认 `search` 的 stdout 仍是 `TYPE SIZE MODIFIED_UNIX NAME PATH` 五列 TSV，`--show-ids` 在前面增加 `ID PARENT_ID`；控制字符会转义。所有命令可选 `--format json`，watch 每次提交输出一个 JSON 对象行。数量、进度、耗时、性能诊断和错误在 stderr。大小单位为字节，修改时间为 Unix 秒；目录大小为 0，子树大小使用 `du <node-id>`。
- Windows 存储和输出的规范路径可能带 `\\?\` 前缀，用于统一路径别名并支持长路径。
- `roots` 显示独立的 `ROOT_ID` 和 `NODE_ID`。`forget <root-id>` 仅删除该根的数据库记录，保留磁盘文件；`info`/`children`/`ancestors`/`tree`/`du` 使用节点 ID。
- 维护命令支持 `--progress`、Ctrl+C 和 `--timeout-ms`；未提交扫描取消会回滚，watch 已成功提交的轮次保留。退出码：0 成功（含无匹配结果），1 操作失败或健康检查不通过，2 参数或扫描范围错误，130 取消。

## 数据和一致性

`roots` 管理扫描根目录；`nodes.parent_id` 保存树结构，同时保存完整路径缓存。数据库用 `application_id` 和 `user_version` 标识格式，拒绝修改非本项目或不兼容版本的数据库。

扫描采用迭代 DFS、复用预编译 SQL 和单次事务。默认 `scan` 仍重建目标根，节点 ID 不保证跨扫描稳定。`--incremental` 复用仍存在的相同规范路径 ID，仅写变化行，并通过连接临时表清理本轮未出现的旧节点；路径移动或以后重建可以改变 ID。目录读取、SQL、进度回调故障或提交前取消都会回滚，保留上次成功索引。

Windows 使用原生目录枚举直接取得大小、属性和修改时间，避免逐文件重复读取元数据。缓存数据库文件身份；仅当扫描根与数据库都确认为本地盘、数据库只有一个硬链接时，省去逐文件身份查询。UNC、映射网络盘或多硬链接数据库仍执行身份检查；提交前检查数据库链接数，发生变化则回滚。数据库格式沿用 **schema 1**，无需迁移即可使用旧版索引，旧版仍可读取本版数据库。

读服务使用只读连接和同一读事务。`forget` 在一个写事务内删除节点与根记录，SQL 故障会完整回滚；`check` 检查 SQLite 完整性、外键、根身份和父链，不自动修复。`refresh`/`watch` 在写事务内复核已注册根 ID，避免其他命令 forget 后重新登记该根。

`watch` 在 Windows 先建立 `ReadDirectoryChangesW` 捕获，再做初次校准；变化事件合并后触发完整目录元数据遍历，通知缓冲溢出也触发校准。非 Windows、原生通知不支持或显式 `--poll` 时按间隔轮询。空闲时每 250 ms 或校准前复核根自身身份；根改名、替换、消失或大小写路径变化会报错退出，保留已提交数据，校准后需重新启动。增量校准依然遍历整个根，未提供逐事件直接更新百万文件的能力。

`--profile` 在 stderr 输出 `prepare_ms`（扫描前数据库准备）、`metadata_ms`（Windows 枚举与元数据）、`identity_ms`（文件身份检查）、`write_ms`（序列化与 SQL 写入）、`commit_ms` 和 `identity_checks`。这些是诊断分类，部分调度与路径准备时间不在分类内，不能当作精确的磁盘 I/O 占比。

细分字段包括 `open_ms`、`transaction_ms`（含等待写锁）、`schema_ms`、`roots_ms`（根目录检查/创建）、`clear_ms` 和 `statements_ms`。默认重建的 `clear_ms` 是扫描前删除旧节点，属于 prepare；增量模式的 `clear_ms` 是遍历结束后删除失效节点，**不属于 prepare**。分析总耗时时不要重复相加。`db_cache_mib` 表示请求的缓存目标；`cache_hits`、`cache_misses`、`cache_writes`、`cache_spills` 是连接从打开至提交后的累计统计，不等同物理磁盘 I/O 次数；不可用时为 -1。

缓存调节保留事务、外键检查、日志、同步和脏页溢出机制，参见 [SQLite cache_size 文档](https://www.sqlite.org/pragma.html#pragma_cache_size)。缓存增大会将部分写出工作移到提交阶段，应比较整次扫描耗时。

历史受控测量与复现命令见 [v0.3 性能报告](docs/v0.3-benchmark.md) 和 [v0.2 性能报告](docs/v0.2-benchmark.md)。这些历史数据不代表 v0.5 的性能；当前验收与测量记录见 [GUI 前验证报告](docs/pre-gui-validation.md)。实际耗时受磁盘、缓存、目录分布和并发文件修改影响。

索引支持互不重叠的多个根目录。符号链接、Windows 目录联接及其他重解析点会跳过并计数；作为输入的根路径会解析到真实位置。扫描范围内不能存放本次使用的数据库，也不能包含指向该库的硬链接。数据库的父目录需要事先存在。

这是当前文件系统的尽力扫描，不是文件系统快照：并发变化可能导致整次扫描失败或需要再次校准。大目录扫描会占用较长写事务；取消为协作检查，正在执行的系统调用和进入 COMMIT 后的提交不保证立刻中断。没有 GUI 进度条或局部权限容错；Windows 大小写敏感目录不是本版支持目标。

## 目录

各文件职责、主要调用关系和推荐阅读顺序见 [项目代码导读](docs/code-guide.md)。源码、测试和脚本中的函数前均有中文用途说明。

```text
include/mini/          扫描、查询、索引管理与维护公开接口
src/index.cpp          重建/增量扫描、进度取消和兼容搜索
src/query.cpp          高级搜索、节点与树浏览
src/catalog.cpp        根列表、统计、forget 和健康检查
src/watch.cpp          Windows 事件捕获与轮询校准
src/read_index.hpp     共享只读事务和格式校验
src/sqlite.hpp         SQLite RAII 封装
src/paths.hpp          UTF-8 与 Windows 路径归一化
src/windows_fs.hpp     原生 Windows 枚举与数据库身份检查
src/main.cpp           命令行参数、取消和服务调度
src/cli_output.hpp     TSV/JSON/NDJSON 与诊断输出
tests/                 真实临时文件系统与 CLI 集成测试
scripts/build.ps1      下载校验、编译和测试
scripts/benchmark.py   两版本扫描耗时与索引一致性对比
docs/cli-usage.md       全部命令与参数使用手册
docs/pre-gui-spec.md    GUI 前范围与不变量
docs/pre-gui-plan.md    实施计划与接口合同
```

## 后续路线

| 版本 | 目标 |
| --- | --- |
| v0.2（已实现） | Windows 扫描优化、长路径修复、阶段诊断与可复现基准 |
| v0.3（已实现） | 重扫页缓存优化、可调缓存目标、数据库阶段诊断 |
| v0.4/v0.5（本版实现） | 高级搜索与树查询、索引管理、扫描进度与取消、增量监听与校准 |
| v0.6 | NTFS MFT/USN 实验及普通扫描回退 |
| v1.0 | 桌面界面、打包与稳定性验收 |

当前包含搜索使用 SQLite `LIKE '%text%'`，不承诺普通名称索引可以加速任意子串查询。后续分别测量精确、前缀与子串搜索的延迟、内存和索引体积。

## Git 与依赖来源

使用 `main` 与 `feat/*` / `fix/*` 分支；测试后提交并推送远程。源码、测试、迁移和基准报告纳入版本管理，真实文件索引、日志和本机配置不提交。

SQLite 源码来自 [SQLite 官方下载](https://www.sqlite.org/download.html)，其代码属于公有领域，见 [SQLite 版权说明](https://www.sqlite.org/copyright.html)。SQLite 源码只在构建目录下载，不复制到仓库中。
