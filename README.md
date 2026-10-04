# MiniEverything

基于 C++20 和 SQLite 的 Windows 文件快速查找学习项目，重点练习树结构、数据库、并发与性能优化。

当前为 **v0.3 命令行版本**：指定目录扫描、SQLite 持久化、文件名搜索、扩展名筛选和结果条数限制。本版提高扫描连接的数据库缓存目标、优化重扫成本，并细分数据库准备耗时与缓存统计。尚未实现桌面界面、实时监听、内容搜索或 NTFS 加速。

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
.\build\Release\mini-everything.exe --help
```

- 搜索只访问已有数据库，磁盘文件变化后需要重新运行 `scan`。
- `scan --db-cache-mib` 支持整数 1～1024，默认 64。这是 SQLite 页缓存的近似目标、按需分配，进程还需其他内存；不是总内存硬上限。配置仅对当前连接有效，不写入数据库格式，也不延续到搜索连接。
- 默认最多返回 100 条，`--limit` 支持 1～10000；稳定按名称和完整路径排序。
- `%`、`_`、引号等作为字面字符处理；ASCII 英文大小写不敏感，其他 Unicode 字符按原字符匹配。
- 无扩展名筛选时结果包括文件和目录；`--ext pdf` 与 `--ext .PDF` 等价，仅匹配文件。
- `search` 的 stdout 为 TSV 结果，控制字符会转义；stderr 为结果数量和耗时。`scan` 的 stdout 为索引数量摘要，stderr 为耗时、可选诊断和错误信息。大小单位为字节，修改时间为 Unix 秒。目录大小为 0，不代表子树大小。
- Windows 存储和输出的规范路径可能带 `\\?\` 前缀，用于统一路径别名并支持长路径。
- 退出码：0 成功（含无匹配结果），1 操作失败，2 参数或扫描范围错误。

## 数据和一致性

`roots` 管理扫描根目录；`nodes.parent_id` 保存树结构，同时保存完整路径缓存。数据库用 `application_id` 和 `user_version` 标识格式，拒绝修改非本项目或不兼容版本的数据库。

扫描采用迭代 DFS、复用预编译 SQL 和单次事务。相同根目录重扫会整体替换旧节点，节点 ID 不保证跨扫描稳定；任何目录读取或数据库写入失败都会回滚，保留上次成功索引。新增或删除的文件在成功重扫后反映到索引。

Windows 使用原生目录枚举直接取得大小、属性和修改时间，避免逐文件重复读取元数据。缓存数据库文件身份；仅当扫描根与数据库都确认为本地盘、数据库只有一个硬链接时，省去逐文件身份查询。UNC、映射网络盘或多硬链接数据库仍执行身份检查；提交前检查数据库链接数，发生变化则回滚。数据库格式仍为 schema 1，可继续查询和重扫 v0.1/v0.2 的索引。

`--profile` 在 stderr 输出 `prepare_ms`（数据库准备与旧索引清理）、`metadata_ms`（Windows 枚举与元数据）、`identity_ms`（文件身份检查）、`write_ms`（序列化与 SQL 插入）、`commit_ms` 和 `identity_checks`。这些是诊断分类，部分调度与路径准备时间不在分类内，不能当作精确的磁盘 I/O 占比。

v0.3 将 `prepare_ms` 进一步拆为 `open_ms`、`transaction_ms`（含等待写锁）、`schema_ms`、`roots_ms`（根目录检查/创建）、`clear_ms`（本根旧节点删除，新建索引为 0）、`statements_ms`。这些子阶段属于 prepare，分析总耗时时不要再重复相加。`db_cache_mib` 表示请求的缓存目标；`cache_hits`、`cache_misses`、`cache_writes`、`cache_spills` 是 SQLite 连接从打开至提交后的累计统计，不等同物理磁盘 I/O 次数；不可用时为 -1。

缓存调节保留事务、外键检查、日志、同步和脏页溢出机制，参见 [SQLite cache_size 文档](https://www.sqlite.org/pragma.html#pragma_cache_size)。缓存增大会将部分写出工作移到提交阶段，应比较整次扫描耗时。

实测、受控对比与复现命令见 [v0.3 性能报告](docs/v0.3-benchmark.md)，历史结果见 [v0.2 性能报告](docs/v0.2-benchmark.md)。实际耗时仍受磁盘、缓存、目录分布和并发文件修改影响。

索引支持互不重叠的多个根目录。符号链接、Windows 目录联接及其他重解析点会跳过并计数；作为输入的根路径会解析到真实位置。扫描范围内不能存放本次使用的数据库，也不能包含指向该库的硬链接。数据库的父目录需要事先存在。

这是当前文件系统的尽力扫描，不是文件系统快照：扫描期间文件变化可能导致整次扫描失败或结果需要再次校准。大目录扫描会占用较长写事务；尚未实现取消按钮、进度 UI、局部权限容错或增量维护。Windows 大小写敏感目录不是 v0.3 的支持目标。

## 目录

```text
include/mini/index.hpp  核心扫描和查询接口
src/index.cpp          邻接表、扫描事务和查询
src/sqlite.hpp         SQLite RAII 封装
src/paths.hpp          UTF-8 与 Windows 路径归一化
src/windows_fs.hpp     原生 Windows 枚举与数据库身份检查
src/main.cpp           命令行参数与结果输出
tests/                 真实临时文件系统与 CLI 集成测试
scripts/build.ps1      下载校验、编译和测试
scripts/benchmark.py   两版本扫描耗时与索引一致性对比
docs/v0.3-plan.md       本版范围和实施记录
```

## 后续路线

| 版本 | 目标 |
| --- | --- |
| v0.2（已实现） | Windows 扫描优化、长路径修复、阶段诊断与可复现基准 |
| v0.3（已实现） | 重扫页缓存优化、可调缓存目标、数据库阶段诊断 |
| v0.4 | 树查询、扫描进度与取消、搜索索引优化 |
| v0.5 | 增量监听与恢复 |
| v0.6 | NTFS MFT/USN 实验及普通扫描回退 |
| v1.0 | 桌面界面、打包与稳定性验收 |

当前包含搜索使用 SQLite `LIKE '%text%'`，不承诺普通名称索引可以加速任意子串查询。后续分别测量精确、前缀与子串搜索的延迟、内存和索引体积。

## Git 与依赖来源

使用 `main` 与 `feat/*` / `fix/*` 分支；测试后提交并推送远程。源码、测试、迁移和基准报告纳入版本管理，真实文件索引、日志和本机配置不提交。

SQLite 源码来自 [SQLite 官方下载](https://www.sqlite.org/download.html)，其代码属于公有领域，见 [SQLite 版权说明](https://www.sqlite.org/copyright.html)。SQLite 源码只在构建目录下载，不复制到仓库中。
