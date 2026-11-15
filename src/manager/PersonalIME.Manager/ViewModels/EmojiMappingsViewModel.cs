using System.Collections.ObjectModel;
using System.Windows.Input;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public class EmojiMappingsViewModel : ViewModelBase
{
    private readonly IAdminClient _client;
    private readonly NotificationBanner _banner;

    private EmojiMappingModel? _selectedMapping;
    private string _editTriggerType = "text";
    private string _editTriggerKey = string.Empty;
    private string _editEmoji = string.Empty;
    private string _editLabel = string.Empty;
    private int _editPriority = 200;
    private bool _editEnabled = true;
    private bool _isAssociationEnabled = true;
    private bool _confirmedAssociationEnabled = true;
    private bool? _pendingAssociationTarget;
    private Task? _associationSaveLoopTask;
    private bool _isSavingAssociationEnabled;
    private bool _suppressToggleCallback;
    private bool _isLoading;

    public EmojiMappingsViewModel(IAdminClient client, NotificationBanner banner)
    {
        _client = client;
        _banner = banner;

        RefreshCommand = new AsyncRelayCommand(LoadDataAsync);
        NewCommand = new RelayCommand(ClearEditor);
        SaveCommand = new AsyncRelayCommand(SaveAsync);
        DeleteCommand = new AsyncRelayCommand(DeleteSelectedAsync, () => SelectedMapping != null);
    }

    public ObservableCollection<EmojiMappingModel> Mappings { get; } = [];

    public List<string> TriggerTypes { get; } = ["text", "code"];

    public EmojiMappingModel? SelectedMapping
    {
        get => _selectedMapping;
        set
        {
            if (SetProperty(ref _selectedMapping, value))
            {
                if (value != null)
                {
                    EditTriggerType = value.TriggerType;
                    EditTriggerKey = value.TriggerKey;
                    EditEmoji = value.Emoji;
                    EditLabel = value.Label;
                    EditPriority = value.Priority;
                    EditEnabled = value.Enabled;
                }
                (DeleteCommand as AsyncRelayCommand)?.RaiseCanExecuteChanged();
            }
        }
    }

    public string EditTriggerType
    {
        get => _editTriggerType;
        set => SetProperty(ref _editTriggerType, value);
    }

    public string EditTriggerKey
    {
        get => _editTriggerKey;
        set => SetProperty(ref _editTriggerKey, value);
    }

    public string EditEmoji
    {
        get => _editEmoji;
        set => SetProperty(ref _editEmoji, value);
    }

    public string EditLabel
    {
        get => _editLabel;
        set => SetProperty(ref _editLabel, value);
    }

    public int EditPriority
    {
        get => _editPriority;
        set => SetProperty(ref _editPriority, value);
    }

    public bool EditEnabled
    {
        get => _editEnabled;
        set => SetProperty(ref _editEnabled, value);
    }

    public bool IsAssociationEnabled
    {
        get => _isAssociationEnabled;
        set
        {
            if (SetProperty(ref _isAssociationEnabled, value) && !_suppressToggleCallback)
            {
                EnqueueAssociationEnabledSave(value);
            }
        }
    }

    public bool IsSavingAssociationEnabled
    {
        get => _isSavingAssociationEnabled;
        private set => SetProperty(ref _isSavingAssociationEnabled, value);
    }

    public Task PendingAssociationSaveTask => _associationSaveLoopTask ?? Task.CompletedTask;

    public bool IsLoading
    {
        get => _isLoading;
        set => SetProperty(ref _isLoading, value);
    }

    public ICommand RefreshCommand { get; }
    public ICommand NewCommand { get; }
    public ICommand SaveCommand { get; }
    public ICommand DeleteCommand { get; }

    public async Task LoadDataAsync()
    {
        if (!_client.IsConnected) return;
        IsLoading = true;
        try
        {
            var res = await _client.GetEmojiMappingsAsync();
            _confirmedAssociationEnabled = res.AssociationEnabled;
            SetAssociationEnabledSilently(res.AssociationEnabled);
            Mappings.Clear();
            foreach (var item in res.Mappings)
            {
                Mappings.Add(item);
            }
        }
        catch (Exception ex)
        {
            _banner.ShowError($"加载表情映射失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    private void ClearEditor()
    {
        SelectedMapping = null;
        EditTriggerType = "text";
        EditTriggerKey = string.Empty;
        EditEmoji = string.Empty;
        EditLabel = string.Empty;
        EditPriority = 200;
        EditEnabled = true;
    }

    private async Task SaveAsync()
    {
        string key = EditTriggerKey.Trim();
        string emoji = EditEmoji.Trim();
        if (string.IsNullOrEmpty(key) || string.IsNullOrEmpty(emoji))
        {
            _banner.ShowWarning("请填写触发词/编码以及目标表情。");
            return;
        }

        try
        {
            var model = new EmojiMappingModel
            {
                Id = SelectedMapping?.Id ?? 0,
                TriggerType = EditTriggerType == "code" ? "code" : "text",
                TriggerKey = key,
                Emoji = emoji,
                Label = EditLabel.Trim(),
                Priority = EditPriority,
                Enabled = EditEnabled
            };

            if (model.Id > 0)
            {
                await _client.UpdateEmojiMappingAsync(model);
                _banner.ShowSuccess("表情联想规则已更新。");
            }
            else
            {
                await _client.AddEmojiMappingAsync(model);
                _banner.ShowSuccess("表情联想规则已添加。");
            }
            await LoadDataAsync();
        }
        catch (Exception ex)
        {
            _banner.ShowError($"保存表情联想规则失败: {ex.Message}");
        }
    }

    private async Task DeleteSelectedAsync()
    {
        if (SelectedMapping == null) return;
        try
        {
            await _client.DeleteEmojiMappingAsync(SelectedMapping.Id);
            _banner.ShowSuccess("已删除表情联想规则。");
            ClearEditor();
            await LoadDataAsync();
        }
        catch (Exception ex)
        {
            _banner.ShowError($"删除表情联想规则失败: {ex.Message}");
        }
    }

    private void SetAssociationEnabledSilently(bool value)
    {
        _suppressToggleCallback = true;
        try
        {
            IsAssociationEnabled = value;
        }
        finally
        {
            _suppressToggleCallback = false;
        }
    }

    private void EnqueueAssociationEnabledSave(bool target)
    {
        if (!_client.IsConnected)
        {
            _pendingAssociationTarget = null;
            SetAssociationEnabledSilently(_confirmedAssociationEnabled);
            _banner.ShowWarning("未连接到输入法服务，无法保存表情联想开关。");
            return;
        }

        _pendingAssociationTarget = target;
        if (_associationSaveLoopTask == null || _associationSaveLoopTask.IsCompleted)
        {
            _associationSaveLoopTask = DrainAssociationSaveQueueAsync();
        }
    }

    private async Task DrainAssociationSaveQueueAsync()
    {
        IsSavingAssociationEnabled = true;
        try
        {
            while (_pendingAssociationTarget is bool target)
            {
                _pendingAssociationTarget = null;
                if (!_client.IsConnected)
                {
                    SetAssociationEnabledSilently(_confirmedAssociationEnabled);
                    _banner.ShowWarning("与输入法服务断开连接，表情联想开关已恢复为已确认状态。");
                    break;
                }

                try
                {
                    await _client.SetEmojiAssociationEnabledAsync(target);
                    _confirmedAssociationEnabled = target;
                    if (_pendingAssociationTarget == null)
                    {
                        SetAssociationEnabledSilently(target);
                        _banner.ShowSuccess(target ? "已开启打字流末尾表情联想。" : "已关闭打字流末尾表情联想。");
                    }
                }
                catch (Exception ex)
                {
                    if (_pendingAssociationTarget == null)
                    {
                        SetAssociationEnabledSilently(_confirmedAssociationEnabled);
                    }
                    _banner.ShowError($"更新表情联想开关失败: {ex.Message}");
                    await ReconcileAssociationStateFromServerAsync();
                }
            }
        }
        finally
        {
            IsSavingAssociationEnabled = false;
        }
    }

    private async Task ReconcileAssociationStateFromServerAsync()
    {
        if (!_client.IsConnected)
        {
            return;
        }
        try
        {
            var res = await _client.GetEmojiMappingsAsync();
            _confirmedAssociationEnabled = res.AssociationEnabled;
            if (_pendingAssociationTarget == null)
            {
                SetAssociationEnabledSilently(res.AssociationEnabled);
            }
        }
        catch
        {
            // Already reverted to last confirmed state and reported the primary failure.
        }
    }
}
