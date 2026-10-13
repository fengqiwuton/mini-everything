# GUI 前核心版本 v0.5 验收记录

2026-10-14。基线为 ae62c2f（v0.3 核心与函数/文件说明），开发分支为 feat/pre-gui-cli。本版将 v0.4/v0.5 的 GUI 前能力合并交付；保留 schema 1。

## 功能验收

| 能力 | 已验证行为 |
| --- | --- |
| 高级搜索 | 字面包含/精确/前缀、名称/路径、扩展名、类型、大小/时间、根/子树、排序、分页、SQL 特殊字符与 Unicode。 |
| 目录服务 | info、children、ancestors、tree、du；1200 层深链、20000 个直接子项、分页/深度/数量边界、父链循环和跨根异常。 |
| 索引管理 | roots、stats、check、forget；空/多根、聚合溢出、损坏树、forget SQL 失败整体回滚，磁盘文件保留。 |
| 增量维护 | 相同规范路径复用 ID；新增、修改、删除、类型变化、多根隔离；进度、取消、回调及 SQL 故障回滚。 |
| 监听 | Windows 原生事件和强制轮询、初次校准、有限轮次退出、取消、根 forget、物理根改名/替换/大小写更名。 |
| CLI | 默认五列 TSV、显式 ID、JSON/NDJSON、白名单/重复/缺值/整数溢出、以 -- 开头的字面文本、超时退出130。 |

本机 Windows MinGW-w64 GCC 14.2、CMake 3.31.5、SQLite 3.53.4：Debug **32/32**、Release **32/32** CTest 通过。query/catalog/maintenance 的套件分别含 8/9/13 组情景；CTest 的一项套件可包含多组情景。独立审查使用另一构建目录再次完成 Debug **32/32**。

远程 Windows MSVC / Ubuntu Release CI 在提交后运行，归档前补记结果。

复现本机测试：

~~~powershell
./scripts/build.ps1 -Configuration Release
./scripts/build.ps1 -Configuration Debug
~~~

## 独立审查与修复

独立 reviewer 阅读核心、CLI、测试和构建配置，并使用真实临时目录复现了三个 P2。修复后原探针和新增回归均通过，没有剩余确认的 P1/P2：

1. Windows 根仅改变大小写后，roots.path 未跟随新的规范路径，导致健康检查和祖先链错误。现在在扫描写事务内同步根路径，保留 root_id；默认/增量均覆盖取消、回调和 SQL 故障恢复旧路径及节点。
2. 原生目录通知不报告被监听目录自身的改名。现在空闲每250ms及每次校准前复核登记状态、磁盘目录身份及 Windows 规范路径，改名/替换/大小写更名时退出并保留已提交数据。
3. CLI 曾把所有以 -- 开头的字面文本当作缺失参数。现在按固定位置读取原始字符串，并回归 --literal.txt 和 --txt 扩展名。

另修复 forget 对缺失数据库创建空文件的问题：写连接可选择只打开已有文件，forget 使用该模式。测试先观察到失败，再验证修复。所有新函数、资源包装和回调保留中文说明；[代码导读](code-guide.md)列出维护文件的职责。

## 受控扫描对比

使用已有固定夹具：10000 文件、101 目录（包括根）。每轮每版本使用独立新数据库，随后重扫同一未变化根；3轮交替版本顺序，默认64MiB缓存。比较 v0.3 归档程序与 v0.5 Release，程序内部 elapsed_ms 的中位数如下。

| 独立实验 | 阶段 | v0.3 | v0.5 | 解释 |
| --- | --- | ---: | ---: | --- |
| v0.5 默认重建 | 新建 | 212 ms | 202 ms | 小夹具首次扫描近似相同。 |
| v0.5 默认重建 | 重扫 | 378 ms | 379 ms | 保留默认重建语义，未见明显中位退化。 |
| v0.5 --incremental | 新建 | 229 ms | 230 ms | 增量新建也需插入全部节点。 |
| v0.5 --incremental | 无变化重扫 | 389 ms | 101 ms | 约3.85倍；耗时降低约74%。 |

增量实验的3轮重扫为：

| 轮次 | v0.3 重建 | v0.5 增量 |
| --- | ---: | ---: |
| 1 | 389 ms | 106 ms |
| 2 | 371 ms | 101 ms |
| 3 | 427 ms | 99 ms |

候选版每轮重扫均为 inserted=updated=removed=0、unchanged=10101。仍遍历目录元数据、查询对应行和写入连接临时见到集合；省去旧节点全量删除/重插入。由于 scanned_at 精度为秒，根时间戳变化时可能仍有少量持久化页写出。默认重建第三轮曾有516ms、其中commit约230ms，体现小样本和磁盘写出波动，不以中位数掩盖其存在。

两组共24次计入测量的扫描均校验 application_id、schema版本、integrity_check、foreign_key_check，并逐条比对名称、完整路径、类型、大小、修改时间、扩展名、父路径和根归属，内容一致。另验证 v0.1/v0.2/v0.3 均可读取 v0.5 数据库并返回相同默认 TSV；v0.5 健康检查也接受 v0.3 数据库。

运行命令（root 请换成一个固定、测试期间不改动的目录）：

~~~powershell
python scripts/benchmark.py --baseline build/archive/v0.3/mini-everything.exe --candidate build/Release/mini-everything.exe --root <fixture> --output-dir build/v0.5-default-benchmark --rounds 3 --mode both --baseline-profile
python scripts/benchmark.py --baseline build/archive/v0.3/mini-everything.exe --candidate build/Release/mini-everything.exe --root <fixture> --output-dir build/v0.5-benchmark --rounds 3 --mode both --baseline-profile --candidate-incremental
~~~

原始本机结果位于忽略目录 build/v0.5-default-benchmark/comparison.json 和 build/v0.5-benchmark/comparison.json，不提交数据库或真实文件索引。

## 查询诊断

上述候选数据库中，对唯一名称 report-09990.pdf，三种模式交替运行9次，均返回1条：

| 模式 | 内部耗时中位数 | 含进程启动/输出的耗时中位数 |
| --- | ---: | ---: |
| substring | 6 ms | 39.419 ms |
| exact | 1 ms | 39.761 ms |
| prefix | 1 ms | 41.164 ms |

内部时间为整数毫秒，CLI启动成本会淹没小查询差异。使用相同 SQLite 3.53.4、名称条件和排序的 EXPLAIN QUERY PLAN 探针显示 exact 使用 idx_nodes_name(name=?)，prefix 使用 idx_nodes_name(name>? AND name<?)；路径/ID 的次排序仍使用临时 B-tree。包含查询不承诺同样的索引范围访问，深 OFFSET、子树过滤和其他排序也可能增加成本。

~~~powershell
python scripts/query_benchmark.py --app build/Release/mini-everything.exe --db <fixture-index.db> --text report-09990.pdf --rounds 9 --output build/v0.5-benchmark/query.json
~~~

## 边界与后续

这些是小型固定夹具且可能命中系统缓存的结果，不能推算 D:/Users 的约77万节点扫描耗时。本轮未改写用户实际索引，也未再次扫描该目录。首次扫描仍以文件系统枚举为主，下一阶段可单独研究 NTFS MFT/USN 与普通扫描回退。

watch 每次校准仍遍历整个根，事件只负责标脏与合并；原生缓冲溢出的恢复路径已实现，但没有对真实网络文件系统通知溢出做压力验收。回调只在成功提交后输出更新；取消当前轮不能撤销之前已提交的轮次。仍采用严格读取错误回滚和较长写事务，没有后台服务、文件内容搜索、GUI或真实文件移动/删除。

原生监听的目录自身通知限制及缓冲处理参见[微软 ReadDirectoryChangesW 文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)；取消后等待异步完成再释放资源遵循[微软 CancelIoEx 文档](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)。
