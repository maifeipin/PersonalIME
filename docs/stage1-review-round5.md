# Stage 1 第五轮复核报告

本文件保留第五轮历史记录；最新结论见 [第六轮复核](stage1-review-round6.md)。

日期：2026-11-02。结论：仍未达到约定的 90% 收尾条件，由执行 AI 继续整改。当前剩余问题集中在失败后的连续输入、上下文终止结果同步及后台线程退出生命周期，不要求增加产品功能。

## 已认可与实际执行

- 本机执行 scripts/run_test.ps1，五组测试全部通过，退出码 0。服务挂起场景约 88ms 返回，管道忙时函数内部耗时约 62ms，测试脚本自检约 818ms。
- 独立堆 I/O 操作对象、独立客户端、deadline 传入连接重试、StartComposition 引用配对及 SetComposition 自赋值修复方向正确。
- 组合失败分类测试已新增，取消失败和部分修改不再一律报告成功；这些改进不再重复列作上一轮原始问题。
- 另行编译运行一个复用现有 TSF 替身、但令同一 composition 的 GetRange 返回持久文本范围的反例程序。它直接编译当前产品源码，未修改主工程。
- 未重新构建主工程，现有套件运行的是交付 Release 产物；未注册输入法，未进行真实宿主上屏。

## P1-1：识别 PartiallyModified 后没有恢复状态，下一键可覆盖已提交词（已复现）

位置：src/tsf_client/text_input_processor.cpp:253 附近；src/tsf_client/edit_session.h 的提交失败和后续组合更新分支。

反例步骤：

1. 现有组合范围含编码 a。
2. 提交“测试”，SetText 成功，但注入 EndComposition 失败。
3. 当前代码报告 PartiallyModified，保留同一个 composition；外层仅消费按键并隐藏候选，没有将 TIP 标记为待恢复或执行收尾。
4. 服务端此前已按提交清空编码。用户下一键 b 对应新的组合更新，仍落到旧 composition 范围上执行 SetText(b)。

持久范围 TSF 替身的实际输出：

```text
COMMIT_FAILED=1 PARTIAL_RESULT=1 COMMITTED_TEXT_PRESENT=1 COMPOSITION_RETAINED=1
NEXT_KEY_SUCCEEDED=1 PREVIOUS_COMMIT_OVERWRITTEN=1
```

这证明“正确分类一次失败”没有闭环到“失败后的连续输入”。现有测试在检查 PartiallyModified 后手动清理对象，跳过了下一键，因此未捕获。

此外，当前 TerminateCompositionOnHost 的通用清空策略也可能在该场景下删除已经写入的提交文本；待收尾提交不能等同于待取消编码。

**整改要求：** 增加真正持久的待恢复状态，区分待结束的已提交文本与可取消的组合编码。提交已写入而结束失败时，保留文本并完成旧组合结束，再允许新组合写入；不能直接覆盖旧范围。恢复失败时明确消费/放行策略，不丢失已上屏文字。

**验收：** EndComposition 失败后继续输入、失焦、切换输入框、Esc、停用输入法，各自验证已提交“测试”仍保留，新的编码范围与旧范围分离。测试必须用持续存在的文档/Range，不能每次 GetRange 都返回新的空文本对象。

反例工程：`C:\Users\chenl\.codex\visualizations\2026\11\02\01a19a4c-2ed5-76c0-b225-9a55e54e1ab5\stage1-round5-probe`。probe.cpp 包含现有测试替身，覆盖 GetRange 以维护同一个范围，编译当前 text_input_processor.cpp、candidate_window.cpp、ipc_client.cpp。它是隔离替身复现，并非记事本真实输入结果。

## P1-2：终止失败/排队后仍清空服务端并继续切换，异步路径检查错误

位置：src/tsf_client/text_input_processor.cpp:42、172、215 附近。

- TerminateCompositionOnHost 仍检查 `hr == TF_E_SYNCHRONOUS`，但同步编辑会话不可建立时该值应从输出 hrSession 判断。真实路径可能是 hr=S_OK、hrSession=TF_E_SYNCHRONOUS。
- OnSetFocus、上下文切换与 Deactivate 忽略终止结果，随后发送 Reset；上下文切换还立即更新 m_activeContextId 并继续处理新输入，即使旧宿主组合尚未结束。
- 异步请求被接受与实际结束是两件事；当前没有带旧 context 身份和代数的完成结果回传。即使异步执行成功，也不能只依赖通用 OnCompositionTerminated 回调来推断是哪次收尾完成。
- OnCompositionTerminated 无论回调是否对应当前组合，都会隐藏候选并 Reset 当前 m_activeContextId。旧组合迟到通知可能误清空新 context 的状态。
- CTerminateCompositionSession 遇 GetRange 失败时仍继续 EndComposition，不能证明计划中的取消文本已清除。

**整改要求：** 将终止操作作为明确的状态转换：失败、待执行、已完成分别处理；只有对应旧上下文的宿主终止确认后才完成其服务端同步。回调必须按组合/上下文/操作代数匹配，不能重置新的活动上下文。正确检查 hr 与 hrSession 两个结果。

**验收：** 真实调用 OnKeyDown/OnSetFocus/TerminateCompositionOnHost 的测试覆盖 hr=S_OK 且 hrSession=TF_E_SYNCHRONOUS、异步排队后失败、旧回调晚于新 context 输入、GetRange 失败。当前 TestContextSwitchCompositionTermination 只直接执行 CTerminateCompositionSession，再手动 SetComposition(nullptr)；没有测试切换入口和结果同步，因此不能称整个上下文切换已通过。

依据：[RequestEditSession 文档](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession)。TF_S_ASYNC 是已安排异步会话，不是 DoEditSession 已成功完成。

## P1-3：后台回收线程在退出前释放 DLL 保护

位置：src/tsf_client/ipc_client.cpp:140–152、198–210 附近；globals.h 中 DllRelease 实现。

堆操作对象由回收线程持有，解决了上一版的立即复用问题；但线程在 lambda 内调用 DllRelease 后仍要退出 lambda、析构捕获的 shared_ptr，并经过 std::thread/静态 CRT 的线程包装代码。此时 g_cDllRef 可能为 0，DllCanUnloadNow 可以允许 COM 卸载 DLL，然而线程尚未离开该模块代码。

当前 DllAddRef 是自定义 COM 模块计数，不是 Windows 加载器的模块引用，不能保证在这段尾部执行期间模块不被卸载。

此外，创建 std::thread 失败时 catch 在宿主调用线程执行 WaitForSingleObject(INFINITE)，又恢复了此前要求消除的无界按键等待。

**整改要求：** 后台任务必须有覆盖整个线程退出的模块生命周期方案，例如正确持有加载器模块引用并以安全退出机制收尾，或使用明确管理、可停止且不会在 DllMain 内等待的工作线程。清理捕获对象之后仍不能留下可被卸载的代码执行尾部。回收调度失败也必须有不阻塞宿主且不释放在途内存的策略。

**验收：** 取消尚未完成时释放全部 TIP/工厂并并发请求 COM 卸载，随后释放取消完成信号；验证对象清理与线程退出期间模块仍安全。注入线程/调度创建失败，检查宿主按键仍有限时返回且在途操作保持有效。当前套件没有覆盖这些路径。

参考：[FreeLibraryAndExitThread 文档](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibraryandexitthread)，微软提供原子释放模块与退出线程的机制来避免执行线程和卸载之间的竞态；采用其他设计也必须达到同样的生命周期保证。

## 交付证据：源码 SHA256 与粘贴清单不匹配

本机逐项核验七个文件：四个 Release 二进制匹配粘贴清单，三个源码不匹配。此处不推断原因，也不能据此证明二进制与当前源码对应。

当前源码实际 SHA256：

```text
src/tsf_client/edit_session.h
899A8F903BEA38C26E3BA7765D8182BFB4A0FD368F71581A4A00915128ACD32D
src/tsf_client/ipc_client.cpp
A2FC0C1A11C12478DA5863C1D04BB1360C4E9DD73EFAFCA9DABA9B3A07130533
src/tsf_client/text_input_processor.cpp
068E7585846F2DCAC67A9156D6487F2848DE8160DDD279C7E998A0AAF257017B
```

完整期望值/实测值/匹配结果见 [机器生成的哈希核验](stage1-review-round5-hashes.json)。下次请由脚本直接生成源码与产物清单，并关联一次干净构建，避免手工转写。

## 下一次交付重点

只需围绕上述三项提供修复和有状态的连续场景验证；已关闭的计数、基础协议和连接预算问题不再重做。特别注意：测试不能只断言“进入了 PartiallyModified”，还必须断言下一次输入和焦点变化之后，文档、组合对象、服务端状态仍一致。

本报告中丢字路径为已编译运行的替身反例；上下文异步与线程卸载风险为源码发现，尚未在真实宿主动态复现。继续由执行 AI 关闭这些问题，再评估 90% 接手条件。
