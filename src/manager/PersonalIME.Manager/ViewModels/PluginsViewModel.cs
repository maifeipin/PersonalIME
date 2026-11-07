using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Input;
using Microsoft.Win32;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public class PluginsViewModel : ViewModelBase
{
    private readonly IAdminClient _client;
    private readonly NotificationBanner _banner;

    private ObservableCollection<PluginModel> _plugins = [];
    private ObservableCollection<PluginLogModel> _logs = [];
    private PluginModel? _selectedPlugin;

    // Form inputs for Add/Edit
    private bool _isEditing;
    private long _editingId;
    private string _formName = string.Empty;
    private string _formIcon = "🧩";
    private string _formExecPath = string.Empty;
    private string _formArguments = string.Empty;
    private bool _formIsEnabled = true;
    private int _formSortOrder;
    private bool _isLoading;

    public PluginsViewModel(IAdminClient client, NotificationBanner banner)
    {
        _client = client;
        _banner = banner;

        RefreshCommand = new AsyncRelayCommand(async _ => await LoadDataAsync());
        NewPluginCommand = new RelayCommand(_ => PrepareNewPlugin());
        EditPluginCommand = new RelayCommand(_ => PrepareEditPlugin(), _ => SelectedPlugin != null);
        SaveFormCommand = new AsyncRelayCommand(async _ => await SaveFormAsync());
        CancelFormCommand = new RelayCommand(_ => CancelForm());
        DeletePluginCommand = new AsyncRelayCommand(async _ => await DeletePluginAsync(), _ => SelectedPlugin != null);
        ExecutePluginCommand = new AsyncRelayCommand(async param => await ExecutePluginAsync(param as PluginModel ?? SelectedPlugin), _ => (SelectedPlugin != null));
        ToggleEnabledCommand = new AsyncRelayCommand(async param => await ToggleEnabledAsync(param as PluginModel));
        ClearLogsCommand = new AsyncRelayCommand(async _ => await ClearLogsAsync());
        BrowsePathCommand = new RelayCommand(_ => BrowseExecutablePath());
    }

    public ObservableCollection<PluginModel> Plugins
    {
        get => _plugins;
        set => SetProperty(ref _plugins, value);
    }

    public ObservableCollection<PluginLogModel> Logs
    {
        get => _logs;
        set => SetProperty(ref _logs, value);
    }

    public PluginModel? SelectedPlugin
    {
        get => _selectedPlugin;
        set
        {
            if (SetProperty(ref _selectedPlugin, value))
            {
                (EditPluginCommand as RelayCommand)?.RaiseCanExecuteChanged();
                (DeletePluginCommand as AsyncRelayCommand)?.RaiseCanExecuteChanged();
                (ExecutePluginCommand as AsyncRelayCommand)?.RaiseCanExecuteChanged();
                if (value != null && !_isEditing)
                {
                    PopulateFormFromModel(value);
                }
            }
        }
    }

    public bool IsEditing
    {
        get => _isEditing;
        set => SetProperty(ref _isEditing, value);
    }

    public string FormName
    {
        get => _formName;
        set => SetProperty(ref _formName, value);
    }

    public string FormIcon
    {
        get => _formIcon;
        set => SetProperty(ref _formIcon, value);
    }

    public string FormExecPath
    {
        get => _formExecPath;
        set => SetProperty(ref _formExecPath, value);
    }

    public string FormArguments
    {
        get => _formArguments;
        set => SetProperty(ref _formArguments, value);
    }

    public bool FormIsEnabled
    {
        get => _formIsEnabled;
        set => SetProperty(ref _formIsEnabled, value);
    }

    public int FormSortOrder
    {
        get => _formSortOrder;
        set => SetProperty(ref _formSortOrder, value);
    }

    public bool IsLoading
    {
        get => _isLoading;
        set => SetProperty(ref _isLoading, value);
    }

    public ICommand RefreshCommand { get; }
    public ICommand NewPluginCommand { get; }
    public ICommand EditPluginCommand { get; }
    public ICommand SaveFormCommand { get; }
    public ICommand CancelFormCommand { get; }
    public ICommand DeletePluginCommand { get; }
    public ICommand ExecutePluginCommand { get; }
    public ICommand ToggleEnabledCommand { get; }
    public ICommand ClearLogsCommand { get; }
    public ICommand BrowsePathCommand { get; }

    public async Task LoadDataAsync()
    {
        if (IsLoading) return;
        IsLoading = true;
        try
        {
            var pluginsList = await _client.GetPluginsAsync(false);
            Plugins = new ObservableCollection<PluginModel>(pluginsList);

            var logsList = await _client.GetPluginLogsAsync(null, 50);
            Logs = new ObservableCollection<PluginLogModel>(logsList);

            if (SelectedPlugin == null && Plugins.Count > 0)
            {
                SelectedPlugin = Plugins[0];
            }
        }
        catch (Exception ex)
        {
            _banner.ShowError($"加载插件列表失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    public void PrepareNewPlugin()
    {
        _editingId = 0;
        FormName = "";
        FormIcon = "🧩";
        FormExecPath = "";
        FormArguments = "";
        FormIsEnabled = true;
        FormSortOrder = Plugins.Count;
        IsEditing = true;
    }

    public void PrepareEditPlugin()
    {
        if (SelectedPlugin == null) return;
        PopulateFormFromModel(SelectedPlugin);
        IsEditing = true;
    }

    private void PopulateFormFromModel(PluginModel p)
    {
        _editingId = p.Id;
        FormName = p.Name;
        FormIcon = p.Icon;
        FormExecPath = p.ExecPath;
        FormArguments = p.Arguments;
        FormIsEnabled = p.IsEnabled;
        FormSortOrder = p.SortOrder;
    }

    public void CancelForm()
    {
        IsEditing = false;
        if (SelectedPlugin != null)
        {
            PopulateFormFromModel(SelectedPlugin);
        }
    }

    public async Task SaveFormAsync()
    {
        if (string.IsNullOrWhiteSpace(FormName))
        {
            _banner.ShowWarning("插件名称不能为空");
            return;
        }
        if (string.IsNullOrWhiteSpace(FormExecPath))
        {
            _banner.ShowWarning("执行路径不能为空");
            return;
        }

        var model = new PluginModel
        {
            Id = _editingId,
            Name = FormName.Trim(),
            Icon = string.IsNullOrWhiteSpace(FormIcon) ? "🧩" : FormIcon.Trim(),
            ExecPath = FormExecPath.Trim(),
            Arguments = FormArguments.Trim(),
            IsEnabled = FormIsEnabled,
            SortOrder = FormSortOrder
        };

        try
        {
            if (_editingId == 0)
            {
                long newId = await _client.AddPluginAsync(model);
                model.Id = newId;
                _banner.ShowSuccess($"插件 '{model.Name}' 添加成功");
            }
            else
            {
                await _client.UpdatePluginAsync(model);
                _banner.ShowSuccess($"插件 '{model.Name}' 更新成功");
            }
            IsEditing = false;
            await LoadDataAsync();
        }
        catch (Exception ex)
        {
            _banner.ShowError($"保存插件失败: {ex.Message}");
        }
    }

    public async Task DeletePluginAsync()
    {
        if (SelectedPlugin == null) return;
        var p = SelectedPlugin;
        try
        {
            await _client.DeletePluginAsync(p.Id);
            _banner.ShowSuccess($"插件 '{p.Name}' 已删除");
            SelectedPlugin = null;
            await LoadDataAsync();
        }
        catch (Exception ex)
        {
            _banner.ShowError($"删除插件失败: {ex.Message}");
        }
    }

    public async Task ExecutePluginAsync(PluginModel? plugin)
    {
        if (plugin == null) return;
        try
        {
            await _client.ExecutePluginAsync(plugin.Id);
            _banner.ShowSuccess($"已触发插件: {plugin.Name}");
            // Reload logs after a brief moment
            await Task.Delay(250);
            var logsList = await _client.GetPluginLogsAsync(null, 50);
            Logs = new ObservableCollection<PluginLogModel>(logsList);
        }
        catch (Exception ex)
        {
            _banner.ShowError($"执行插件失败: {ex.Message}");
        }
    }

    public async Task ToggleEnabledAsync(PluginModel? plugin)
    {
        if (plugin == null) return;
        plugin.IsEnabled = !plugin.IsEnabled;
        try
        {
            await _client.UpdatePluginAsync(plugin);
            _banner.ShowSuccess($"插件 '{plugin.Name}' 状态已更新为: {(plugin.IsEnabled ? "启用" : "禁用")}");
            await LoadDataAsync();
        }
        catch (Exception ex)
        {
            _banner.ShowError($"更新插件状态失败: {ex.Message}");
        }
    }

    public async Task ClearLogsAsync()
    {
        try
        {
            await _client.ClearPluginLogsAsync(null);
            Logs.Clear();
            _banner.ShowSuccess("执行日志已清空");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"清空日志失败: {ex.Message}");
        }
    }

    private void BrowseExecutablePath()
    {
        var dlg = new OpenFileDialog
        {
            Title = "选择插件可执行程序",
            Filter = "应用程序 (*.exe;*.cmd;*.bat)|*.exe;*.cmd;*.bat|所有文件 (*.*)|*.*"
        };
        if (dlg.ShowDialog() == true)
        {
            FormExecPath = dlg.FileName;
            if (string.IsNullOrWhiteSpace(FormName))
            {
                FormName = Path.GetFileNameWithoutExtension(dlg.FileName);
            }
        }
    }
}

