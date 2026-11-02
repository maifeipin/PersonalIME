# Stage 1 独立复核

本文件保留首次复核的历史记录；最新结论见 [第二轮复核](stage1-review-round2.md)。

日期：2026-11-02。结论：构建成功，基础 IPC 可运行；Stage 1 尚未通过验收。当前未注册输入法，未进行记事本/Edge/VS Code 真实宿主验证，未进入 Stage 2。

## 已实际执行

- 通过 CMake/MSBuild 重新构建 x64 Release，退出码 0。
- 启动后台服务并运行现有测试器，退出码 0；测试进程与本次启动的服务已退出。
- 使用替代命名管道服务端，对全部请求回复 596 字节全零响应。测试器仍退出 0，并打印所有测试通过，尽管组合串和提交内容均为空。
- 使用替代服务端读取首个 16 字节请求后不回复，客户端在 1500ms 内未退出，随后由复核脚本终止。
- 检查生成工程：Release 定义了 NDEBUG，RuntimeLibrary 为 MultiThreadedDLL。

复现脚本：`C:\Users\chenl\.codex\visualizations\2026\11\02\01a19a4c-2ed5-76c0-b225-9a55e54e1ab5\stage1-repro.ps1`。脚本只启动/停止自己的测试进程，不修改系统注册信息；发现已有同名服务时拒绝执行。

## 阻断项

### P1：Release 测试禁用了关键断言

`tests/test_ipc_runner.cpp` 使用 assert 校验响应内容，Release 的 NDEBUG 将这些校验移除。全零响应仍然通过已经实测。部分 SendRequest 返回值也没有检查。

要求：使用所有构建配置中均有效的检查，失败必须返回非零；验证 Ping、类型、长度、内容及全部请求返回值。保留全零响应反例，证明测试能够拒绝错误结果。现有测试只覆盖 IPC，不覆盖真实 TSF 组合串/上屏。

### P1：按键线程同步阻塞且没有响应期限

`src/tsf_client/ipc_client.cpp` 的 WriteFile/ReadFile 使用同步句柄；OnTestKeyDown、OnKeyDown、Reset 均会调用该路径。服务仍连接但停止响应时，会阻塞宿主按键线程。服务端 CreateNamedPipe 的默认 timeout 参数不构成客户端 ReadFile 超时。

要求：设置可取消的有限等待，明确超时后的组合串与按键处理；服务缺失、退出、挂起、重启都必须分别测试。不能将“服务退出导致管道断开”推广为“任何故障都不会挂死”。

### P1：DLL 生命周期计数没有覆盖 TIP 活实例

`src/tsf_client/class_factory.cpp` 只统计类工厂和 LockServer。`CTextInputProcessor` 的构造/析构没有维护 g_cDllRef；释放类工厂后，即使仍有 TIP 实例，DllCanUnloadNow 也可能返回 S_OK。编辑会话所持有的 TIP 引用不能补足模块计数。

要求：所有存活的 COM 对象都必须确保 DLL 不可卸载，并增加类工厂释放后仍持有 TIP/编辑会话的生命周期验证。

### P1：输入失败仍可能吞键，快捷键修饰符未处理

OnKeyDown 在申请编辑会话前就设置 pfEaten，后续未检查 hrSession 或文本修改的 HRESULT。申请/执行失败仍可能吞键。请求中的 modifiers 始终为零，服务端按 A–Z 直接处理，因此 Ctrl+C、Ctrl+V、Alt 组合等也可能被截获。

要求：明确测试按键与实际处理间的状态一致性；正确处理修饰符、非编辑状态、受限输入框和编辑失败；验证系统及宿主快捷键。

## 其他必须补齐的内容

1. **上下文隔离**：进程内单例 IpcClient 共享一个服务端 buffer，没有 context/session/generation 标识或线程同步。同一进程的不同输入框/线程可能串状态；异步编辑会话也缺少防止迟到结果应用到旧上下文的机制。失焦和 Deactivate 仅释放组合引用或重置服务，不等于正确结束宿主组合串。
2. **注册结果真实性**：DllRegisterServer 忽略 Register、AddLanguageProfile、RegisterCategory 的失败并返回 S_OK；注销同样忽略失败。需传播 HRESULT、处理部分成功的回滚、配对 COM 初始化，并验证配置文件确实可启用。仅注册 GUID_TFCAT_TIP_KEYBOARD 不足以证明现代应用兼容。
3. **候选 UI 尚未实现**：服务端填充 candidates，但 TSF 客户端没有显示候选列表的实现；也没有设置组合串显示属性的代码。因此现阶段不能声称已实现候选窗口或保证虚线下划线效果。
4. **IPC 边界**：服务端没有验证请求字节数；客户端没有验证响应字符串终止符和 candidate_count。固定全局管道名、Everyone GenericAll 权限和无客户端身份校验也需要收敛到目标用户/会话，限制远程访问并验证实际权限。增加 AC 的 ACE 不等于已经通过 AppContainer 实测。
5. **运行时依赖**：当前为 /MD 动态 CRT 配置，不能称为零额外运行时依赖；应检查 DLL 导入依赖与干净机器上的部署要求。
6. **构建脚本**：build.ps1 没有逐次检查原生命令 LASTEXITCODE，配置或编译失败后仍可能打印成功。run_test.ps1 依赖当前工作目录，且服务启动未设置隐藏窗口；应使用脚本路径定位项目并检查服务就绪。

## 进入 Stage 2 前的门槛

1. 修复上述阻断项，证明错误响应会失败、服务挂起有界退出、COM 活实例不会被允许卸载。
2. 在独立测试进程中覆盖提交、取消、失焦、上下文切换、服务中断及重连。
3. 再进行注册与真实宿主测试，逐一记录记事本、浏览器、VS Code 的应用版本、输入结果、快捷键与故障恢复；浏览器主进程中的成功不能单独证明 AppContainer 通信通过。
4. 验证注销清理，确认已启动的测试后台进程全部退出。

本次仅增加复核文档，未修改产品源码或既有测试器。以上源码问题为静态复核发现，除明确列出的反例外，尚未逐项动态复现。
