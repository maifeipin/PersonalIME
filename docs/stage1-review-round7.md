# Stage 1 第七轮复核报告

本文件保留第七轮历史记录；最新结论见 [第八轮复核](stage1-review-round8.md)。

日期：2026-11-02。结论：暂不签字验收，仍由执行 AI 完成；尚未达到约定的 90% 收尾条件。主要原因是上下文失败流程仍未闭环，新增关键测试未触达目标分支，孤儿队列新增了并发回收风险。

## 本轮实际验证

- 本机运行 scripts/run_test.ps1，五阶段通过，退出码 0；组合替身目标打印 12 项通过。
- 服务不回复约 104ms 返回；管道忙时内部耗时约 62ms；测试脚本自检约 806ms。
- verify_hashes.ps1 输出与本轮交付说明中的 11 个 SHA256 一致。
- RecoveryBlocked 已加入并被 OnKeyDown 按未应用请求处理；上一轮持续结束失败还返回 PartiallyModified 的具体问题已改正。
- 孤儿操作采用预分配侵入式节点，并在 CreateThread 失败时保留模块和 COM 引用，方向正确，不再按旧静态 vector 析构问题重复要求整改。
- 本轮未重新构建主工程、未修改产品源码、未注册输入法；真实宿主输入尚未验收。

## 1. P1：关键测试存在未触达目标分支的“通过”

### 1.1 上下文 pending 测试实际上没有排入任何 pending

位置：tests/test_composition_lifecycle.cpp:582–615；text_input_processor.cpp:54。

TestMultiContextPendingTerminationTracking 直接构造 TIP，没有 Activate，m_tfClientId 保持 TF_CLIENTID_NULL。TerminateCompositionOnHost 因此提前返回 true，不调用 RequestEditSession，也不产生 m_pendingTerminations。

随后测试手动用 SetComposition 把 A 换为 B，再手动调用 A 的 OnCompositionTerminated；只能证明一个不匹配的通知没有清掉 B，不能证明异步终止、pending 所有权或结果同步。

### 1.2 回收线程失败测试在连接阶段就返回

位置：tests/test_composition_lifecycle.cpp:619–643。

测试打开故障注入开关后连接不存在的管道。EnsureConnected 失败，根本没有发起 I/O、取消、DispatchBackgroundCleanup 或 CreateThread；GetOrphanCount 也没有在请求后断言增加。

故即使完全删掉 CreateThread 失败处理，这个测试依然会通过，不能作为调度失败资源保活证据。

### 1.3 连续输入测试仍未检查真实按键与服务端状态

TestRound6PersistentRecoverySequence 只直接调用 CEditSession，未走 OnKeyDown 的 pfEaten、Rollback 与服务端 buffer；打印“zero false keystroke consumption”超出了断言范围。

**整改与验收：** 激活 TIP 的测试替身，注入真实的同步拒绝/异步排队结果，断言 pending 数量与最终完成状态；建立已连接但拒绝响应的测试管道，再触发取消与 CreateThread 失败，断言入队、保活和完成后回收；连续键场景断言 pfEaten、宿主文档和服务端状态。增加分支命中计数，命中为零时测试必须失败。

## 2. P1：同步终止失败仍会被新 context 覆盖；异步失败没有完成结果通道

位置：text_input_processor.cpp:50–98、260–272、158–164。

### 同步执行失败

仅 TF_E_SYNCHRONOUS 路径把旧对象转入 pending。若旧结束操作已执行但 SetText/EndComposition 返回 E_FAIL，TerminateCompositionOnHost 返回 false，旧对象仍在主槽。OnKeyDown 仍更新活动 context 并继续创建 B 的新组合，SetComposition 将覆盖 A；这一第六轮问题仍存在。

### 异步调度与执行失败

异步路径先把主槽解绑，再调用第二次 RequestEditSession。若请求失败，pending 留下但没有撤销/重试路径；若请求被接受后 DoEditSession 执行失败，也没有明确回传完成结果的通知，不能依赖“终止回调必定发生”。

pending 只保存 contextId、composition 和 isCommitted，没有持有所属 ITfContext 与操作代数。代码在第二次 RequestEditSession 之前释放 m_pCompositionContext，也没有为 pContextToTerm 单独取得引用，存在对借用指针生命周期的依赖。

Deactivate 会 Reset 全部 pending 并释放它们，不检查对应宿主终止是否已经完成，仍可能在结束失败/尚未执行时丢失跟踪。

**整改与验收：** 同步失败和异步待执行都必须保留操作所有权；不能只有某一个 HRESULT 分支得到保护。保存 context 的强引用、组合身份、操作代数与完成状态，异步会话主动回传结果。只有对应宿主收尾成功后才同步服务端并释放跟踪；Deactivate 同样遵守此约束。

至少验证 A 结束同步 E_FAIL 后进入 B、异步请求立即失败、异步执行稍后失败、Deactivate 发生于排队期间、旧通知迟到五条路径，并断言 pending 最终不会永久悬挂或提前丢弃。

## 3. P1：孤儿队列 CAS 重试会遍历/修改不再属于当前线程的链表

位置：src/tsf_client/ipc_client.cpp:121–129。

DrainOrphanQueue 的重新挂回逻辑：

```text
每次 CAS 重试都从 remainingHead 重新查找 tail
tail->next = oldHead
CAS(globalHead, oldHead, remainingHead)
```

第一次 CAS 失败后，remainingHead 的尾部已经链接了先前读到的全局 oldHead。下一轮从 remainingHead 遍历到空，会走入该共享链表；另一个线程可能已经 exchange 并释放这些节点，造成读已释放内存，或把更新写入其他线程拥有的节点。

SendRequest 每次都会调用 DrainOrphanQueue，不同 TIP 线程可能同时进入；当前“两个客户端并发”测试没有产生孤儿节点，因此覆盖不到这个回收竞争。

GetOrphanCount 也直接遍历共享节点，没有防止并发 Drain 释放节点的保护，不适合作为并发过程中的安全观察接口。

**整改：** 保留当前线程私有链表的固定 tail，CAS 每次重试只改私有 tail 的 next，不重新遍历已链接的全局链表；或者采用经验证的所有权/回收实现。观察接口也需避免遍历可能被释放的节点。

**验收：** 至少两个线程同时生产、摘取和挂回未完成孤儿节点，强制 CAS 重试；延迟部分完成信号，检查无节点重复、环、丢失、悬空访问，最终句柄/模块/COM 引用均恢复。不要只用没有真实在途 I/O 的连接失败来替代。

## 下一次交付要求

请先把关键测试改成确实进入目标路径，并关闭同步终止失败、异步完成结果、孤儿队列所有权问题。保持已通过的编码保护与基础 IPC 功能。

每项测试应提供“目标分支命中次数”和最终状态断言；不再仅凭测试名称或 PASS 输出声称对应问题闭环。当前报告的分支未触达及并发风险来自源码检查，尚未在真实宿主中触发；本轮套件通过这一事实予以保留。
