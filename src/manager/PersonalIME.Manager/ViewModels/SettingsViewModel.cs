using System.Collections.ObjectModel;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public class SettingsViewModel : ViewModelBase
{
    private readonly IAdminClient _client;
    private readonly NotificationBanner _banner;

    private uint _currentRevision = 0;
    private int _candidatePageSize = 5;
    private string _primaryInputMethod = InputSchemes.Wubi86;
    private string _shuangpinLayout = ShuangpinLayouts.Xiaohe;
    private bool _frequencyEnabled = true;
    private bool _learningEnabled = true;

    private bool _initialChinese = true;
    private bool _initialFullWidth = false;
    private bool _initialFullPunct = true;
    private bool _mixedInputEnabled = true;

    private bool _reverseLookupEnabled = false;
    private string _reverseLookupTargetMethod = InputSchemes.Wubi86;
    private string _reverseLookupShuangpinLayout = ShuangpinLayouts.Natural;
    private string _reverseLookupTiming = "always";

    private bool _isLoading;
    public bool IsLoaded { get; private set; }

    private InputMethodPriorityItem? _selectedMethodPriority;
    private DictionaryPriorityItem? _selectedDictPriority;

    public ObservableCollection<InputMethodPriorityItem> MethodPriorities { get; } = [];
    public ObservableCollection<DictionaryPriorityItem> DictionaryPriorities { get; } = [];

    public string[] AvailableSchemes { get; } = [
        InputSchemes.Wubi86,
        InputSchemes.Pinyin,
        InputSchemes.Shuangpin
    ];

    public string[] AvailableLayouts { get; } = ShuangpinLayouts.All;

    public bool MixedInputEnabled
    {
        get => _mixedInputEnabled;
        set => SetProperty(ref _mixedInputEnabled, value);
    }

    public uint CurrentRevision
    {
        get => _currentRevision;
        private set
        {
            if (SetProperty(ref _currentRevision, value))
            {
                OnPropertyChanged(nameof(RevisionDisplay));
            }
        }
    }

    public string RevisionDisplay => CurrentRevision > 0 ? $"配置版本: rev.{CurrentRevision}" : "未加载";

    public int CandidatePageSize
    {
        get => _candidatePageSize;
        set => SetProperty(ref _candidatePageSize, Math.Clamp(value, 5, 9));
    }

    public string PrimaryInputMethod
    {
        get => _primaryInputMethod;
        set
        {
            if (SetProperty(ref _primaryInputMethod, value))
            {
                var match = MethodPriorities.FirstOrDefault(m => m.Scheme == value);
                if (match != null && !match.IsEnabled)
                {
                    match.IsEnabled = true;
                }
            }
        }
    }

    public string ShuangpinLayout
    {
        get => _shuangpinLayout;
        set => SetProperty(ref _shuangpinLayout, value);
    }

    public bool FrequencyEnabled
    {
        get => _frequencyEnabled;
        set => SetProperty(ref _frequencyEnabled, value);
    }

    public bool LearningEnabled
    {
        get => _learningEnabled;
        set => SetProperty(ref _learningEnabled, value);
    }

    public bool InitialChinese
    {
        get => _initialChinese;
        set => SetProperty(ref _initialChinese, value);
    }

    public bool InitialFullWidth
    {
        get => _initialFullWidth;
        set => SetProperty(ref _initialFullWidth, value);
    }

    public bool InitialFullPunct
    {
        get => _initialFullPunct;
        set => SetProperty(ref _initialFullPunct, value);
    }

    public bool IsLoading
    {
        get => _isLoading;
        private set => SetProperty(ref _isLoading, value);
    }

    public InputMethodPriorityItem? SelectedMethodPriority
    {
        get => _selectedMethodPriority;
        set => SetProperty(ref _selectedMethodPriority, value);
    }

    public DictionaryPriorityItem? SelectedDictPriority
    {
        get => _selectedDictPriority;
        set => SetProperty(ref _selectedDictPriority, value);
    }

    public bool ReverseLookupEnabled
    {
        get => _reverseLookupEnabled;
        set => SetProperty(ref _reverseLookupEnabled, value);
    }

    public string ReverseLookupTargetMethod
    {
        get => _reverseLookupTargetMethod;
        set
        {
            if (SetProperty(ref _reverseLookupTargetMethod, value))
            {
                OnPropertyChanged(nameof(IsShuangpinTarget));
            }
        }
    }

    public string ReverseLookupShuangpinLayout
    {
        get => _reverseLookupShuangpinLayout;
        set => SetProperty(ref _reverseLookupShuangpinLayout, value);
    }

    public string ReverseLookupTiming
    {
        get => _reverseLookupTiming;
        set => SetProperty(ref _reverseLookupTiming, value);
    }

    public bool IsShuangpinTarget => ReverseLookupTargetMethod == InputSchemes.Shuangpin;

    public string[] AvailableReverseLookupTimings { get; } = ["always", "composition"];

    public AsyncRelayCommand LoadPolicyCommand { get; }
    public AsyncRelayCommand SavePolicyCommand { get; }
    public RelayCommand MoveMethodUpCommand { get; }
    public RelayCommand MoveMethodDownCommand { get; }
    public RelayCommand MoveDictUpCommand { get; }
    public RelayCommand MoveDictDownCommand { get; }

    public event Action<InputPolicyConfig>? PolicySaved;

    public SettingsViewModel(IAdminClient client, NotificationBanner banner)
    {
        _client = client;
        _banner = banner;

        LoadPolicyCommand = new AsyncRelayCommand(LoadPolicyAsync);
        SavePolicyCommand = new AsyncRelayCommand(SavePolicyAsync);

        MoveMethodUpCommand = new RelayCommand(MoveMethodUp, () => SelectedMethodPriority != null && MethodPriorities.IndexOf(SelectedMethodPriority) > 0);
        MoveMethodDownCommand = new RelayCommand(MoveMethodDown, () => SelectedMethodPriority != null && MethodPriorities.IndexOf(SelectedMethodPriority) < MethodPriorities.Count - 1);

        MoveDictUpCommand = new RelayCommand(MoveDictUp, () => SelectedDictPriority != null && DictionaryPriorities.IndexOf(SelectedDictPriority) > 0);
        MoveDictDownCommand = new RelayCommand(MoveDictDown, () => SelectedDictPriority != null && DictionaryPriorities.IndexOf(SelectedDictPriority) < DictionaryPriorities.Count - 1);

        InitDefaults();
    }

    private void InitDefaults()
    {
        MethodPriorities.Clear();
        MethodPriorities.Add(new InputMethodPriorityItem { Scheme = InputSchemes.Wubi86, IsEnabled = true });
        MethodPriorities.Add(new InputMethodPriorityItem { Scheme = InputSchemes.Pinyin, IsEnabled = true });
        MethodPriorities.Add(new InputMethodPriorityItem { Scheme = InputSchemes.Shuangpin, IsEnabled = true });

        DictionaryPriorities.Clear();
        DictionaryPriorities.Add(new DictionaryPriorityItem { DictId = "dict_custom", IsEnabled = true });
        DictionaryPriorities.Add(new DictionaryPriorityItem { DictId = "dict_builtin_wubi", IsEnabled = true });
        DictionaryPriorities.Add(new DictionaryPriorityItem { DictId = "dict_builtin_pinyin", IsEnabled = true });
    }

    public async Task LoadPolicyAsync()
    {
        IsLoading = true;
        try
        {
            var policy = await _client.GetPolicyAsync();
            CurrentRevision = policy.Revision;
            CandidatePageSize = policy.CandidatePageSize;
            PrimaryInputMethod = policy.PrimaryInputMethod;
            ShuangpinLayout = policy.ShuangpinLayout;
            FrequencyEnabled = policy.FrequencyEnabled;
            LearningEnabled = policy.LearningEnabled;
            MixedInputEnabled = policy.MixedInputEnabled;

            if (policy.InitialMode != null)
            {
                InitialChinese = policy.InitialMode.Chinese;
                InitialFullWidth = policy.InitialMode.FullWidth;
                InitialFullPunct = policy.InitialMode.ChinesePunctuation;
            }

            if (policy.ReverseLookup != null)
            {
                ReverseLookupEnabled = policy.ReverseLookup.Enabled;
                ReverseLookupTargetMethod = policy.ReverseLookup.TargetMethod ?? InputSchemes.Wubi86;
                ReverseLookupShuangpinLayout = ShuangpinLayouts.Canonicalize(policy.ReverseLookup.ShuangpinLayout);
                ReverseLookupTiming = policy.ReverseLookup.Timing ?? "always";
            }

            if (policy.MethodPriorities != null && policy.MethodPriorities.Count > 0)
            {
                MethodPriorities.Clear();
                foreach (var m in policy.MethodPriorities)
                {
                    bool isEnabled = true;
                    if (policy.InputMethods != null && policy.InputMethods.Count > 0)
                    {
                        var found = policy.InputMethods.FirstOrDefault(im => im.Scheme == m);
                        if (found != null) isEnabled = found.Enabled;
                    }
                    else if (policy.EnabledInputMethods != null)
                    {
                        isEnabled = policy.EnabledInputMethods.Contains(m);
                    }
                    MethodPriorities.Add(new InputMethodPriorityItem { Scheme = m, IsEnabled = isEnabled });
                }
            }

            if (policy.DictionaryPriorities != null && policy.DictionaryPriorities.Count > 0)
            {
                DictionaryPriorities.Clear();
                foreach (var d in policy.DictionaryPriorities)
                {
                    DictionaryPriorities.Add(new DictionaryPriorityItem
                    {
                        DictId = d.DictId,
                        IsEnabled = d.IsEnabled
                    });
                }
            }

            IsLoaded = true;
            _banner.ShowInfo($"设置已加载 (rev.{CurrentRevision})");
        }
        catch (TimeoutException)
        {
            _banner.ShowWarning("加载设置超时，结果尚未确认");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"加载设置失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    public void MoveMethodUp()
    {
        if (SelectedMethodPriority == null) return;
        int idx = MethodPriorities.IndexOf(SelectedMethodPriority);
        if (idx > 0)
        {
            MethodPriorities.Move(idx, idx - 1);
        }
    }

    public void MoveMethodDown()
    {
        if (SelectedMethodPriority == null) return;
        int idx = MethodPriorities.IndexOf(SelectedMethodPriority);
        if (idx >= 0 && idx < MethodPriorities.Count - 1)
        {
            MethodPriorities.Move(idx, idx + 1);
        }
    }

    public void MoveDictUp()
    {
        if (SelectedDictPriority == null) return;
        int idx = DictionaryPriorities.IndexOf(SelectedDictPriority);
        if (idx > 0)
        {
            DictionaryPriorities.Move(idx, idx - 1);
        }
    }

    public void MoveDictDown()
    {
        if (SelectedDictPriority == null) return;
        int idx = DictionaryPriorities.IndexOf(SelectedDictPriority);
        if (idx >= 0 && idx < DictionaryPriorities.Count - 1)
        {
            DictionaryPriorities.Move(idx, idx + 1);
        }
    }

    public async Task SavePolicyAsync()
    {
        // Validation 1: At least one input method must be enabled
        if (!MethodPriorities.Any(m => m.IsEnabled))
        {
            _banner.ShowError("至少需要启用一种输入方式");
            return;
        }

        // Validation 2: Primary input method must be enabled
        var primaryItem = MethodPriorities.FirstOrDefault(m => m.Scheme == PrimaryInputMethod);
        if (primaryItem != null && !primaryItem.IsEnabled)
        {
            _banner.ShowError("默认（首选）输入方式必须处于启用状态");
            return;
        }

        IsLoading = true;
        try
        {
            var config = new InputPolicyConfig
            {
                Revision = CurrentRevision,
                CandidatePageSize = CandidatePageSize,
                PrimaryInputMethod = PrimaryInputMethod,
                ShuangpinLayout = ShuangpinLayouts.Canonicalize(ShuangpinLayout),
                FrequencyEnabled = FrequencyEnabled,
                LearningEnabled = LearningEnabled,
                MixedInputEnabled = MixedInputEnabled,
                InitialMode = new InitialModeConfig
                {
                    Chinese = InitialChinese,
                    FullWidth = InitialFullWidth,
                    ChinesePunctuation = InitialFullPunct
                },
                ReverseLookup = new ReverseLookupConfig
                {
                    Enabled = ReverseLookupEnabled,
                    TargetMethod = ReverseLookupTargetMethod,
                    ShuangpinLayout = ShuangpinLayouts.Canonicalize(ReverseLookupShuangpinLayout),
                    Timing = ReverseLookupTiming
                },
                MethodPriorities = MethodPriorities.Select(m => m.Scheme).ToList(),
                EnabledInputMethods = MethodPriorities.Where(m => m.IsEnabled).Select(m => m.Scheme).ToList(),
                InputMethods = MethodPriorities.Select(m => new InputMethodConfigItem
                {
                    Scheme = m.Scheme,
                    Enabled = m.IsEnabled
                }).ToList(),
                DictionaryPriorities = DictionaryPriorities.Select(d => new DictionaryPriorityItem
                {
                    DictId = d.DictId,
                    IsEnabled = d.IsEnabled
                }).ToList()
            };

            uint newRev = await _client.SavePolicyAsync(config, CurrentRevision);
            CurrentRevision = newRev;
            config.Revision = newRev;
            IsLoaded = true;
            _banner.ShowSuccess($"输入设置保存成功 (更新至 rev.{CurrentRevision})");
            PolicySaved?.Invoke(config);
        }
        catch (AdminException ex) when (ex.IsConflict)
        {
            _banner.ShowError("配置已被其他管理窗口修改，请刷新后重试 (并发版本冲突)");
        }
        catch (AdminUnconfirmedException ex)
        {
            _banner.ShowWarning(ex.Message);
        }
        catch (TimeoutException)
        {
            // Preserve settings draft, prompt "结果尚未确认", no auto-retry!
            _banner.ShowWarning("保存设置超时，结果尚未确认");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"保存设置失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }
}

