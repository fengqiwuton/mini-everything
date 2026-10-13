# 命令行使用手册（v0.5）

以下示例从仓库根目录运行，数据库目录需要事先存在，数据库必须在扫描范围外。

```powershell
$app = ".\build\Release\mini-everything.exe"
$db = ".\build\index.db"
& $app scan "D:\Documents" --db $db --progress
& $app --help
```

除 `--help`/`--version` 外，所有命令都必须带 `--db <path>`，可选 `--format tsv|json`，默认 TSV。未知、重复、越界或缺少值的选项会被拒绝；开关不带 `true`/`false` 值。搜索文本位于命令后第一个位置；`--draft` 等以 `--` 开头的字面文本和扩展名也支持。

## 扫描、进度和取消

```powershell
# 默认重建该根，节点 ID 不保证跨扫描稳定。
& $app scan "D:\Documents" --db $db

# 增量校准同一个目录，复用保留下来的相同规范路径节点 ID。
& $app scan "D:\Documents" --db $db --incremental --progress --profile --db-cache-mib 128

# 批处理限时：60 秒后请求取消；0 表示不限时。
& $app scan "D:\Documents" --db $db --incremental --timeout-ms 60000 --format json
```

`scan` 支持互不重叠的多个根。链接、Windows 联接和其他重解析点会跳过；数据库及其硬链接不能位于扫描范围内。扫描严格处理读取错误，不会默默把读取失败的目录当作空目录。

`--incremental` 仍遍历全部目录元数据，仅复用相同路径 ID、写变化行并删除失效节点；移动文件或以后默认重建可能改变 ID。统计 `inserted`、`updated`、`removed`、`unchanged` 都按节点行计数，包含目录，文件内容变化只有其大小或修改时间等元数据变化才会反映为更新。

`scan`、`refresh`、`watch` 共用这些选项：

| 选项 | 默认值与范围 | 行为 |
| --- | --- | --- |
| `--progress` | 关闭 | 在 stderr 显示 preparing/scanning/committing 阶段及文件、目录、跳过数量。 |
| `--profile` | 关闭 | 在 stderr 输出扫描阶段耗时、身份检查与 SQLite 缓存统计。 |
| `--db-cache-mib N` | 64；1..1024 | 本次扫描连接的页缓存目标，不是进程总内存上限。 |
| `--timeout-ms N` | 0；0..86400000 | 从维护命令开始计算的协作取消截止时间，0 不限时。watch 整段运行共用一个截止时间。 |

Ctrl+C（Windows 也支持 Ctrl+Break）和超时请求都返回退出码 **130**。提交前取消或进度回调/SQL/读取故障会回滚当前扫描；watch 先前成功提交的轮次保留。取消在正常执行路径检查，系统调用或已进入 COMMIT 的提交不保证立即中断；成功完成提交的扫描仍报告成功。

默认扫描 stdout 为 `Indexed ...` 数量概要。JSON 扫描与 refresh 输出一个对象：

```json
{"files":2,"directories":3,"skipped":0,"inserted":5,"updated":0,"removed":0,"unchanged":0}
```

增量的 `Changes: ...` 诊断始终在 stderr。性能字段保留 `prepare_ms`、`open_ms`、`transaction_ms`、`schema_ms`、`roots_ms`、`clear_ms`、`statements_ms`、`metadata_ms`、`identity_ms`、`write_ms`、`commit_ms`、`identity_checks` 和缓存计数。默认重建的 `clear_ms` 属于扫描前 prepare；增量的 `clear_ms` 在遍历结束后清理失效节点，不能计入 prepare。子阶段不要和汇总值重复相加，缓存计数也不等同物理磁盘 IO。

## 名称、路径与元数据搜索

```powershell
& $app search "报告" --db $db
& $app search "report.pdf" --db $db --mode exact --type file
& $app search "report" --db $db --mode prefix --ext PDF --limit 50
& $app search "\Projects\" --db $db --path --type file --sort mtime --desc
& $app search "" --db $db --type file --min-size 1048576 --max-size 10485760 --sort size --desc
& $app search "report" --db $db --after 1704067200 --before 1735689599 --limit 20 --offset 20 --format json
```

| 选项 | 默认值与范围 | 行为 |
| --- | --- | --- |
| `--mode substring|exact|prefix` | substring | 字面包含、完整相等或开头匹配。 |
| `--path` | 关闭 | 将文本匹配字段从名称切换到完整规范路径。 |
| `--type any|file|dir` | any | 限定文件或目录。 |
| `--ext TEXT` | 无 | 仅匹配文件；`pdf`、`.PDF` 等价。 |
| `--root ID` | 无；正 int64 | 限定注册根 ID。 |
| `--under ID` | 无；正 int64 | 限定节点及其整个子树，可与根范围组合。 |
| `--min-size N` / `--max-size N` | 无；非负 int64 | 包含上下界，单位字节；目录存储大小为 0。 |
| `--after N` / `--before N` | 无；有符号 int64 | 修改时间的包含上下界，单位 Unix 秒，允许负值。 |
| `--sort name|path|size|mtime` | name | 主排序字段，相同值再按完整路径与节点 ID 稳定排序。 |
| `--desc` | 关闭 | 主排序字段降序。 |
| `--limit N` | 100；1..10000 | 本页最多条数。 |
| `--offset N` | 0；0..1000000 | 跳过前 N 条。 |
| `--show-ids` | 关闭 | TSV 前面增加节点 ID 与父节点 ID，JSON 始终带 ID。 |

大小或时间下界不能大于上界。根/节点 ID 必须存在；不匹配的范围组合返回空结果。`%`、`_`、引号和反斜杠都按字面处理，ASCII 英文大小写不敏感，其他 Unicode 字符按原字符匹配。任意子串查询仍使用 SQLite `LIKE '%text%'`，不能保证普通名称索引会加速这种查询。

默认搜索保持旧五列 TSV：

```text
TYPE    SIZE    MODIFIED_UNIX    NAME    PATH
```

实际分隔符为 Tab；`--show-ids` 增加 `ID PARENT_ID` 两列。名称和路径的控制字节编码为 `\xHH`，不破坏行结构；Windows 规范路径可能带 `\\?\` 前缀。

## 获取 ID 和浏览目录树

根 ID 与根节点 ID 是不同字段，不能互换。先读取根列表，后续根管理命令用 `id`，目录浏览用 `node_id` 或搜索结果的 `id`。

```powershell
$roots = & $app roots --db $db --format json | ConvertFrom-Json
$rootId = $roots[0].id
$nodeId = $roots[0].node_id

& $app info $nodeId --db $db --format json
& $app children $nodeId --db $db --limit 20 --offset 20
& $app ancestors $nodeId --db $db
& $app tree $nodeId --db $db --depth 2 --limit 1000
& $app du $nodeId --db $db --format json
```

| 命令 | 结果 |
| --- | --- |
| `info <node-id>` | 指定节点的持久化元数据，JSON 是单个节点对象。 |
| `children <node-id>` | 直接子项，按名称和路径稳定排序；limit 默认 100、1..10000，offset 默认 0、0..1000000。 |
| `ancestors <node-id>` | 从根到自身的祖先链，包含自身；检测缺失父节点和父链循环。 |
| `tree <node-id>` | 包含自身的有限树；depth 默认 3、0..1000，limit 默认 1000、1..10000，深度 0 仅显示自身。 |
| `du <node-id>` | 包含自身的子树文件数、目录数及文件字节，目录节点本身的 size 不作为子树大小。 |

这些命令只查询索引，不要求磁盘文件仍存在。TSV 默认带 `ID PARENT_ID`，树表再增加 `DEPTH`。JSON 的 children/ancestors 是节点数组，tree 是增加 `depth` 字段的节点数组，du 是 `{files,directories,bytes}` 对象。

## 根管理与健康检查

```powershell
& $app roots --db $db
& $app stats --db $db --format json
& $app check --db $db --format json

# 只忘记这个根的索引，磁盘文件和目录保留。
& $app forget $rootId --db $db --format json
```

`roots` 按路径稳定排序。TSV 列为 `ROOT_ID NODE_ID FILES DIRECTORIES BYTES SCANNED_UNIX PATH`，JSON 为数组，每项字段是 `id/path/scanned_at/node_id/files/directories/bytes`。目录数包含扫描根，字节仅统计文件，scanned_at 为上次成功扫描的 Unix 秒。

`stats` 输出全索引 `{roots,files,directories,bytes}`；空格式库各项为 0。统计超出 int64 范围时明确报错。

`check` 只读检查 SQLite 完整性、外键、根的节点身份/路径/类型、跨根父链、文件充当父节点、断链与循环，不自动修复。JSON 为 `{"ok":true,"issues":[]}`；检测异常时 ok 为 false，退出码为 **1**。不兼容格式或无法读取的损坏库也会失败。

`forget` 在一个事务中删除所选根全部节点和根记录。失败则完整回滚；根 ID 必须为正且仍注册，重复 forget 会明确拒绝。JSON 成功结果为 `{"forgotten_root":ID}`。这一命令不删除、移动或重命名磁盘文件。

## Refresh 与 Watch

```powershell
# 对已经注册的根做一次增量校准，不接受 --incremental 开关。
& $app refresh $rootId --db $db --progress --profile --format json

# Windows 事件监听；Ctrl+C 结束，默认不限轮次。
& $app watch $rootId --db $db --interval-ms 1000 --progress

# 强制轮询，初次校准也计一轮，完成两轮后退出。
& $app watch $rootId --db $db --poll --interval-ms 1000 --updates 2 --format json

# 限时运行，不需要为了结束批处理而发送控制台按键。
& $app watch $rootId --db $db --timeout-ms 60000 --format json
```

`refresh` 从注册根列表找路径，并在写事务内核对根 ID。`watch` 同样要求已注册根，不会自动重新添加被 forget 的根。空闲时每 250 ms 或每次校准前检查根自身的路径和文件系统身份；根自身改名、替换、消失或 Windows 大小写路径变化会使监听失败退出，已提交索引保留。恢复原路径、同路径替换或仅改变大小写后，可先 refresh 校准再重新启动 watch；移动到其他路径则需要 scan 新路径，并使用 roots 返回的对应注册根 ID。

Windows `watch` 先建立 `ReadDirectoryChangesW` 变化捕获，再执行初次增量校准；之后把事件合并成标脏状态，按间隔触发完整目录元数据校准。通知缓冲溢出也触发校准，校准期间发生的新变化留给下一轮。非 Windows、原生通知不支持或 `--poll` 时使用轮询。监听不是 MFT/USN，也不是按单个文件事件直接更新数据库；每次校准仍遍历整个根。

`--interval-ms` 默认 1000，允许 10..3600000，表示校准之间的最小间隔。`--updates` 默认 0（无限），允许 0..1000000；初次校准计一轮。事件模式下没有变化就不会产生后续轮次，因此有限轮次不等于固定运行时长；需要时间上限时用 `--timeout-ms`。

每轮单独事务，成功提交后才输出结果。JSON 格式是 **NDJSON**：每个已提交更新对应一行扫描 JSON 对象，无外层数组；Ctrl+C/超时退出仍保留此前已输出的成功轮次。进度和 `Changes`/`Profile`/`Watch` 诊断在 stderr，不混入 JSON 行。

## JSON 与退出码

搜索和节点查询共用这些节点字段：

```json
{"id":42,"parent_id":1,"name":"report.txt","path":"D:\\Documents\\report.txt","is_directory":false,"size":5,"modified_at":1704067200}
```

JSON 文本正确转义引号、反斜杠和控制字符，非 ASCII 文本保持 UTF-8；数字为十进制 int64。使用 JavaScript 的脚本在 ID/大小超过安全整数范围时需要能保留大整数的 JSON 解析方式。

| 退出码 | 含义 |
| --- | --- |
| 0 | 命令成功，包含空搜索结果。 |
| 1 | 文件系统、数据库等操作失败，或健康检查发现异常。 |
| 2 | 参数、ID、选项范围或扫描范围非法。 |
| 130 | Ctrl+C/Ctrl+Break 或批处理超时请求取消。 |

stdout 用于数据或默认扫描概要；stderr 用于错误、结果数量、进度、变化和耗时。脚本应同时检查退出码：NDJSON 中已有成功结果不意味着 watch 最终退出码为 0。
