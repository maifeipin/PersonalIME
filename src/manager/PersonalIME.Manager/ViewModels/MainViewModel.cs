using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public enum ServiceConnectionState
{
    Disconnected,
    Starting,
    Connected,
    Failed
}

public class MainViewModel : ViewModelBase, IDisposable
{
    private readonly IAdminClient _client;
    private readonly CancellationTokenSource _cts = new();
    private int _selectedTabIndex = 0;
    private ServiceConnectionState _serviceState = ServiceConnectionState.Disconnected;
    private string _connectionStatusText = "正在连接服务...";
    private string _offlineWarningText = "后台服务离线，输入法可能退化为英文直通模式";
    private string _activeSchemeText = "未知";
    private string _activeMixedInputText = "未知";
    private string _activeInitialModeText = "未知";
    private string _configSavedStatusText = "未加载";
    private InputPolicyConfig? _activePolicy;

    public CancellationToken CancellationToken => _cts.Token;
    public bool IsDisposed { get; private set; }

    public NotificationBanner Banner { get; }
    public WordbookViewModel Wordbook { get; }
    public SortingViewModel Sorting { get; }
    public SettingsViewModel Settings { get; }
    public MaintenanceViewModel Maintenance { get; }
    public PluginsViewModel Plugins { get; }

    public ServiceConnectionState ServiceState
    {
        get => _serviceState;
        private set
        {
            if (SetProperty(ref _serviceState, value))
            {
                OnPropertyChanged(nameof(IsConnected));
                OnPropertyChanged(nameof(HasOfflineWarning));
            }
        }
    }

    public bool IsConnected
    {
        get => ServiceState == ServiceConnectionState.Connected;
        private set
        {
            ServiceState = value ? ServiceConnectionState.Connected : ServiceConnectionState.Disconnected;
        }
    }

    public bool HasOfflineWarning => !IsConnected;

    public string ConnectionStatusText
    {
        get => _connectionStatusText;
        private set => SetProperty(ref _connectionStatusText, value);
    }

    public string OfflineWarningText
    {
        get => _offlineWarningText;
        private set => SetProperty(ref _offlineWarningText, value);
    }

    public string ActiveSchemeText
    {
        get => _activeSchemeText;
        private set => SetProperty(ref _activeSchemeText, value);
    }

    public string ActiveMixedInputText
    {
        get => _activeMixedInputText;
        private set => SetProperty(ref _activeMixedInputText, value);
    }

    public string ActiveInitialModeText
    {
        get => _activeInitialModeText;
        private set => SetProperty(ref _activeInitialModeText, value);
    }

    public string ConfigSavedStatusText
    {
        get => _configSavedStatusText;
        private set => SetProperty(ref _configSavedStatusText, value);
    }

    public InputPolicyConfig? ActivePolicy
    {
        get => _activePolicy;
        private set => SetProperty(ref _activePolicy, value);
    }

    public int SelectedTabIndex
    {
        get => _selectedTabIndex;
        set
        {
            if (SetProperty(ref _selectedTabIndex, value))
            {
                OnTabChanged(_selectedTabIndex);
            }
        }
    }

    public AsyncRelayCommand CheckConnectionCommand { get; }
    public ICommand OpenCalendarCommand { get; }

    public MainViewModel(IAdminClient client)
    {
        _client = client;
        if (client is AdminIpcClient pipeClient)
        {
            pipeClient.LifetimeCancellationToken = _cts.Token;
        }
        Banner = new NotificationBanner();

        Wordbook = new WordbookViewModel(_client, Banner);
        Sorting = new SortingViewModel(_client, Banner);
        Settings = new SettingsViewModel(_client, Banner);
        Maintenance = new MaintenanceViewModel(_client, Banner);
        Plugins = new PluginsViewModel(_client, Banner);

        Settings.PolicySaved += OnPolicySaved;

        CheckConnectionCommand = new AsyncRelayCommand(CheckConnectionAsync);
        OpenCalendarCommand = new RelayCommand(() =>
        {
            var win = new Views.CalendarWindow();
            win.Show();
        });
    }

    private void OnPolicySaved(InputPolicyConfig policy)
    {
        ApplyActivePolicy(policy);
    }

    private void ApplyActivePolicy(InputPolicyConfig policy)
    {
        ActivePolicy = policy;
        ActiveSchemeText = InputSchemes.ToDisplayName(policy.PrimaryInputMethod);
        ActiveMixedInputText = policy.MixedInputEnabled ? "开启" : "关闭";
        ActiveInitialModeText = policy.InitialMode?.Chinese == true ? "中文" : "英文";
        ConfigSavedStatusText = policy.Revision > 0 ? $"已保存生效 (rev.{policy.Revision})" : "默认配置已生效";
    }

    private void ResetActivePolicyOffline()
    {
        ActivePolicy = null;
        ActiveSchemeText = "未知 (离线)";
        ActiveMixedInputText = "未知 (离线)";
        ActiveInitialModeText = "未知 (离线)";
        ConfigSavedStatusText = "未连接后台";
        OfflineWarningText = "后台服务离线，输入法可能退化为英文直通模式";
    }

    public async Task InitializeAsync()
    {
        await CheckConnectionAsync();
        if (IsConnected)
        {
            await Wordbook.LoadEntriesAsync();
        }
    }

    public async Task CheckConnectionAsync()
    {
        // 1. Try connecting to already running service
        try
        {
            var status = await _client.GetStatusAsync();
            ServiceState = ServiceConnectionState.Connected;
            ConnectionStatusText = $"● 输入法已就绪 (v{status.ServiceVersion})";
            await RefreshActivePolicyAsync();
            OnTabChanged(SelectedTabIndex);
            return;
        }
        catch (OperationCanceledException)
        {
            throw; // Re-throw cancellation for lifetime tests
        }
        catch (Exception)
        {
            // Not connected yet
        }

        // Only auto-launch for AdminIpcClient on default pipe (do not auto-launch in isolated tests or mock clients)
        if (_client is AdminIpcClient ipcClient && ipcClient.PipeName == AdminIpcClient.DefaultPipeName)
        {
            await TryAutoLaunchAndConnectAsync();
        }
        else
        {
            ServiceState = ServiceConnectionState.Disconnected;
            ConnectionStatusText = "○ 输入法未就绪 (后台服务离线)";
            ResetActivePolicyOffline();
        }
    }

    private async Task TryAutoLaunchAndConnectAsync()
    {
        string? servicePath = LocateServiceExecutable();
        if (string.IsNullOrEmpty(servicePath))
        {
            ServiceState = ServiceConnectionState.Failed;
            ConnectionStatusText = "○ 启动失败: 未找到后台服务程序 ime-service.exe";
            ResetActivePolicyOffline();
            return;
        }

        ServiceState = ServiceConnectionState.Starting;
        ConnectionStatusText = "● 正在准备输入法引擎...";

        // Check if an existing ime-service process is already running (e.g. launched by another instance)
        Process? startedProcess = null;
        try
        {
            var existingProcesses = Process.GetProcessesByName("ime-service");
            bool hasRunning = existingProcesses.Length > 0;
            foreach (var p in existingProcesses)
            {
                p.Dispose();
            }

            if (!hasRunning)
            {
                var psi = new ProcessStartInfo
                {
                    FileName = servicePath,
                    WorkingDirectory = Path.GetDirectoryName(servicePath) ?? string.Empty,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                };
                startedProcess = Process.Start(psi);
            }
        }
        catch (Exception ex)
        {
            ServiceState = ServiceConnectionState.Failed;
            ConnectionStatusText = $"○ 启动失败: 进程创建失败 ({ex.Message})";
            ResetActivePolicyOffline();
            return;
        }

        // Loop wait for IPC handshake up to 4000ms
        const int maxAttempts = 20;
        const int intervalMs = 200;

        for (int attempt = 0; attempt < maxAttempts; attempt++)
        {
            if (_cts.IsCancellationRequested) return;

            if (startedProcess != null && startedProcess.HasExited)
            {
                ServiceState = ServiceConnectionState.Failed;
                ConnectionStatusText = $"○ 启动失败: 服务异常退出 (退出码: {startedProcess.ExitCode})";
                ResetActivePolicyOffline();
                return;
            }

            try
            {
                var status = await _client.GetStatusAsync();
                ServiceState = ServiceConnectionState.Connected;
                ConnectionStatusText = $"● 输入法已就绪 (v{status.ServiceVersion})";
                await RefreshActivePolicyAsync();
                OnTabChanged(SelectedTabIndex);
                return;
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch
            {
                await Task.Delay(intervalMs, _cts.Token);
            }
        }

        ServiceState = ServiceConnectionState.Failed;
        ConnectionStatusText = "○ 启动失败: 等待后台服务握手超时";
        ResetActivePolicyOffline();
    }

    private async Task RefreshActivePolicyAsync()
    {
        try
        {
            var policy = await _client.GetPolicyAsync();
            ApplyActivePolicy(policy);
        }
        catch
        {
            ActiveSchemeText = "五笔86";
            ActiveMixedInputText = "开启";
            ActiveInitialModeText = "中文";
            ConfigSavedStatusText = "服务已就绪 (默认配置)";
        }
    }

    public static string? LocateServiceExecutable()
    {
        var candidates = new List<string>();

        var envPath = Environment.GetEnvironmentVariable("PERSONAL_IME_SERVICE_PATH");
        if (!string.IsNullOrEmpty(envPath))
        {
            candidates.Add(envPath);
        }

        // 1. Same directory (portable / co-located)
        candidates.Add(Path.Combine(AppContext.BaseDirectory, "ime-service.exe"));
        // 2. Parent directory (e.g. Program Files\PersonalIME\manager\..\ime-service.exe)
        candidates.Add(Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "ime-service.exe")));

        // 3. Configured build directory (e.g. CI / dev override via PERSONALIME_BUILD_DIR)
        var envBuildDir = Environment.GetEnvironmentVariable("PERSONALIME_BUILD_DIR");
        if (!string.IsNullOrEmpty(envBuildDir))
        {
            candidates.Add(Path.Combine(envBuildDir, "Release", "ime-service.exe"));
            candidates.Add(Path.Combine(envBuildDir, "Debug", "ime-service.exe"));
        }

        // 4. Dev / CI build paths: walk up ancestor directories from AppContext.BaseDirectory
        string? dir = Path.TrimEndingDirectorySeparator(AppContext.BaseDirectory);
        for (int depth = 0; depth < 8 && !string.IsNullOrEmpty(dir); depth++)
        {
            if (!string.IsNullOrEmpty(envBuildDir) && !Path.IsPathRooted(envBuildDir))
            {
                candidates.Add(Path.Combine(dir, envBuildDir, "Release", "ime-service.exe"));
            }
            candidates.Add(Path.Combine(dir, "build", "Release", "ime-service.exe"));
            candidates.Add(Path.Combine(dir, "build_ci", "Release", "ime-service.exe"));
            candidates.Add(Path.Combine(dir, "build", "bin", "Debug", "ime-service.exe"));
            candidates.Add(Path.Combine(dir, "build", "Debug", "ime-service.exe"));
            dir = Path.GetDirectoryName(dir);
        }

        // 5. Program Files installed location
        candidates.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "PersonalIME", "ime-service.exe"));

        foreach (var p in candidates)
        {
            try
            {
                if (File.Exists(p))
                {
                    return p;
                }
            }
            catch { }
        }
        return null;
    }

    public void Dispose()
    {
        if (!IsDisposed)
        {
            IsDisposed = true;
            Settings.PolicySaved -= OnPolicySaved;
            try
            {
                if (!_cts.IsCancellationRequested)
                {
                    _cts.Cancel();
                }
                _cts.Dispose();
            }
            catch { }
        }
    }

    private void OnTabChanged(int index)
    {
        switch (index)
        {
            case 0:
                _ = Wordbook.LoadEntriesAsync();
                break;
            case 1:
                break;
            case 2:
                if (!Settings.IsLoaded)
                {
                    _ = Settings.LoadPolicyAsync();
                }
                break;
            case 3:
                _ = Plugins.LoadDataAsync();
                break;
            case 4:
                _ = Maintenance.RefreshStatusAsync();
                break;
        }
    }
}
