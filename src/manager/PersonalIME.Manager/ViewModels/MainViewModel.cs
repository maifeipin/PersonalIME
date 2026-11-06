using PersonalIME.Manager.Ipc;

namespace PersonalIME.Manager.ViewModels;

public class MainViewModel : ViewModelBase, IDisposable
{
    private readonly IAdminClient _client;
    private readonly CancellationTokenSource _cts = new();
    private int _selectedTabIndex = 0;
    private bool _isConnected;
    private string _connectionStatusText = "正在连接服务...";

    public CancellationToken CancellationToken => _cts.Token;
    public bool IsDisposed { get; private set; }

    public NotificationBanner Banner { get; }
    public WordbookViewModel Wordbook { get; }
    public SortingViewModel Sorting { get; }
    public SettingsViewModel Settings { get; }
    public MaintenanceViewModel Maintenance { get; }

    public void Dispose()
    {
        if (!IsDisposed)
        {
            IsDisposed = true;
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

    public bool IsConnected
    {
        get => _isConnected;
        private set => SetProperty(ref _isConnected, value);
    }

    public string ConnectionStatusText
    {
        get => _connectionStatusText;
        private set => SetProperty(ref _connectionStatusText, value);
    }

    public AsyncRelayCommand CheckConnectionCommand { get; }

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

        CheckConnectionCommand = new AsyncRelayCommand(CheckConnectionAsync);
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
        try
        {
            var status = await _client.GetStatusAsync();
            IsConnected = true;
            ConnectionStatusText = $"● 已连接 (PersonalIME 服务 v{status.ServiceVersion})";
        }
        catch (Exception)
        {
            IsConnected = false;
            ConnectionStatusText = "○ 未连接 (PersonalIME 后台服务离线)";
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
                // Sorting page
                break;
            case 2:
                if (!Settings.IsLoaded)
                {
                    _ = Settings.LoadPolicyAsync();
                }
                break;
            case 3:
                _ = Maintenance.RefreshStatusAsync();
                break;
        }
    }
}

