# Stage 5 交付与复核整改报告：真实宿主验收与安装发布收口

## 1. 第一轮独立复核整改结论 (2026-11-07)

针对《Stage 5 第一轮独立复核》（`docs/stage5-review-round1.md`）指出的真实宿主证据边界、CI 缺少服务依赖、进程身份缺乏防护、绿色包路径断裂、安装器权限与状态机等关键问题，本轮已全部完成针对性修复与自动化验证：

1. **严格区分“自动化门禁”与“真实宿主实测” (P1-1)**：
   - 全面修正 `docs/stage5-host-acceptance.md` 与本总结报告，不再将 C++ 核心引擎、IPC 管道通讯或 ViewModel 单元测试冒充为 Notepad、Chromium、VS Code 等应用的真实键盘鼠标打字成果。
   - 真实宿主实测列明确标注为【待宿主实测】，待用户在具体受控环境下进行物理打字与窗口验证。
   - 纠正中英文切换快捷键文档为当前实际实现的 `Ctrl+Space`（非未实现的单击 Shift）。
2. **统一本地与 CI 质量门禁 `scripts/run_ci_gate.ps1` (P1-2)**：
   - 新建统一回归门禁脚本，包含三大阶段：独立原生测试、启动独立后台服务进行 IPC 测试（并在 `finally` 块中强制回收服务）、以及执行 WPF Manager MSTest 并解析 TRX 确保 40/40 通过、0 失败、0 跳过、0 Inconclusive。
   - 覆盖 `.github/workflows/ci.yml` 与 `release.yml`，对每一项原生命令严格检查 `$LASTEXITCODE`，遇任何错误立即阻断发布流水线；增加 Release 触发的版本参数传递与 Draft 权限。
3. **严格进程身份保护与端点冲突阻断 (P1-3)**：
   - `scripts/host_test_env.ps1` 在启动服务前严格检测系统既有 `ime-service` 进程或已占用的命名管道；遇冲突硬拒绝启动，杜绝端点抢占。
   - 启动测试服务后记录 `test_service.json` 包含 PID、进程名、二进制绝对路径、启动时间戳。
   - `Stop-TestService` 实施严格身份核验：核对 `ProcessName == "ime-service"`、可执行文件路径与启动时间差。实测独立探针确认：非本轮测试服务的子进程（如普通 PowerShell）被硬防护拦截，绝不误杀。
4. **绿色包与便携布局注册路径解耦 (P1-6)**：
   - 重构 `scripts/register.ps1` 与 `unregister.ps1`，支持自动探测绿色便携包根目录（`..\PersonalIME.dll`）、同级目录及开发构建目录（`..\build\Release\PersonalIME.dll`）。
   - 实测独立探针确认：解压至便携目录后运行 `register.ps1 -DryRun` 退出码由 1 转正为 0。
5. **规范机器级安装与用户数据隔离模型 (P1-4 & P1-5)**：
   - `installer/setup.iss` 明确为机器级安装（`{autopf}\PersonalIME`），去除提升安装器直接写入 `HKCU` 的不规范做法。用户自造词与配置严格归属于用户上下文的 `%APPDATA%\PersonalIME`，卸载默认保留。
   - 安装、升级与卸载前通过安全状态机先排空并停止特定安装目录下的后台服务；使用中的 `PersonalIME.dll` 采用 `restartreplace` 延迟替换，需重启时明确待重启状态，不启动混合版本实例。
   - 补齐第三方词典来源与开源许可材料声明：`data/dictionaries/NOTICE.txt`。
6. **非代码临时目录与文件清理归档**：
   - 彻底清理根目录遗留的 `handover-*`、`receipt-*` 临时目录及 `.db*`、`.obj` 临时文件，完整迁移至 `.归档/` 目录。
   - 更新 `.gitignore` 忽略 `.归档/`，保证临时资产绝不污染远程 Git 仓库。

---

## 2. 交付基线清单与产物哈希表

- **当前分支**: `feature/stage3`
- **基线 Commit**: `376310227968e21ed4cebba14424595228b76066`
- **版本编号**: `1.0.0`
- **目标平台**: Windows 10 / Windows 11 (64-bit x64 ONLY)

| 产物名称 | 文件相对路径 | 大小 (字节) | SHA-256 校验和 |
| :--- | :--- | :--- | :--- |
| **TSF 核心 DLL** | `build/Release/PersonalIME.dll` | 365,568 | `719CD9486706D94AE2ADAE7252425652A6D63242ED04D0878E2FB128EE7F8A72` |
| **后台服务进程** | `build/Release/ime-service.exe` | 2,484,224 | `A562BC7DC5BE223F606F30A63E843FCFB89FA40661F6D971F8F6C7E0EA82C58A` |
| **维护诊断 CLI** | `build/Release/ime-cli.exe` | 2,203,136 | `142B5CFD3D69A35370A1EA452226DBB4D0954A778BBDE5B86E734E885B47DF60` |
| **拼音基础词典** | `data/dictionaries/pinyin.dict` | 1,811,189 | `30B139BB7B3CA1F39BF05052820B4C6B2D124ACC4C9BB67047A03170ED2B5F97` |
| **五笔基础词典** | `data/dictionaries/wubi86.dict` | 1,985,170 | `A420ADDA6A5E36DEB7DA052C2FC43512F28DD4AFC698757CA0CCE4386932FA6F` |
| **WPF 管理端** | `out/publish/manager/PersonalIME.Manager.exe` | 131,597,153 | `D24B6D96B430414418B349FE0CB9B8F410F581E83FD9C83479F90CAFBE650CC6` |
| **Inno Setup 安装包** | `out/installer/PersonalIME-Setup-x64.exe` | 46,162,074 | `F6B9CA468D3BBAAC037535FC189D66CCEDDFD06F4B07468D2172FA147386E952` |

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

## 4. 后续步骤建议

1. **真实宿主上机验收**：
   在获得针对特定宿主实测的具体授权后，在已安装的目标宿主（记事本、Edge/Chrome、VS Code、Windows Terminal）中逐项执行 `docs/stage5-host-acceptance.md` 规定的击键与选词操作，记录实际输出文本与截屏。
2. **安装升级实操验收**：
   在测试虚拟机或用户确认的环境中实际运行 `PersonalIME-Setup-x64.exe`，执行首次安装、覆盖升级及卸载流程，验证 Program Files 落地、TSF 真实上屏及 `%APPDATA%` 数据保留。
