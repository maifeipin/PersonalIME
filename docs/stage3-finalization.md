# Stage 3 代码与自动化收尾交接

日期：2026-11-05。基线为 `feature/stage3` 的 `c4fc39d`，本次修复保存在其上的未提交工作区。

**结论：本轮已达到约定的局部收尾范围，复核方直接完成两处修补，并通过一次完整统一验收。Stage 3 的代码与自动化验收可交接；真实宿主 GUI 验收仍待完成。Stage 4 WPF 尚未实现。**

## 1. 本次修复

### 回执刷盘失败后的重试

原 Append 在读取到相同完整 R1 帧后直接返回成功。第一次 `_commit` 失败时，该帧仍可能可读，读取成功不能证明已经完成物理提交。故障探针得到：

```text
FIRST_APPEND=0 FAILED_SYNC_RETRY=1
SYNC_CALLS_BEFORE_RECOVERY=1 RECOVERED=1 TOTAL_SYNC_CALLS=1
```

第二次和恢复后的调用都跳过了刷盘，错误地宣称耐久性成功。

现对相同事件、所有者与动作的重试重新追加有效帧并执行刷盘，成功后才返回 true；恢复视图继续按事件 ID 去重。ACK 终态仍覆盖后续重复结果。新增 `test_receipt_durability.exe` 用仅限测试翻译单元的 `_commit` 替身验证：连续两次刷盘故障必须都失败，故障恢复后第三次真正调用刷盘并成功，恢复集合始终只有一条事件。

### 无状态更新响应保留候选快照

原 TIP 在任何未带候选的 handled 响应后清零当前上下文候选版本。首页 PageUp、末页 PageDown 或不修改组合的控制操作会返回这种响应，导致下次选词使用 0版本而被服务拒绝。

现仅在服务明确报告组合串已清空时清零；没有状态更新的响应保留原有效快照。新增 `test_tip_candidate_snapshot.exe` 通过真实 OnKeyDown/IPC 路径验证首页边界 PageUp，当前版本 7与另一上下文版本 9都保持不变。测试使用可超时的独立管道和确定性键盘状态替身，不注入系统输入、不注册输入法。

新增测试的初稿曾因测试访问宏使 IPC 静态测试管道存储不一致而未命中请求；已修正头文件包含顺序，并补齐服务端连接/读写超时。最终测试要求 handled=true，避免未触达路径的假通过。

## 2. 完整验证

实际命令：

```powershell
.\scripts\verify_stage3.ps1
```

最终退出码 **0**。同一次执行包含：

| 验证 | 实测结果 |
| --- | --- |
| Release x64 全工程构建 | 通过 |
| test_ime_core.exe | 14/14 组通过 |
| test_stage3_integration.exe | 15/15 项通过 |
| test_receipt_journal.exe | 通过，多进程/断尾/ACK/迁移来源/锁与存储故障 |
| test_receipt_handover.exe | 通过，独立结果所有者及 TIP 销毁后的交接 |
| test_receipt_durability.exe | 通过，物理提交失败后的真实重试屏障 |
| test_tip_candidate_snapshot.exe | 通过，真实 TIP 保留无更新响应前的有效快照 |
| scripts/run_test.ps1 | Stage 1 全套 5组通过 |

最终统一执行的隔离环境：

`C:\Projects\AITools\PersonalIME\out\stage3-verification-b3dbdc2e78614fc98b6e57467c290a28`

脚本为服务数据库和 TEMP/TMP 指定独立路径，并在结束时恢复环境变量。不连接已有用户服务；测试服务由测试入口清理。本轮没有注册输入法或访问真实个人词库。

源码和关键产物哈希见 `stage3-finalization-hashes.json`。

## 3. 工程交接

- 新入口 `scripts/verify_stage3.ps1` 避免 `build.ps1 -RunTests` 漏跑独立回执测试和 Stage 1 回归。
- README 已更新到 Stage 3 实际状态。
- `.gitignore` 已允许正式 Stage 3 设计、回执契约和本交接说明纳入版本管理。它们此前在本地存在，但未随 c4fc39d 提交。
- `docs/stage3-receipt-wal.md` 为当前回执契约。`stage3-design.md` 已标明 Round 3–10 的压缩/紧急追加方案属于历史记录。
- 本次未提交、未推送、未合并。`main` 保持原基线；执行 AI 接手时先保留并检查工作区修改，不能重置覆盖。

## 4. 验收边界与下一阶段

| 真实宿主 | 本轮状态 |
| --- | --- |
| 记事本实际注册打字 | 待验收 |
| Edge/Chrome 实际注册打字 | 待验收 |
| VS Code 实际注册打字 | 待验收 |

真实 TIP 方法及服务进程的自动化测试，不能代替以上 GUI 结果。下一步验证五笔/拼音切换、候选翻页选择、取消与快捷键、特殊字符/Emoji/多行短语、全半角/标点、服务中断恢复；通过后再推进 WPF 词库与设置管理器。

本轮接受 `stage3-receipt-wal.md` 已公开的保证边界：宿主已修改但结果尚未提交且整个进程崩溃的窗口不能自动推断；永久存储/线程创建故障有明确降级；追加日志增长与恢复扫描成本未实施压缩治理。不要恢复“所有崩溃窗口100%零丢失”等超出证据的承诺。
