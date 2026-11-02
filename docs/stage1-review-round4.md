# Stage 1 第四轮复核报告

本文件保留第四轮历史记录；最新结论见 [第五轮复核](stage1-review-round5.md)。

日期：2026-11-02。结论：已有明显进展，但尚未达到约定的 90% 收尾接手条件；由执行 AI 继续关闭以下核心问题。无需重做已通过的模块，也不要进入 Stage 2。

## 1. 已关闭项与本机验证

本轮在沙箱外运行现有 scripts/run_test.ps1，退出码 0，确认：

- COM 类工厂/TIP 基础生命周期测试通过。
- 候选窗口未使用即销毁、重复销毁、两个实例交错销毁的计数测试通过。上一轮计数变负的具体问题已修复，不再列为阻断项。
- 协议尺寸由二进制报告 REQUEST_SIZE=32、RESPONSE_SIZE=612；完整全零、语义空载荷、短帧、身份不匹配等反例均被拒绝。
- 服务不回复场景本次约 89ms 退出；测试守护自检约 805ms 完成。
- 正常 IPC 测试退出 0；源码已增加两个独立客户端的工作线程测试。独立客户端、响应身份匹配修复予以认可，不再按上一轮的进程单例问题重复要求整改。
- 检查服务端源码中的真实用户 SID 获取及 PIPE_REJECT_REMOTE_CLIENTS 修改。

注意：候选窗口测试里的“两个 concurrent windows”实际在同一个线程依次 Show/Destroy，证明多实例交错生命周期，不证明窗口类注册的跨线程并发初始化。测试输出的中文在当前重定向环境仍有截断，退出码与源码检查是主要依据。

本轮没有重新构建主工程，测试针对磁盘已有 Release 产物，静态复核针对当前源码；未做系统注册、真实宿主上屏，也未修改产品源码。

## 2. P1：限时等待取消后，仍没有完成确认与独立 I/O 生命周期

位置：src/tsf_client/ipc_client.cpp 的两个超时分支；ipc_client.h 的成员 OVERLAPPED/缓冲区。

当前流程是：CancelIoEx → 等事件 10ms → GetOverlappedResult(..., FALSE) → Disconnect → 返回。

代码没有检查等待是否完成，也没有检查 GetOverlappedResult 是否返回 ERROR_IO_INCOMPLETE。只要操作仍未完成，下次请求就可能重置并复用同一 OVERLAPPED、事件和缓冲区；TIP 析构也可能关闭事件并释放内存。成员变量仅能延长到客户端析构，不能保证延长到 I/O 完成。

整改说明中“CloseHandle 立刻撕毁 I/O 并解绑”没有可依赖的完成确认语义；当前代码本身也没有证明这点。不能将关闭句柄当作可立即复用内存的依据。

**要求：** 每个在途操作拥有独立的 OVERLAPPED、缓冲区和事件，超时后转入取消/回收状态，在确知完成前不得复用或析构；宿主线程到期可以返回，但后台回收仍必须持有操作对象与 DLL 生命周期。无需无限等待，也不能到期就直接放弃所有权。

**验收：** 注入“取消超过 10ms 才完成”的 I/O 替身，验证超时返回后立即再次发送/析构 TIP 时旧操作对象仍有效，直到完成才释放。当前普通管道挂起反例未覆盖取消延迟。

参考：[微软取消 I/O 指南](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)，异步操作实际完成后才能复用其 OVERLAPPED。

## 3. P1：上下文切换和失焦仍只释放引用，没有结束宿主组合串

位置：src/tsf_client/text_input_processor.cpp 的 OnKeyDown 上下文切换、OnSetFocus、Deactivate，以及 SetComposition。

新增 m_pCompositionContext 和 GetComposition(context) 有助于避免直接混用 Range。但切换时调用 SetComposition(nullptr, nullptr) 只执行 Release；它不等同于在所属 context 的有效编辑会话中执行 EndComposition 或取消文本。

失焦路径还把服务端 buffer 清空，而宿主旧组合可能仍存在。再次进入输入框后会缺少原组合的跟踪状态，可能重复创建组合或留下旧编码。也不能假设 Windows 为每个输入框单独创建 TIP；必须处理同一实例对应多个 context 的情况。

**要求：** 明确选择提交或取消策略，在旧 context 合法编辑会话中处理组合结束，结束后再释放跟踪状态；context 销毁/文档栈变化也有清理路径。失败时保留必要状态，不能仅丢弃指针。

**验收：** 同一个 TIP 中 A 框输入 a → 切到 B 框输入 b → 回到 A；再覆盖失焦、停用、context 销毁，检查旧组合确实终止且服务端状态一致。至少用 TSF 替身验证调用和结果，再做真实宿主联调。

参考：[EndComposition 文档](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcomposition-endcomposition)：结束组合和释放对象是不同操作，结束成功会移除 GUID_PROP_COMPOSING。

## 4. P1：取消失败仍虚报成功；SetText 成功不等于整个状态已恢复

位置：src/tsf_client/edit_session.h 的空组合分支和提交分支。

- 空组合分支仍忽略清空文本的 SetText 以及 EndComposition 的返回值，无论是否清空成功都丢弃跟踪对象并最终返回 S_OK。Esc 可能被吞掉而原编码仍留在宿主，服务端却已清空。
- 提交分支在 SetText 成功后忽略 EndComposition 失败，也直接清除跟踪。这会留下“文字已修改，但宿主组合尚未正常结束”的情况。
- SetText 成功后不回滚服务端是合理方向，但应记录文本已修改，并处理选择/结束操作失败；不能用统一返回 S_OK 隐去后续失败。

**要求：** 明确记录未修改、已修改待收尾、完成三种结果，决定按键消费、服务端确认及恢复方式。取消失败不能走成功路径；已修改后的失败不能简单放行原始键或丢失组合状态。

**验收：** 注入取消 SetText 失败、提交 SetText 成功后 EndComposition 失败、SetSelection 失败；同时断言宿主文字、组合是否仍存活、服务端 buffer 与 pfEaten。现有纯 IPC 测试不能覆盖这些结果。

## 5. P2：StartComposition 返回引用未配对释放

位置：src/tsf_client/edit_session.h 的 StartComposition → SetComposition；text_input_processor.cpp 的 SetComposition。

StartComposition 返回一个持有引用的 pComp；SetComposition 又执行 AddRef，但调用处未释放原始返回引用。后续清空成员仅释放一份，原始引用泄漏。EndComposition 本身不负责释放该对象。实际组合生命周期没有被当前“创建/释放一个未激活 TIP”的 COM 测试覆盖。

**要求：** 使用明确的拥有型 COM 智能指针或一致的 transfer/AddRef 约定，配对释放 StartComposition 返回引用；避免旧对象 Release 后再 AddRef 同一个裸指针的自赋值风险。

**验收：** 用可计数的 ITfComposition 替身或真实组合循环验证，反复开始/提交/取消后对象引用归零，DLL 可按预期卸载。

## 6. P2：管道忙时重试绕过总预算（新增实际反例）

EnsureConnected 在 ERROR_PIPE_BUSY 时最多进行五次 WaitNamedPipeW(..., 50)，不接收剩余预算。即使 SendRequest 提前计算 deadline，也只有连接循环结束后才检查，不能限制连接阶段总等待。

本轮创建一个仅单实例且已被占用的同名管道，再启动现有测试器，实测：

```text
PIPE_BUSY_ELAPSED_MS=738 EXIT=1
[TEST FAILED] Ping SendRequest failed or timed out at line 98
```

738ms 包含测试进程启动等开销，不应当作 SendRequest 的纯函数耗时；源码中的五次固定等待已足以证明没有遵守调用方的 80ms 总预算。

复现脚本：`C:\Users\chenl\.codex\visualizations\2026\11\02\01a19a4c-2ed5-76c0-b225-9a55e54e1ab5\stage1-pipe-busy-probe.ps1`。脚本不注册输入法，已有服务运行时拒绝执行，结束后释放自己创建的管道和进程。

**要求与验收：** 把 deadline 传入连接逻辑，每轮按剩余预算等待；对已占用管道直接测量 SendRequest 的总耗时，而非仅测读取挂起。

## 7. 交付要求

请关闭以上三个 P1，并补齐实际组合生命周期的失败注入测试；同时修正引用配对和连接预算。不要新增输入引擎功能。下一次提供源码版本/哈希、对应构建产物与上述针对性结果，避免只重复整套 PASS 日志。

本报告中“管道忙的进程级耗时”为本机动态结果；取消延迟、宿主组合失败和引用泄漏为当前源码发现，尚未在真实宿主中复现。不能据此声称宿主已经崩溃，也不能据现有测试通过认定这些问题已关闭。

这些问题关闭后再评估是否达到 90% 并由复核方接手安装与跨宿主联调。
