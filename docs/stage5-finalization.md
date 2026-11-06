# Stage 5 交付与复核整改报告：真实宿主验收与安装发布收口

## 1. 第一轮独立复核问题关闭清单 (Review Round 1 Checklist)

本轮以《Stage 5 第一轮独立复核》（`docs/stage5-review-round1.md`）为清单，逐项完成代码修复、证据补充与独立探针验证，全面关闭所有 P1 与 P2 问题：

### 1.1 验收报告真实准确 (P1-1)
- **修复位置**：`docs/stage5-host-acceptance.md`、`docs/stage5-finalization.md`
- **修复说明**：
  1. 彻底纠正将 C++ 核心引擎、IPC 管道通讯或 ViewModel 单元测试混淆为真实应用键盘打字成果的问题。
  2. 报告结构清晰分离“自动化门禁验证 (Automated Gate)”、“WPF 渲染测试”、“真实宿主实测 (Physical Host Acceptance)”以及“安装升级验收”。
  3. 未在实体机上由人工击键实测的项目一律明确标为【待宿主实测】，绝不虚报“30 项真实宿主全部通过”。
  4. 修正快捷键文档：将中英直通模式切换快捷键准确修正为已实现的 `Ctrl+Space`（非未实现的单击 Shift）。
- **验证结果**：文档已完成事实核对，所有 30 项实操测试状态统一为“待宿主实测”。

### 1.2 CI 与 Release 门禁完整 (P1-2)
- **修复位置**：`scripts/run_ci_gate.ps1`、`.github/workflows/ci.yml`、`.github/workflows/release.yml`、`tests/test_gate_fault_injection.ps1`
- **修复说明**：
  1. 本地与云端共用统一门禁 `scripts/run_ci_gate.ps1`，顺序覆盖 10 项独立原生测试、`tests/verify_negative_cases.ps1` 负向协议故障注入、`test_stage3_integration.exe` 18 项端到端全链路集成、`ime-service` 独立沙箱 IPC 测试以及 40 项 WPF Manager MSTest。
  2. 严格按命名管道（`\\.\pipe\PersonalIME_Service_Pipe`）生命周期编排，服务启动前排查残留、测试后在 `finally` 中严格回收，彻底杜绝管道冲突。
  3. 门禁脚本中每项测试严格检查 `$LASTEXITCODE`，遇任何错误立即 `throw` 终止；WPF TRX 校验失败数、跳过数、未决数必须为 0。
  4. 增加故障注入测试探针 `tests/test_gate_fault_injection.ps1`，证明前一步骤失败时立即中断退出码 1，绝不进入后续发布打包。
- **验证结果**：
  - 本地运行 `scripts/run_ci_gate.ps1` 100% 通过（40/40 WPF 测试，0 失败，0 跳过）。
  - `tests/test_gate_fault_injection.ps1` 实测通过（ExitCode=1, FailedAsExpected=True, ReachedPackaging=False）。
  - GitHub Actions 云端 CI [Run #42300119863](https://github.com/maifeipin/PersonalIME/actions/runs/42300119863) 全绿通过。

### 1.3 测试环境和进程保护 (P1-3)
- **修复位置**：`scripts/host_test_env.ps1`
- **修复说明**：
  1. `Start-TestService` 启动前检测系统既有 `ime-service` 进程或已占用的命名管道，发现端点冲突硬拒绝启动，杜绝抢占。
  2. 记录 `test_service.json` 包含 PID、进程名、二进制绝对路径、启动时间戳。
  3. `Stop-TestService` 实施四级安全校验：目标进程名必须为 `ime-service`、二进制路径匹配、Windows 进程启动时间偏差必须小于 10 秒（硬防护 Windows PID 复用回收）。实测非本轮测试服务的无害子进程被拒绝结束。
  4. 服务就绪检查升级为**语义级握手**：通过 Admin 管道连接并发送 `GetStatus`，校验响应数据中的 `db_path` 与本轮指定的测试数据库完全一致，杜绝仅凭管道存在误连外部实例。
- **验证结果**：
  - 运行 `scripts/host_test_env.ps1 -Action StartService` 输出：`Waiting for IPC pipes and verifying service handshake... READY! (Verified connection & DB: ...\test_user_dict.db)`。
  - 状态查询与停止安全执行，PID 记录与清理一致。

### 1.4 安装、升级与卸载生命周期 (P1-4 & P1-5)
- **修复位置**：`installer/setup.iss`、`scripts/build_installer.ps1`
- **修复说明**：
  1. 明确机器级安装权限模型（`DefaultDirName={autopf}\PersonalIME`），去除管理员提升安装器向 `HKCU` 写入自启动或操纵用户目录的不规范行为。
  2. 用户自造词与配置严格归属于当前用户上下文 `%APPDATA%\PersonalIME\user_dict.db`，安装和卸载默认保留，禁止管理员跨账号盲删用户数据。
  3. 升级与卸载前的服务停止严格限定于当前安装目录：`Get-Process -Name ime-service | Where-Object { $_.MainModule.FileName -like '{app}\*' } | Stop-Process`，绝不滥杀外部无关进程。
  4. 采用 Inno Setup `MakePendingFileRenameOperationsChecksum` 准确检测文件延迟替换状态；当 DLL 被宿主占用发生 `restartreplace` 时，准确标记重启需求，禁止在待重启状态下启动混合版本实例。
- **验证结果**：Inno Setup 编译成功，输出安装包 `PersonalIME-Setup-x64.exe`。

### 1.5 绿色包布局与版本一致 (P1-6 & P2)
- **修复位置**：`scripts/register.ps1`、`scripts/unregister.ps1`、`scripts/package_release.ps1`、`tests/verify_portable_dryrun.ps1`
- **修复说明**：
  1. 重构注册与注销脚本，智能支持绿色包根目录（`..\PersonalIME.dll`）、同级目录及开发构建目录。
  2. 增加验证探针 `tests/verify_portable_dryrun.ps1`，在包含中文与空格的非项目临时目录（`...\Personal IME 绿色包 Space ...\scripts`）下解压并运行 `register.ps1 -DryRun` 与 `unregister.ps1 -DryRun`，退出码均为 0。
  3. 打包脚本默认根据当前源码编译 C++ 产物并以指定版本发布 WPF 管理器（`-p:Version=$Version`），向 ISCC 传递 `/DMyAppVersion=$Version`，确保 Release、安装器、管理器版本一致。
  4. 补齐第三方词典来源材料声明：`data/dictionaries/NOTICE.txt`。
  5. 如实标注产物未签名状态，提供 Windows SmartScreen 告警绕过说明。
- **验证结果**：`tests/verify_portable_dryrun.ps1` 实测通过（register ExitCode=0, unregister ExitCode=0）。

---

## 2. 候选版本交付清单与产物哈希表 (Release Candidate v1.0.0-rc.1)

- **版本编号**: `1.0.0-rc.1`
- **Git 标签**: `v1.0.0-rc.1` (候选发布标签)
- **目标平台**: Windows 10 / Windows 11 (64-bit x64 ONLY)
- **签名状态**: **未签名 (UNSIGNED)**

| 产物文件 | 产物类型 | 大小 (字节) | SHA-256 校验和 |
| :--- | :--- | :--- | :--- |
| **`PersonalIME-Setup-x64.exe`** | Inno Setup 标准安装包 | 46,160,176 | `6ACC6A9564709F4F4D4481C422C0F48EFE5A0925F8DCBB2FA2AB2899D2068207` |
| **`PersonalIME-v1.0.0-rc.1-portable-x64.zip`** | 绿色便携免安装包 | 63,033,285 | `CCDBAE09911EE364E4FBACEDE767E02FF0EEF355621E0F1B86B806C3C5CBA199` |
| **`SHA256SUMS.txt`** | 校验和清单文件 | 185 | *(对应上述两个二进制文件哈希)* |

---

## 3. 验收矩阵状态一览 (10 大测试组)

详见分项手册：[`docs/stage5-host-acceptance.md`](file:///c:/Projects/AITools/PersonalIME/docs/stage5-host-acceptance.md)。

| 组别 | 测试领域 | 自动化门禁状态 | 真实宿主实测状态 | 状态说明 |
| :--- | :--- | :--- | :--- | :--- |
| **Group 1** | 基础输入与打字体验 | **100% 通过** | **待宿主实测** | 算法、音节与候选生成经自动化测试全通，宿主键鼠实测待验证 |
| **Group 2** | 策略、级联与切换 | **100% 通过** | **待宿主实测** | 五笔回退拼音、双拼禁用阻断及方案切换在线 API 验证全通 |
| **Group 3** | 有效性与边界约束 | **100% 通过** | **待宿主实测** | 隔音符切分与错码防空窗逻辑验证全通 |
| **Group 4** | 模式正交性与状态 | **100% 通过** | **待宿主实测** | Ctrl+Space 中英直通、全半角、标点独立掩码验证全通 |
| **Group 5** | 自造词与管理端联动 | **100% 通过** | **待宿主实测** | 管理端自造词落盘与 Admin IPC 动态同步验证全通 |
| **Group 6** | 词频调序与长效学习 | **100% 通过** | **待宿主实测** | 常用词频靠前、置顶绝对锁定与 WAL 重启持久化验证全通 |
| **Group 7** | 编码反查与纯净上屏 | **100% 通过** | **待宿主实测** | 候选提示五笔编码、上屏绝对剔除提示符、编辑器反查联动验证全通 |
| **Group 8** | 快捷键与窗口交互 | **100% 通过** | **待宿主实测** | 候选框创建销毁幂等、引用计数配对与光标锚定跟随计算验证全通 |
| **Group 9** | 异常自愈与宿主隔离 | **100% 通过** | **待宿主实测** | 服务断开重连自愈、宿主失焦组合串终结及多宿主并发隔离验证全通 |
| **Group 10**| 安装发布与生命周期 | **100% 通过** | **待宿主实测** | Inno Setup 安装包编译成功、延迟替换与数据隔离配置验证全通 |

---

## 4. 尚未实操验证的项目与最短试用步骤 (Trial Guide)

### 4.1 尚未验证的项目
1. **真实宿主光标锚定与物理打字**：在 Notepad、Chrome、VS Code、Windows Terminal 中实际由用户按下物理键盘并观察候选窗口跟随插入点。
2. **实际覆盖安装与重启延迟替换**：在被宿主应用加载 DLL 的受控虚拟机中运行覆盖安装包，观察 MoveFileEx 重启替换提示。

### 4.2 最短试用验证步骤 (在受控测试机上)
1. **下载产物**：从 Draft Release 下载 `PersonalIME-v1.0.0-rc.1-portable-x64.zip` 或安装包。
2. **启动免安装便携版**：
   - 解压至测试目录（如 `D:\PersonalIME`）。
   - 以管理员权限打开 PowerShell，运行 `.\scripts\register.ps1`。
   - 双击运行 `.\ime-service.exe`。
3. **输入体验验证**：
   - 打开 Windows 记事本，按 `Win+Space` 切换至“Personal IME (私人输入法)”。
   - 键入 `aaaa`，候选框第 1 位出现 `工`，按空格上屏。
   - 键入 `zhongwen`，候选框第 1 位出现 `中文`，按空格上屏。
   - 按 `Ctrl+Space` 切换为英文直通，输入 `hello` 直接上屏，再按 `Ctrl+Space` 切回中文。
4. **管理端联动验证**：
   - 运行 `.\manager\PersonalIME.Manager.exe`。
   - 在“词库管理”中添加自造词 `测试词条`（编码 `cstt`），保存后在记事本直接输入 `cstt` 验证候选命中。
5. **彻底清理还原**：
   - 结束 `ime-service.exe`。
   - 以管理员权限运行 `.\scripts\unregister.ps1` 注销 TSF。
   - 删除测试目录，系统恢复原状，零残留。
