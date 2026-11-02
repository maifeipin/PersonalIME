# Stage 1 第三轮复核报告（供执行 AI 整改）

本文件保留第三轮历史记录；最新结论见 [第四轮复核](stage1-review-round4.md)。

日期：2026-11-02。范围：本轮 TSF、IPC、候选窗口及测试脚本修改。

**结论：未达到约定的 90% 收尾接手条件，由执行 AI 继续整改。** 主要缺口仍涉及并发所有权、宿主组合状态和失败恢复，不能归为仅剩安装联调。不要进入 Stage 2；先完成以下整改及对应验证。

## 1. 已认可的修复与证据范围

- Release 测试已使用 CHECK_TRUE，原 assert 被 NDEBUG 禁用的问题已修复。
- TIP/编辑会话增加 DLL 引用维护，现有类工厂/TIP 生命周期测试通过。
- 候选窗口改为 TIP 成员，修饰键和只读状态增加了入口过滤。
- IPC 改为 overlapped I/O，写读阶段共享 deadline；取消后等待完成，相比上一版立即释放栈对象有所改进。
- 测试脚本已改为超时先 Kill 后读取输出，并校验退出码和错误标记；新增自我超时检查，子 PowerShell 使用 -NoProfile。
- 本轮实际执行 scripts/run_test.ps1，退出码 0：COM、两个 IPC 反例、测试脚本自检、串行多 context 查询均报告通过。服务不回复场景约 100ms 退出，自检约 810ms 终止子进程。

这些测试结果有效，但其覆盖范围不足以证明真实宿主可用，也不足以证明下面的问题不存在。本轮未重新构建主工程，运行的是已有 Release 测试产物；另行构建了直接包含当前候选窗口源码的独立探针。未注册输入法，未做记事本/浏览器/VS Code 上屏验证，未修改产品源码。

## 2. P1：候选窗口类计数因重复销毁损坏（已动态复现）

位置：src/tsf_client/candidate_window.cpp:54、63；src/tsf_client/text_input_processor.cpp:24、77。

Destroy 无条件递减 s_cClassRef，未创建窗口时也递减；TIP 停用、TIP 析构和候选窗口成员析构还会重复调用。EnsureWindow 只有在递增结果等于 1 时才注册窗口类。

独立探针的实际输出：

```text
Initial class refs=0
Destroy on unused window: refs=-1
Repeated Destroy: refs=-2
After destructor: refs=-3
```

因此后续实例可能跳过类注册，造成候选窗口创建失败。当前 COM 测试只检查 DLL 引用，没有覆盖这一计数。

**整改：** Destroy 必须幂等；只有成功取得窗口类引用的对象才释放一次；处理注册失败、窗口创建失败及并发初始化。不要仅靠删掉某一个 Destroy 调用规避问题。

**验收：** 未使用即析构、连续两次 Destroy、两个 TIP 共存、反复启用/停用后重新显示候选框，均成功且计数最终回到零。

探针源码与 CMake 工程：`C:\Users\chenl\.codex\visualizations\2026\11\02\01a19a4c-2ed5-76c0-b225-9a55e54e1ab5\stage1-round3-probe`。探针不创建窗口、不修改系统注册，直接检查当前实现中的实际计数。

## 3. P1：IPC 仍为无并发保护的进程单例

位置：src/tsf_client/ipc_client.h:8、29；src/tsf_client/ipc_client.cpp:97、138。

所有 TIP 仍共享 IpcClient::Instance()。管道、事件、OVERLAPPED、发送和接收缓冲区均为共享成员，没有线程隔离或调度保护。两个宿主线程同时进入 SendRequest，可覆盖未完成操作的 OVERLAPPED/缓冲区、读取对方响应，或取消对方 I/O。把栈对象改成单例成员并没有解决所有权问题。

现有测试是在一个线程里串行发送 context 101/102，只验证服务端 map 的分桶，不验证客户端并发安全。

**整改：** 明确客户端和 I/O 操作的所属线程/实例，优先使用每 TIP 或每所属线程的独立客户端；或实现有等待预算的串行调度。不能用可能无界等待的全局锁替代。

**验收：** 两个真实并发调用者交错输入，结合超时、取消、断开和重连；验证无请求/响应串配、无共享操作对象重用、无相互取消。客户端应拒绝 msg_type、session_id、context_id 不匹配的响应。

## 4. P1：宿主组合串仍未隔离，部分成功后的 Rollback 不成立

位置：src/tsf_client/text_input_processor.h:46；src/tsf_client/text_input_processor.cpp:174、202、212；src/tsf_client/edit_session.h:75、130、169。

### 4.1 只有服务端 buffer 按 context 隔离

TIP 仍只有一个 m_pComposition。切换输入框时仅改变 m_activeContextId，没有核对组合对象所属 context，也没有先终止旧组合。后续会话可能拿旧 context 的 Range 配新 context 的编辑 cookie；候选和宿主组合状态仍可能错位。裸 ITfContext 指针也不是有失效管理的 context generation。

### 4.2 返回失败不代表宿主文本没有变化

例如 SetText 已成功，而后 SetSelection 或 EndComposition 失败：外层放行原始键并仅恢复服务端 prevBuffer，不能撤销已经修改的宿主文字，可能重复输入或留下不一致的组合串。只测服务端 Rollback 从 d 回到空串，不足以证明宿主失败恢复正确。

此外，取消分支的 SetText 返回值仍被忽略，提交分支有 Collapse 返回值未检查，修复说明中“每一步严格检查”的说法与代码不符。

**整改：** 为组合对象维护所属 context；定义 context 切换/销毁策略。区分“尚未修改文本即失败”“部分文本修改后失败”“完整成功”，分别处理按键消费和状态恢复。Rollback IPC 自身失败也必须有恢复策略。必要时采用明确的请求序号和确认协议。

**验收：** 用可注入失败的 TSF 替身覆盖 SetText 失败、SetText 成功后 SetSelection 失败、EndComposition 失败、清空组合失败；验证宿主文本、服务端状态和按键消费一致。再验证同进程两个输入框切换及服务退出/重启，不串词、不重复提交、不遗留旧候选。

## 5. P2：请求总耗时仍不受声明的硬上限约束

位置：src/tsf_client/ipc_client.cpp:111、151。

超时后调用 GetOverlappedResult(..., TRUE) 等待取消完成，这段等待没有 deadline。连接建立也在 deadline 计算之前；测试按键与实际按键各发一次请求。因此当前证据不能支撑“单次交互严格 50–60ms”“彻底消除挂死风险”等结论。普通本地管道场景约 100ms 返回，是已观察结果，不是所有取消路径的上限证明。

**整改与验收：** 将宿主线程的有限等待与后台 I/O 完成回收分离，操作内存、事件及 DLL 在完成回收前保持有效；报告整个按键路径的预算。验证超时边界附近回复、取消/完成竞争、服务挂起及恢复。

参考：[CancelIoEx 文档](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)。该 API 请求取消后，不等待所有被取消操作完成。

## 6. P2：全零反例帧大小错误，测试覆盖声明不准确

位置：tests/verify_negative_cases.ps1:34。

当前测试发送 512 字节，并注释为 sizeof(IpcKeyResponse)。实际当前 Windows pack(1) 响应结构是 **612 字节**：原 596 字节加两个 uint64_t 标识。于是 SendRequest 在长度检查处失败，输出 Ping SendRequest failed；尚未走到有效长度响应的语义检查。

**整改与验收：** 从协议定义生成尺寸或由原生测试辅助程序输出 sizeof，避免手写失配。分别覆盖短帧、完整长度的全零帧、身份不匹配帧、正确帧；检查每个反例在预期阶段失败，不能只匹配通用的 TEST FAILED 文本。

## 7. P2：DACL 注释与实际 SID 不一致，用户隔离未实现

位置：src/ime_service/main.cpp:163、171、192。

通过 .NET RawSecurityDescriptor 对当前 SDDL 的实际解析：

| 缩写 | 实际 SID | 说明 |
| --- | --- | --- |
| IU | S-1-5-4 | 交互登录组，并非当前用户 SID |
| AC | S-1-15-2-1 | 所有应用包 |
| RC | S-1-5-12 | Restricted Code，并非文中所写 S-1-15-2-2 |
| BA | S-1-5-32-544 | 内置管理员组 |
| SY | S-1-5-18 | Local System |

当前没有显式拒绝 NU/AN 的 ACE，也没有 PIPE_REJECT_REMOTE_CLIENTS，不能仅由注释证明严格拒绝网络调用；这里不认定已实测远程连接成功。

**整改与验收：** 明确用户/登录会话边界，使用对应 SID 与最小必要访问权；限制远程连接、验证客户端身份，并验证实际宿主调用权限。不要将 IU 等同于当前个人账户，也不要把浏览器地址栏输入成功单独当作 AppContainer 通信证据。

## 8. 下一次交付要求

1. 按以上问题逐项给出修复位置和有失败判定的针对性验证，保留已通过的基础测试。
2. 说明测试针对的源码版本和产物，提供一次干净构建结果；不以产物存在或脚本最后一行成功替代证据。
3. 先完成 P1 的隔离与失败恢复，再做记事本、浏览器、VS Code 的注册、组合、选词、快捷键、焦点切换、服务故障恢复和注销验证。
4. 区分“已动态复现”“静态发现”“尚未验证”。本报告中计数负值为动态复现；并发和宿主部分失败风险为源码发现，尚未在真实宿主中复现。

以上缺口关闭后再评估是否达到 90% 并由复核方收尾。本轮不要求扩展拼音/五笔算法或增加新产品功能。
