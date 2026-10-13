# GUI 前核心与 CLI Implementation Plan

**Goal:** 提供命令行日常可用、可被 GUI 复用的搜索、目录、索引与维护服务。
**Architecture:** 沿用 schema 1 和 RAII SQLite。查询与管理分模块，扫描支持增量/回调，监听触发事务校准，CLI 组合接口。
**Tech Stack:** C++20、SQLite 3.53.4、CMake/CTest、Win32 文件变化 API。
**Spec:** docs/pre-gui-spec.md。

## 任务与文件所有权

- [x] 共享读事务：父 agent 创建 src/read_index.hpp、tests/read_index_tests.cpp；先验证缺少接口导致编译失败，再实现并验证只读/错误格式行为。
- [x] 查询模块：独立 agent 创建 include/mini/query.hpp、src/query.cpp、tests/query_tests.cpp。接口 query(database, QueryOptions)、get_node(database,id)、children(database,id,limit,offset)、ancestors(database,id)、tree(database,id,max_depth,limit)、summarize(database,id)。先写搜索/过滤/分页/树关系/边界测试，验证 RED，再实现。
- [x] 索引管理：独立 agent 创建 include/mini/catalog.hpp、src/catalog.cpp、tests/catalog_tests.cpp。接口 list_roots(database)、index_stats(database)、forget_root(database,root_id)、check_index(database)。先验证空/多根、文件保留、事务失败和树关系异常。
- [x] 扫描维护：独立 agent 仅修改 include/mini/index.hpp、src/index.cpp，创建 tests/maintenance_tests.cpp。增加进度/取消、增量模式、expected_root_id；旧默认重建及 search 保持兼容。先验证保留 ID、删除旧行和取消回滚，再实现。
- [x] 父 agent 集成 CLI、CMake、watch 模块与测试；先写 CLI 回归，在旧程序上观察失败，再实现。监听测试仅修改临时夹具。
- [x] 更新 README、使用手册、code-guide、基准与验收报告；所有新函数保留中文说明。
- [ ] 独立整分支审查，处理确认的问题，Debug/Release 和 Windows/Linux CI，提交推送并归档 v0.5.0（GUI 前核心版本）。

## 接口约定

QueryOptions 前三个成员为 text、extension、limit，随后为 MatchMode{substring,exact,prefix}、NodeKind{any,file,directory}、SortField{name,path,size,modified}、descending、offset、min_size/max_size/modified_after/modified_before（optional<int64>）、root_id、under_id、match_path。默认限制100，offset上限1000000，0仅在范围过滤表示不限。

TreeEntry{Node node; int depth;}；SubtreeStats{int64 files,directories,bytes;}。
CatalogRoot{int64 id; string path; int64 scanned_at,node_id,files,directories,bytes;}；IndexStats{int64 roots,files,directories,bytes;}；IndexHealth{bool ok; vector<string> issues;}。

ScanResult 新增 inserted、updated、removed、unchanged（节点行数量，含目录）；ScanProgress{ScanPhase phase; int64 files,directories,skipped;}，ScanPhase{preparing,scanning,committing}。ScanOptions 原首成员 db_cache_mib 保持，新增 incremental=false、expected_root_id=0、cancelled=function<bool()>、progress=function<void(const ScanProgress&)>、progress_interval_ms=1000（允许0供无节流调用）。ScanCancelled 为 runtime_error 派生。

所有 agent 不提交、不切分支、不推送、不构建共享 build/Release。查询/目录模块不得改 index.hpp/index.cpp，管理模块不得改其他模块，扫描模块不得改 CMake。独立验证使用 ignored build/pre-gui-<name>，父 agent 统一注册目标。

## 验证与决策记录

基线 ae62c2f，沿用当前工作目录新分支 feat/pre-gui-cli。用户自主授权下，由父 agent 执行设计和集成；文件所有权不重叠的模块按 dispatching-parallel-agents 并行，最后统一验证。

已完成共享读接口、查询、目录、增量扫描、监听和 CLI 整合。CLI 在管理模块完成后交由其 agent 编写，父 agent 同时完成监听与统一构建；文件所有权分阶段交接。独立审查确认并修复三个 P2 和 forget 缺失数据库问题，均有先失败后通过的回归证据。本机 Debug/Release 各32项通过；受控默认/增量扫描和查询结果见 pre-gui-validation.md。远程 CI 与版本归档等待提交后完成。
