# Stage 1 第八轮复核报告

日期：2026-11-02。结论：仍未签字验收，继续由执行 AI 完成最后一组 pending 生命周期问题。当前仍存在已复现的 COM 过度释放，以及失败 pending 缺少恢复出口，因此尚未达到约定的 90% 收尾接手条件。

## 已关闭的问题与本轮执行

- 运行现有 scripts/run_test.ps1，五阶段通过，退出码 0；12 项组合测试均报告通过。
- 新增测试已设置非空 client ID、注入异步请求和执行故障，并连接真实拒绝响应的测试管道；上一轮指出的两个“未触达目标分支”问题已修正。
- RecoveryBlocked 测试现已覆盖 OnKeyDown、pfEaten 与服务端 Rollback，予以认可。
- 队列重挂使用固定 remainingTail，GetOrphanCount 改为原子计数；上一轮对应 CAS 遍历问题已修正，不再重复列入整改。
- 服务挂起场景约 98ms 返回，管道忙内部耗时约 62ms，测试守护自检约 810ms。
- 本轮另行编译并运行一个直接包含当前源码和现有替身的 pending 引用探针。未修改产品源码，未重新构建主工程，未注册系统输入法。

## 1. P1：pending 在 TIP 析构时被重复 Release（已复现）

位置：src/tsf_client/text_input_processor.cpp 的析构函数；text_input_processor.h 的 PendingTermination 析构函数。

TIP 析构先遍历 m_pendingTerminations，手动调用 pending.pComp->Release()，随后 clear()。而 PendingTermination 已经是 RAII 所有者，它的析构函数还会 Release 同一个 pComp。

当 FailedSync/FailedAsync/FailedRequest 留在列表中直到 TIP 销毁，组合对象会被多释放一次。当前测试在 Deactivate 时先清除了失败项，未覆盖“仍持有失败 pending 直接析构 TIP”的路径。

本轮探针使用不在引用归零时立即删除自身的计数替身，避免刻意调用已释放内存。输入一次同步终止失败后，释放调用方引用，再析构 TIP，实际输出：

```text
TERMINATED=0 FAILED_PENDING=1
BEFORE_TIP_DESTRUCTOR_COMP_REFS=1 CONTEXT_REFS=2
AFTER_TIP_DESTRUCTOR_COMP_REFS=-1 CONTEXT_REFS=1
```

组合引用从 1 降到 -1，证明存在两次 Release；正常 COM 实现第一次即可能销毁对象，第二次便有释放后访问风险。本报告不声称已在真实应用中触发崩溃。

**整改：** PendingTermination 统一拥有自己的 COM 引用，不再由外层手工重复释放其字段；审查析构、clear、erase、复制/赋值、同步重入回调这几条释放路径。

**验收：** TIP 持有失败 pending 时直接释放、多个 pending 混合析构、Deactivate 后再析构、异步完成后再析构，均无过度释放，所有被持有对象引用回到预期值。

## 2. P2：显式传入所属 context 时，原主槽强引用未配对释放（同一探针复现）

位置：TerminateCompositionOnHost 中三处 `if (pContextOverride == nullptr) pContextToTerm->Release()`。

函数把主槽两个指针置空，相当于把原主槽持有的引用交给局部变量；随后 PendingTermination 构造又持有一份引用。pComp 的原主槽引用最后无条件释放，但 context 仅在未传 override 时释放。

当调用 `TerminateCompositionOnHost(context)` 且 context 正是所属 context，主槽原先持有的 context 引用并不会因为参数来自外部就消失，仍必须配对。探针中调用方引用和 pending 都清理之后，context 还残留 1，正是这一份泄漏。

**整改：** 显式区分借用参数与从主槽移交的拥有引用，采用统一强引用局部变量或明确 transfer 约定，避免按参数是否为空猜测引用归属。

**验收：** override 为空/等于所属 context 两条路径，在同步成功、同步失败、异步请求失败、异步执行完成后，context 引用均归零或回到调用方持有数。

探针工程与源码：`C:\Users\chenl\.codex\visualizations\2026\11\02\01a19a4c-2ed5-76c0-b225-9a55e54e1ab5\stage1-round8-probe`。probe.cpp 编译当前产品实现，调用真实 TerminateCompositionOnHost；替身只对 AddRef/Release 计数并延迟删除，不改变产品中的引用操作。

## 3. P1：记录 Failed* 状态后没有恢复出口，Deactivate 又把未结束组合当作终态释放

位置：OnTerminationSessionCompleted、OnTerminationSessionRequestFailed、TerminateCompositionOnHost、Deactivate。

本轮已解决“保存不了异步结果”，但保存 FailedSync/FailedAsync/FailedRequest 并不等于旧宿主组合已经结束。

当前这些失败记录只被保留和查询，没有针对其持有的 context/composition 的重试或恢复入口。主槽已经解绑，随后调用 TerminateCompositionOnHost 又只查主槽，不会重新调度失败 pending。旧组合因此可能一直存在。

Deactivate 则保留 QueuedAsync，却直接 erase 所有 Failed*。Failed* 表示这次尝试失败，而非宿主组合已结束；直接释放跟踪仍会留下未完成宿主组合。测试目前断言“状态变成 Failed*”，随后通过 Deactivate 清理记录，没有断言旧组合后来真正结束。

**整改：** 定义失败 pending 的后续策略：何时可重试、如何切回原 context、何时由宿主销毁确认可放弃、停用时如何收尾。持有 context/composition 的记录要有成功结束或宿主失效确认这个出口，不能只以状态为 Failed 就删除。排队期间返回同一 context 也要避免建立与旧组合重叠的新组合。

**验收：** 终止失败 → 故障解除 → 恢复/切回原框 → 停用，最终断言旧 EndComposition 成功或宿主销毁已确认、pending 清零、宿主文本和服务端状态一致、引用归零。异步请求失败和异步执行失败都要覆盖。

## 下一次交付范围

只处理 pending 所有权及失败恢复出口，保留本轮已通过的队列与测试分支改进。至少把本报告的析构反例和“失败后恢复至终态”加入套件；不要再用清空容器本身作为宿主完成清理的证明。

本轮过度释放和 context 残留引用已由隔离探针动态复现；失败 pending 缺少恢复出口为源码发现。真实宿主输入与卸载尚未验收。
