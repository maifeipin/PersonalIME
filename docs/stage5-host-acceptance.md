# Personal IME Stage 5: 真实宿主验收与安装发布报告

## 1. 交付基线与环境状态 (Delivery Baseline)

- **当前分支**: `feature/stage3`
- **基线 Commit**: `376310227968e21ed4cebba14424595228b76066`
- **代码状态**: 保留全部 Stage 4C 及本阶段未提交成果（未切换、重置、提交或推送）
- **架构支持范围**: **Windows 10 / Windows 11 (64-bit x64 / AMD64 ONLY)**
  > [!IMPORTANT]
  > 首版产物为纯 64 位架构（x64），未编译且不支持 32 位（x86/WOW64）宿主应用。文档与安装包均明确标明支持范围为 x64。

### 核心产物版本与校验和 (SHA-256)

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

## 2. 隔离与数据保护方案 (Isolation & Data Protection)

1. **用户现有数据零污染原则**:
   - 生产模式默认数据路径位于 `%APPDATA%\PersonalIME\user_dict.db`。
   - 调试测试工具（`scripts/host_test_env.ps1`）强制指定独立测试数据路径：`out/stage5_host_test/test_user_dict.db`。
   - 严禁任何测试脚本触碰或覆盖 `%APPDATA%\PersonalIME` 中的实际用户词库。
2. **进程管理隔离安全护栏 (P1-3 整改落实)**:
   - `scripts/host_test_env.ps1` 在启动测试服务前，先检测系统中既有的 `ime-service.exe` 进程及管道端点；若已有服务或端点占用，硬拒绝重复启动，杜绝端点冲突。
   - 启动测试服务后完整记录 PID、进程名、可执行文件全路径、启动时间戳于 `test_service.json`。
   - 在停止与清理阶段（`Stop-TestService`），严格核对目标进程名为 `ime-service`、二进制路径及创建时间，硬防护拒绝误杀任何非测试服务的进程（实测通过探针防护）。
3. **注册与注销审计防线 (P1-6 整改落实)**:
   - `scripts/register.ps1` 与 `scripts/unregister.ps1` 支持 `-DryRun` 审查模式。
   - 智能识别绿色便携布局（`..\PersonalIME.dll`）、同目录布局及开发构建布局（`..\build\Release\PersonalIME.dll`），彻底解决发行包执行 DryRun 报错问题。
   - 强制绑定 64 位 `System32\regsvr32.exe`，完成时校验 `HKCR\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}`。
4. **安装包与权限模型 (P1-4 & P1-5 整改落实)**:
   - 安装目录位于 `{autopf}\PersonalIME`，为纯机器级安装，不向 `HKCU` 写入用户级自启或数据路径。
   - 用户自造词与个人配置严格保留在用户独立的 `%APPDATA%\PersonalIME`，卸载默认保留，禁止管理员提升进程跨账号盲删用户数据。
   - 升级与卸载前通过安全状态机先排空并停止特定安装目录下的 `ime-service.exe`，在用 DLL（`PersonalIME.dll`）采用 `restartreplace` 延迟替换，需重启时明确待重启状态，不启动混合版本实例。

---

## 3. 验收覆盖矩阵与状态表 (Acceptance Matrix)

> [!NOTE]
> **证据分级说明**：
> - **自动化门禁验证 (Automated Gate)**：通过 `scripts/run_ci_gate.ps1` 覆盖的单元测试、IPC 管道通讯、状态机、故障注入及 WPF 测试，已 100% 自动化通过。
> - **真实宿主交互验收 (Physical Host Acceptance)**：需在安装/注册该输入法的主机环境中使用物理键盘和鼠标逐项实操验证，记录实际宿主表现。

### 测试宿主覆盖清单 (真实环境检测结果)
- **Host A (基础文本编辑)**: Windows 记事本 (`C:\WINDOWS\system32\notepad.exe` - x64) - **已就绪**
- **Host B (现代浏览器)**: Microsoft Edge (`C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe` - x64) & Google Chrome (`C:\Program Files\Google\Chrome\Application\chrome.exe` - x64) - **已就绪**
- **Host C (代码编辑器)**: Visual Studio Code (`C:\Users\chenl\AppData\Local\Programs\Microsoft VS Code\Code.exe` - Electron x64) - **已就绪**
- **Host D (现代终端)**: Windows Terminal (`C:\Users\chenl\AppData\Local\Microsoft\WindowsApps\wt.exe` - WinUI 3 x64) - **已就绪**
- **Host E (办公套件)**: Microsoft Word / Office (x64) - **未安装 (按规范跳过)**

---

### 验收测试项执行表 (10 大测试组)

| 编号 | 测试用例 | 详细操作步骤 | 预期结果 | 自动化门禁状态 | 真实宿主实测状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **G1-1** | 五笔单字与词组打字 | 在宿主输入 `aaaa`，按空格 | 候选框首位显示 `工`，空格后 `工` 成功上屏，记事本光标跟随 | **通过** (`test_stage3_integration`, `test_ipc_runner`) | **待宿主实测** |
| **G1-2** | 拼音全拼音节输入 | 在宿主输入 `zhong`，按数字 `1` | 候选首位显示 `中`，按 1 后纯净上屏 `中`，无残留拼音串 | **通过** (`test_stage3_integration` TEST 2) | **待宿主实测** |
| **G1-3** | 双拼输入体验 | 切换至双拼模式输入 `vs` | 候选准确显示 `中`；输入 `xk` 候选准确显示 `想` | **通过** (`test_input_policy` TEST 1 小鹤双拼) | **待宿主实测** |
| **G1-4** | 长词与多音节分页 | 输入 `fangan`，按 `-` / `=` 或 `PgUp` / `PgDn` | 候选正确展示，按键平滑翻页，首尾页边界不越界 | **通过** (`test_ime_core`, `test_candidate_window`) | **待宿主实测** |
| **G2-1** | 混输级联优先回退 | 开启混输，输入五笔无匹配编码 `xian` | 自动平滑回退至拼音解析，无输入吞键，准确召回 `西安`、`先` | **通过** (`test_input_policy` TEST 3 级联回退) | **待宿主实测** |
| **G2-2** | 禁用双拼严格阻断 | 在管理端禁用双拼，在宿主输入双拼编码 | 双拼完全不参与解析或回退，不产生隐式匹配，行为与仅全拼一致 | **通过** (`test_input_policy` TEST 20, `test_admin_ipc` TEST 18) | **待宿主实测** |
| **G2-3** | 方案独立切换 | 在宿主通过配置或快捷键切换主输入方案 | 切换方案后，当前会话立即生效，无需重启进程 | **通过** (`test_admin_ipc` TEST 18 在线切换方案) | **待宿主实测** |
| **G3-1** | 拼音合法性与隔音符 | 分别输入 `xian` 与 `xi'an` | `xian` 优先展示 `先/线`，`xi'an` 精确匹配展示 `西安` | **通过** (`test_ime_core` Step 2 隔音符切分) | **待宿主实测** |
| **G3-2** | 错码拦截与防悬浮 | 输入无法成音节或成字的错码 `qzk` | 无匹配候选时不上屏乱码，不弹出空白悬浮框 | **通过** (`test_input_policy` TEST 8 错码拒绝) | **待宿主实测** |
| **G4-1** | 中英文模式正交 | 输入中途按 `Ctrl+Space` 切换中英直通 | 立即进入英文直接上屏模式；再次按 `Ctrl+Space` 切回中文，输入方案保持原样 | **通过** (`test_ipc_runner`, `test_admin_ipc` 模式掩码) | **待宿主实测** |
| **G4-2** | 全半角独立切换 | 按 `Shift+Space` 切换全/半角 | 符号与字母呈现全角/半角输出，主输入方案及中英状态不被意外篡改 | **通过** (`test_admin_ipc` TEST 18 全半角正交掩码) | **待宿主实测** |
| **G4-3** | 中英文标点独立切换 | 按 `Ctrl+.` 切换标点符号模式 | 输入 `,` 分别上屏 `,` 或 `，`，当前编码方案不发生漂移 | **通过** (`test_admin_ipc` TEST 18 标点独立生效) | **待宿主实测** |
| **G4-4** | 多窗口上下文隔离 | 记事本 A 保持中文，切换至 Chrome 输入框 | 各窗口模式与组合串互不干扰，返回记事本 A 状态完全保留 | **通过** (`test_ipc_runner` TEST 5 上下文隔离) | **待宿主实测** |
| **G5-1** | 管理端自造词落盘 | 打开管理端录入词条 `赛博朋克` (编码 `sbpk`) 保存 | 保存成功，提示修订号递增，本地数据库更新 | **通过** (`test_admin_ipc` TEST 8, `WordbookViewModelTests`) | **待宿主实测** |
| **G5-2** | 宿主无感同步 | 无需重启记事本，在记事本直接输入 `sbpk` | 候选列表第 1 位即刻命中新词 `赛博朋克` | **通过** (`test_admin_ipc` TEST 8 实时同步引擎) | **待宿主实测** |
| **G5-3** | 词条停用即刻生效 | 在管理端停用该词条并保存，记事本再输入 `sbpk` | 候选列表不再展示停用的词条，立即退回原匹配规则 | **通过** (`test_admin_ipc` TEST 8 停用即刻生效) | **待宿主实测** |
| **G6-1** | 常用词频动态靠前 | 输入某个多候选编码，连续多次选择第 3 候选上屏 | 该候选词词频自增，再次输入该编码时，位置前移至第 1 候选 | **通过** (`test_ime_core` Step 7 学习模型) | **待宿主实测** |
| **G6-2** | 置顶绝对保护 | 在管理端将特定词置顶，高频使用其他候选 | 无论其他词频多高，置顶词始终锁定在固定序位，不被冲挤 | **通过** (`test_ime_core` Step 6, `test_admin_ipc` TEST 17) | **待宿主实测** |
| **G6-3** | 学习数据重启持久化 | 结束并重启后台服务 `ime-service.exe` | 之前自增的词频与排序仍然保留，WAL 恢复正确无损 | **通过** (`test_admin_ipc` TEST 9 重启持久化复原) | **待宿主实测** |
| **G7-1** | 拼音候选反查提示 | 开启反查提示，拼音输入 `gong` | 候选显示类似 `1. 工 [aaaa]` 的编码提示 | **通过** (`test_admin_ipc` TEST 19 反查编码提示) | **待宿主实测** |
| **G7-2** | 纯净文本绝对上屏 | 按空格上屏 `工 [aaaa]` | 写入宿主文档的文字严格为 `工`，绝不包含方括号及编码字符 | **通过** (`test_stage3_integration` TEST 2 纯净上屏) | **待宿主实测** |
| **G7-3** | 管理端编辑器反查联动 | 在管理端词条编辑框录入文本 `测试` | 界面预览区域实时回显对应的全拼/双拼/五笔反查编码 | **通过** (`test_admin_ipc` TEST 19, `WordbookViewModelTests`) | **待宿主实测** |
| **G8-1** | 核心控制键交互 | 输入编码按 `Space`、`Enter`、`Esc` | Space 首位上屏，Enter 原编码字母上屏，Esc 撤销组合串并关闭浮窗 | **通过** (`test_composition_lifecycle`, `test_candidate_window`) | **待宿主实测** |
| **G8-2** | 候选窗光标锚定定位 | 在宿主不同位置打字，改变窗口滚动位置 | 候选窗口随文本插入点精准定位，不超出屏幕可视边界 | **通过** (`test_candidate_window`, `test_tip_candidate_snapshot`) | **待宿主实测** |
| **G9-1** | 服务意外终止与自愈 | 输入过程中强制结束测试 `ime-service.exe` | 宿主应用不卡死崩溃；重启服务后，下一个按键自动重连 | **通过** (`test_composition_lifecycle` 故障注入与恢复) | **待宿主实测** |
| **G9-2** | 宿主焦点切换丢弃 | 组合串正在输入时，鼠标点击其他宿主窗口 | 原宿主组合串安全终结，不污染宿主文档，无残留孤儿浮窗 | **通过** (`test_composition_lifecycle` Context Switch 终结) | **待宿主实测** |
| **G9-3** | Chromium / Electron 兼容 | 在 Edge 地址栏及 VS Code 终端中输入 | 字符上屏正常，候选窗口渲染无重影，IME Composition 范围同步正确 | **通过** (`test_ipc_runner` 异步 Overlapped IO) | **待宿主实测** |
| **G10-1** | 安装器完整性与注册 | 运行 `PersonalIME-Setup-x64.exe` 安装 | 正确解压至 Program Files，服务可启动，COM/TSF 成功注册 | **通过** (Inno Setup 编译成功，包含 DLL/Service/Manager/Dict) | **待宿主实测** |
| **G10-2** | 使用中 DLL 延迟替换 | 宿主进程正在占用 DLL 时运行升级安装包 | 安装程序不强杀宿主进程，提示重启后延迟替换 (RestartReplace) | **通过** (`setup.iss` 明确 `CloseApplications=no` 与 `restartreplace`) | **待宿主实测** |
| **G10-3** | 覆盖升级数据完整性 | 覆盖安装新版本安装包 | 现有 `%APPDATA%\PersonalIME\user_dict.db` 及配置完全保留 | **通过** (安装目录 `{autopf}` 与用户目录 `{userappdata}` 严格分离) | **待宿主实测** |
| **G10-4** | 卸载与清理安全验证 | 运行卸载程序 | TSF 注册表项彻底注销；用户词库默认安全保留 | **通过** (`setup.iss` 注册注销与数据保护) | **待宿主实测** |

---

## 4. 应急回滚与清理步骤 (Emergency Rollback & Cleanup)

若在真实宿主验证中发现异常或需要恢复初始状态，请按以下步骤操作：

1. **退出宿主输入**:
   在正在输入的宿主应用中按 `Win+Space` 切换回 Windows 自带微软拼音/英文键盘。
2. **停止并清理测试服务**:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/host_test_env.ps1 -Action Cleanup
   ```
   *该命令将核验身份并仅终止本次测试启动的 ime-service.exe，清理临时数据库，并保持用户个人数据完好。*
3. **注销 TSF 注册项**:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/unregister.ps1
   ```
   *注销输入法在系统的注册，移除 `HKCR\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}`。*
