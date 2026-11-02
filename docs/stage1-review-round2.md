# Stage 1 第二轮复核

本文件保留第二轮历史记录；最新结果见 [第三轮复核](stage1-review-round3.md)。

日期：2026-11-02。结论：已有修复有效，但仍未满足进入 Stage 2 的门槛。真实宿主注册/上屏验证尚未执行。

## 本机复现结果

- 执行现有 scripts/run_test.ps1，退出码 0。
- COM 生命周期测试通过；测试范围是类工厂和 TIP 实例，未覆盖实际组合输入、候选窗口或异步编辑会话。
- 全零响应被拒绝，测试器退出 1。
- 服务端不回复时，本次测量约 94ms 后测试器退出 1。
- 正常 IPC 路径退出 0；重定向输出中中文附近日志出现截断，不能据日志认定每条 PASS 均完整显示。
- dumpbin /dependents 显示 PersonalIME.dll 仅导入 ole32、ADVAPI32、USER32、GDI32、KERNEL32，未见动态 VC CRT 依赖。
- 测试脚本启动的 Windows PowerShell 加载了用户 profile，出现 fnm 权限错误但继续执行；建议加入 -NoProfile 消除环境干扰。
- 本轮未重新构建，验证的是磁盘上已有 Release 产物，并同时静态检查当前源码。测试后台进程已退出，未修改注册信息或产品源码。

## 仍需修复的阻断项

### P1：CancelIoEx 后过早释放异步 I/O 所用对象

位置：src/tsf_client/ipc_client.cpp:77、115。

超时路径调用 CancelIoEx 后立即关闭事件、断开管道并返回。ovWrite/ovRead 是栈对象，请求/响应也引用调用方内存。CancelIoEx 只请求取消，不等待完成；当前实现未确认操作已完成就结束这些对象的生命周期，存在取消竞态下的失效内存访问风险。一次挂起反例及时退出不能证明此路径内存安全。

修复要求：使用持有 OVERLAPPED、事件及收发缓冲区的操作对象，在确认完成之前保持其存活。若必须限制宿主线程等待时间，应将取消后的完成回收交给独立所有者处理，不应在按键线程无界等待取消完成。覆盖临近超时回复、断开、取消与正常完成竞争的场景。

另外，当前分别给写入和读取 timeoutMs，单次请求并非严格总计 80ms；OnTestKeyDown 和 OnKeyDown 还各发一次请求。应明确端到端预算，并用同一个 deadline 计算剩余时间。

依据：https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex

### P1：编辑会话仍会虚报成功，异步回退检查位置不正确

位置：src/tsf_client/edit_session.h:71、117、127、164；src/tsf_client/text_input_processor.cpp:192。

DoEditSession 忽略 SetText、SetSelection、EndComposition 等 HRESULT；StartComposition 或 GetSelection 失败也可能最终返回 S_OK。外层检查 SUCCEEDED(hrSession) 无法发现这些失败，因此仍可能吞键。

RequestEditSession 的 TF_E_SYNCHRONOUS 应检查输出 hrSession，当前只检查函数返回 hr。异步请求成功时的 TF_S_ASYNC 只表示已安排，不表示文本修改已执行成功；需要区分“已接受”和“执行完成”。

服务端在宿主编辑成功之前已经修改或清空 buffer。失败后仅放行按键会留下服务端/宿主状态不一致，后续可能重复字母或错误提交。需要定义提交确认、失败恢复和上下文失效规则，并用可注入失败的 TSF 替身测试。

依据：https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession

### P1：上下文、线程与故障恢复隔离仍未落实

IpcClient 与 CCandidateWindow 仍是进程级单例。没有会话 ID、上下文代数或对应线程保护；一个 TIP 的失焦/停用可重置或销毁其他 TIP 使用的状态。OnSetFocus 不能替代文档上下文切换处理；Deactivate 释放组合引用也不等于正确终止宿主组合串。

服务故障时也没有统一清理旧候选与组合状态的路径。请验证同进程两个输入框/两个线程、组合中切焦点、服务退出与恢复，确保不会串词、重复提交或保留悬浮候选框。

## 测试自身还存在回归时挂起的缺陷

tests/verify_negative_cases.ps1:50 调用 WaitForExit(1500) 后，在检查 exited 之前就 ReadToEnd。如果客户端再次挂死，ReadToEnd 会等待 EOF，永远到不了后面的 Kill，finally 也无法自动进入。

要求：等待超时先终止并确认子进程退出，再读取输出；或并发异步采集输出。挂起反例还应检查预期非零退出码和错误原因，不能仅凭“进程退出”就当作成功，崩溃退出也会满足该条件。增加一个真正不退出的替代测试进程，验证测试脚本自身能限时失败并清理资源。

## 其他未关闭项

- 管道仍为固定全局名字并向 Everyone 授予 GenericAll，未落实目标用户/会话身份隔离；服务端仍未验证请求长度等协议边界。
- 注销接口仍忽略 API 失败并返回 S_OK，不能保证回滚和卸载确实完成。
- 候选窗口代码已经新增，但多 DPI、窗口失活/最小化后的更新、AppContainer 可见性与实际焦点行为均未验证。
- 本轮不否定已通过的 COM 基础测试和 CRT 依赖检查；这些结果不足以推出整个 Stage 1 通过。

## 准入条件

先修复上述 I/O 生命周期、编辑失败和上下文隔离问题，补充具有失败退出码与外部时限的对应测试；再做记事本、浏览器、VS Code 的注册、组合、选词、快捷键、失焦、服务退出/重启与注销实测。通过后进入 Stage 2。
