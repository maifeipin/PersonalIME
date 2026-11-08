using System.IO;
using System.Windows;
using Microsoft.Win32;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public class MaintenanceViewModel : ViewModelBase
{
    private readonly IAdminClient _client;
    private readonly NotificationBanner _banner;

    private ServiceStatusInfo _status = new();
    private bool _isLoading;

    private string _importMode = "merge"; // "merge" or "replace"
    private bool _isConfirmDialogOpen;
    private string _confirmDialogTitle = string.Empty;
    private string _confirmDialogMessage = string.Empty;
    private Func<Task>? _pendingConfirmedAction;

    public ServiceStatusInfo Status
    {
        get => _status;
        private set => SetProperty(ref _status, value);
    }

    public bool IsLoading
    {
        get => _isLoading;
        private set => SetProperty(ref _isLoading, value);
    }

    public string ImportMode
    {
        get => _importMode;
        set => SetProperty(ref _importMode, value);
    }

    public bool IsConfirmDialogOpen
    {
        get => _isConfirmDialogOpen;
        set => SetProperty(ref _isConfirmDialogOpen, value);
    }

    public string ConfirmDialogTitle
    {
        get => _confirmDialogTitle;
        set => SetProperty(ref _confirmDialogTitle, value);
    }

    public string ConfirmDialogMessage
    {
        get => _confirmDialogMessage;
        set => SetProperty(ref _confirmDialogMessage, value);
    }

    public string FormattedDbSize => $"{Status.DatabaseSizeBytes / 1024.0 / 1024.0:F2} MB";
    public string FormattedUptime => TimeSpan.FromSeconds(Status.UptimeSeconds).ToString(@"d\.hh\:mm\:ss");

    public AsyncRelayCommand RefreshStatusCommand { get; }
    public RelayCommand CopyDiagnosticsCommand { get; }
    public AsyncRelayCommand ExportDataCommand { get; }
    public RelayCommand RequestImportDataCommand { get; }
    public RelayCommand RequestResetStatsCommand { get; }
    public AsyncRelayCommand ConfirmActionCommand { get; }
    public RelayCommand CancelActionCommand { get; }
    public AsyncRelayCommand? RestartServiceCommand { get; }

    public MaintenanceViewModel(IAdminClient client, NotificationBanner banner, Func<Task>? restartServiceAction = null)
    {
        _client = client;
        _banner = banner;

        if (restartServiceAction != null)
        {
            RestartServiceCommand = new AsyncRelayCommand(restartServiceAction);
        }

        RefreshStatusCommand = new AsyncRelayCommand(() => RefreshStatusAsync(true));
        CopyDiagnosticsCommand = new RelayCommand(CopyDiagnostics);
        ExportDataCommand = new AsyncRelayCommand(ExportDataAsync);
        RequestImportDataCommand = new RelayCommand(RequestImportData);
        RequestResetStatsCommand = new RelayCommand(RequestResetStats);
        ConfirmActionCommand = new AsyncRelayCommand(ConfirmActionAsync);
        CancelActionCommand = new RelayCommand(CancelAction);
    }

    public async Task RefreshStatusAsync(bool showBanner = true)
    {
        IsLoading = true;
        try
        {
            Status = await _client.GetStatusAsync();
            OnPropertyChanged(nameof(FormattedDbSize));
            OnPropertyChanged(nameof(FormattedUptime));
            if (showBanner)
            {
                _banner.ShowInfo("服务状态已刷新");
            }
        }
        catch (TimeoutException)
        {
            if (showBanner)
            {
                _banner.ShowWarning("查询服务状态超时，结果尚未确认");
            }
        }
        catch (Exception ex)
        {
            if (showBanner)
            {
                _banner.ShowError($"获取服务状态失败: {ex.Message}");
            }
        }
        finally
        {
            IsLoading = false;
        }
    }

    private void CopyDiagnostics()
    {
        string info = $"PersonalIME Diagnostics:\n" +
                      $"Version: {Status.ServiceVersion}\n" +
                      $"Uptime: {FormattedUptime}\n" +
                      $"Active Clients: {Status.ActiveClients}\n" +
                      $"Database: {Status.DatabasePath} ({FormattedDbSize})\n" +
                      $"Last Commit Receipt: {Status.LastCommitReceiptTime}";

        try
        {
            Clipboard.SetText(info);
            _banner.ShowSuccess("诊断信息已复制到剪贴板");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"复制失败: {ex.Message}");
        }
    }

    public async Task ExportDataAsync()
    {
        var sfd = new SaveFileDialog
        {
            Filter = "JSON 词库备份 (*.json)|*.json",
            FileName = $"personal_ime_backup_{DateTime.Now:yyyyMMdd_HHmmss}.json",
            Title = "导出词库与配置"
        };

        if (sfd.ShowDialog() != true) return;

        string targetPath = Path.GetFullPath(sfd.FileName);
        IsLoading = true;
        try
        {
            await _client.ExportDataAsync(targetPath);
            _banner.ShowSuccess($"词库与配置成功导出到: {Path.GetFileName(targetPath)}");
        }
        catch (TimeoutException)
        {
            _banner.ShowWarning("导出超时，结果尚未确认");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"导出失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    private void RequestImportData()
    {
        var ofd = new OpenFileDialog
        {
            Filter = "JSON 词库备份 (*.json)|*.json",
            Title = "导入词库与配置"
        };

        if (ofd.ShowDialog() != true) return;

        string filePath = Path.GetFullPath(ofd.FileName);
        string mode = ImportMode;

        if (mode == "replace")
        {
            ConfirmDialogTitle = "危险操作：覆盖导入";
            ConfirmDialogMessage = $"您选择的是【覆盖导入】模式。\n这将会彻底清空现有的所有自定义词条与自学习输入习惯，替换为文件中的数据。\n\n备份文件：{Path.GetFileName(filePath)}\n\n确定要继续执行覆盖导入吗？";
            _pendingConfirmedAction = async () => await ExecuteImportAsync(filePath, mode);
            IsConfirmDialogOpen = true;
        }
        else
        {
            _ = ExecuteImportAsync(filePath, mode);
        }
    }

    private async Task ExecuteImportAsync(string filePath, string mode)
    {
        IsLoading = true;
        try
        {
            await _client.ImportDataAsync(filePath, mode);
            _banner.ShowSuccess($"导入成功 (模式: {(mode == "replace" ? "覆盖" : "增量合并")})");
            await RefreshStatusAsync(showBanner: false);
        }
        catch (TimeoutException)
        {
            // Prompt "结果尚未确认", no auto-retry!
            _banner.ShowWarning("导入超时，结果尚未确认");
        }
        catch (AdminException ex) when (ex.IsBusy)
        {
            _banner.ShowError("服务正忙（存在未确认的提交），无法执行导入");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"导入失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    private void RequestResetStats()
    {
        ConfirmDialogTitle = "危险操作：重置自学习统计";
        ConfirmDialogMessage = "重置操作将彻底清除所有已学习的词频、习惯和动态权重数据，恢复为初始默认词频。\n已添加的自定义词条正文不会被删除。\n\n确定要重置自学习习惯统计吗？";
        _pendingConfirmedAction = ExecuteResetStatsAsync;
        IsConfirmDialogOpen = true;
    }

    private async Task ExecuteResetStatsAsync()
    {
        IsLoading = true;
        try
        {
            await _client.ResetStatsAsync();
            _banner.ShowSuccess("自学习统计与习惯已成功重置");
            await RefreshStatusAsync(showBanner: false);
        }
        catch (TimeoutException)
        {
            // Prompt "结果尚未确认", no auto-retry!
            _banner.ShowWarning("重置超时，结果尚未确认");
        }
        catch (AdminException ex) when (ex.IsBusy)
        {
            _banner.ShowError("服务正忙，无法执行重置");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"重置失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    public async Task ConfirmActionAsync()
    {
        IsConfirmDialogOpen = false;
        if (_pendingConfirmedAction != null)
        {
            var action = _pendingConfirmedAction;
            _pendingConfirmedAction = null;
            await action();
        }
    }

    public void CancelAction()
    {
        IsConfirmDialogOpen = false;
        _pendingConfirmedAction = null;
    }
}
