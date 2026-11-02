# Stage 1 第六轮复核报告

本文件保留第六轮历史记录；最新结论见 [第七轮复核](stage1-review-round7.md)。

日期：2026-11-02。结论：尚未达到约定的 90% 收尾接手条件，继续由执行 AI 关闭以下三项失败路径。上一轮已改善的正常路径予以认可，不要求重做。

## 1. 本轮验证与已关闭部分

本机执行 scripts/run_test.ps1 和 scripts/verify_hashes.ps1：

- 五组测试退出码 0，新增的持久 Range、CommitPendingEnd 文本保护测试通过。
- 服务不回复约 94ms 返回，管道忙时内部测量约 63ms，测试脚本自检约 815ms。
- 清单中的 11 个源码/二进制 SHA256 与本机输出一致，上一轮哈希不一致的问题已关闭。
- CommitPendingEnd 状态以及“已提交文本终止时不清空文本”的修复有效。结束失败后下一次结束成功的单次恢复路径不再覆盖旧提交词。
- 正常回收线程使用加载器引用及 FreeLibraryAndExitThread，上一轮正常线程退出尾部的卸载竞态已改善。

本轮运行已有 Release 产物，未重新构建主工程、未修改产品源码、未注册系统输入法。真实宿主验证尚未执行。

## 2. P1：持续结束失败时，新键没有执行却被当作“已修改”消费

位置：src/tsf_client/edit_session.h:142–154；src/tsf_client/text_input_processor.cpp:272 附近；tests/test_composition_lifecycle.cpp 的 TestPersistentRangeCommitFailureStillFailing。

在恢复 CommitPendingEnd 的入口，如果旧 EndComposition 再次失败，当前请求还没有执行任何 SetText，但返回 PartiallyModified。OnKeyDown 据此消费本次按键，并禁止回滚服务端。

这里把“上一次提交修改了文本”与“这一次按键修改了文本”混为一谈。旧提交词虽然保护住了，新按键的宿主执行与服务端状态却不再一致。

可沿当前源码得到以下序列：

1. 第一次提交“测试”已写入，但结束组合失败，进入 CommitPendingEnd。
2. 用户输入 b：服务端缓存变为 b；宿主恢复仍失败，未写入 b，但键被消费。
3. 用户按空格：服务端产生第二次提交并清空 b；宿主恢复仍失败，第二次提交未执行，空格也被消费。
4. 后续结束恢复后，服务端已没有第二次提交记录，该次输入丢失。

现有持续失败测试只断言旧“测试”未被覆盖，并把 PartiallyModified 当作正确结果；没有断言新键、下一次选词和服务端缓存是否一致，因此测试通过不代表此路径闭环。

**整改：** 对当前请求提供“恢复被阻塞且当前请求未应用”的独立结果，或在向服务端发送当前 KeyDown 前完成旧提交收尾。选择明确的有限重试、按键排队或回滚/旁路策略，不能直接消费尚未执行的新请求。回滚不得撤销已经写入的旧提交词。

**验收：** 将上述 b → 空格 → 恢复串成一个有持久宿主文档和服务端状态的测试，验证第二次输入没有静默丢失；同时覆盖 b → c、Esc 和失焦。这里是源码路径发现，尚未用真实宿主动态复现。

## 3. P1：旧上下文终止未完成时仍允许新组合覆盖唯一跟踪槽

位置：src/tsf_client/text_input_processor.cpp:225–237、TerminateCompositionOnHost、Deactivate；src/tsf_client/edit_session.h:324。

本轮只在终止成功且 m_pComposition 为空时发送 Reset，避免了部分过早清空，这一点有效。但 OnKeyDown 在终止返回失败或异步排队后，仍更新 m_activeContextId 并继续处理新 context。

TIP 仍只有一个 composition 跟踪槽。新 context 的 StartComposition 成功后，SetComposition(pNewComp, newContext) 会释放旧跟踪指针。旧组合尚未结束，就失去恢复入口；旧通知随后因不等于当前 composition 而被忽略，旧服务端状态也可能一直残留。

另外，Deactivate 前部虽有条件地 Reset，后部仍无条件调用 ResetContext(m_sessionId, m_activeContextId) 并断开管道，与“终止确认后再同步”的说明不一致。

异步终止会话目前没有自己的完成通知与身份令牌，仅依靠 OnCompositionTerminated。不能把“已提交异步请求”视为终止结果已经同步。

**整改：** 在“唯一活动组合”设计下，旧终止未确认时不得建立会覆盖其跟踪的新组合；或者将待终止操作及其上下文单独保存到完成为止。选择其中一种并明确行为。异步会话完成时主动返回成功/失败及原组合身份，处理同步重入回调和迟到结果。Deactivate 应遵守同一规则。

**验收：** 激活同一 TIP，A 框输入 → 旧终止同步失败/异步待执行 → B 框输入 → A 的迟到成功/失败，验证每个组合均有所有者和最终结果，A 不遗留，B 不被误清空。测试必须调用真实切换入口；直接运行 CTerminateCompositionSession 并手动清除成员不能替代该验证。

## 4. P1：CreateThread 失败的孤儿队列仍不能保证在途 I/O 生命周期

位置：src/tsf_client/ipc_client.cpp:8–9、52–76。

正常回收线程已经持有加载器引用，并在销毁上下文后安全退出，此路径予以认可。剩余问题在调度失败分支：

- CreateThread 失败后先 DllRelease、FreeLibrary，撤销了模块保护。
- 操作对象放入 DLL 的静态 std::vector，DLL 卸载时该容器会析构，仍可能在 I/O 完成前释放缓冲区和事件。它并不是独立于模块卸载的保活队列。
- 删除 CleanupContext 时 pipeToClose 的所有权丢失，没有后台所有者继续回收该句柄。
- 该分支的 lock_guard/push_back 可能等待或分配失败；new CleanupContext 也在没有错误处理的路径上分配。资源耗尽时的兜底又依赖额外资源。
- GetModuleHandleExW 的失败结果未检查，不能无条件认为正常回收线程已取得有效模块保护。

**整改：** 在发起 I/O 之前准备好恢复所需的资源和所有权；调度失败时，操作对象、管道句柄、模块引用必须一起保留，直到完成。可以使用预建回收设施；若选择极端故障时有意保留资源到进程结束，必须保留模块本身并使用不会在 DLL 卸载时析构的所有者，同时清楚记录这一降级行为。不能仅把 shared_ptr 放进模块静态容器后释放模块保护。

**验收：** 注入 GetModuleHandleEx/CreateThread/回收上下文分配失败，且让取消延迟完成；释放全部 TIP 后触发卸载检查，证明在途内存、事件和句柄仍有有效所有者，宿主不无界等待。普通服务挂起测试不会触发 CreateThread 失败，当前没有该分支的实测证据。

## 5. 下一次交付范围

请只关闭上述三项：当前请求未应用时的按键/服务端同步、旧上下文未结束时的新输入、回收调度失败时的资源保活。为每项增加会检查最终状态的测试，保留已通过的现有套件和机器生成哈希。

本报告不认定正常路径无法运行，也不声称已在日常应用中观察到上述失败。问题属于源码中仍未闭环的状态与生命周期分支，因此尚不适合按“仅剩安装和界面联调”交接。
