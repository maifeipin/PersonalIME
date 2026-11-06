using System.Collections.ObjectModel;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public class SortingViewModel : ViewModelBase
{
    private readonly IAdminClient _client;
    private readonly NotificationBanner _banner;

    private string _selectedScheme = InputSchemes.Wubi86;
    private string _selectedLayout = ShuangpinLayouts.Xiaohe;
    private string _queryCode = string.Empty;
    private ulong _currentRevision = 0;
    private bool _isLoading;

    private string? _loadedScheme;
    private string? _loadedLayout;
    private string? _loadedCode;
    private bool _isScopeLoaded;

    private long _loadGeneration = 0;
    private long _searchGeneration = 0;
    private long _draftGeneration = 0;
    private long _loadedDraftGen = 0;

    private PinnedItem? _selectedPinned;
    private string _candidateSearchText = string.Empty;
    private UserEntryItem? _selectedCandidate;

    public ObservableCollection<PinnedItem> PinnedEntries { get; } = [];
    public ObservableCollection<UserEntryItem> AvailableCandidates { get; } = [];

    public string[] AvailableSchemes { get; } = [
        InputSchemes.Wubi86,
        InputSchemes.Pinyin,
        InputSchemes.Shuangpin,
        InputSchemes.Custom
    ];

    public string[] AvailableLayouts { get; } = ShuangpinLayouts.All;

    public string SelectedScheme
    {
        get => _selectedScheme;
        set
        {
            if (SetProperty(ref _selectedScheme, value))
            {
                OnPropertyChanged(nameof(IsShuangpinSelected));
                InvalidateCurrentScope();
            }
        }
    }

    public bool IsShuangpinSelected => SelectedScheme == InputSchemes.Shuangpin;

    public string SelectedLayout
    {
        get => _selectedLayout;
        set
        {
            if (SetProperty(ref _selectedLayout, value))
            {
                InvalidateCurrentScope();
            }
        }
    }

    public string QueryCode
    {
        get => _queryCode;
        set
        {
            if (SetProperty(ref _queryCode, value))
            {
                OnPropertyChanged(nameof(RevisionDisplay));
                OnPropertyChanged(nameof(IsQueryCodeDirty));
                OnPropertyChanged(nameof(ScopeDirtyWarning));
            }
        }
    }

    private void InvalidateCurrentScope()
    {
        Interlocked.Increment(ref _draftGeneration);
        Interlocked.Increment(ref _loadGeneration);
        Interlocked.Increment(ref _searchGeneration);
        _isScopeLoaded = false;
        _loadedDraftGen = 0;
        _loadedScheme = null;
        _loadedLayout = null;
        _loadedCode = null;
        PinnedEntries.Clear();
        AvailableCandidates.Clear();
        CurrentRevision = 0;
        IsLoading = false;
        SaveSortOrderCommand?.RaiseCanExecuteChanged();
        OnPropertyChanged(nameof(RevisionDisplay));
        OnPropertyChanged(nameof(IsQueryCodeDirty));
        OnPropertyChanged(nameof(ScopeDirtyWarning));
    }

    public ulong CurrentRevision
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

    public bool IsQueryCodeDirty => _isScopeLoaded &&
        QueryCode.Trim().ToLowerInvariant() != _loadedCode;

    public string ScopeDirtyWarning => IsQueryCodeDirty
        ? $"⚠️ 当前列表归属于已加载的 {_loadedCode}，未重新加载输入框中的 {QueryCode}"
        : string.Empty;

    public string RevisionDisplay
    {
        get
        {
            if (!_isScopeLoaded || string.IsNullOrEmpty(_loadedCode)) return "未加载";
            string layoutInfo = !string.IsNullOrEmpty(_loadedLayout) ? $" [{_loadedLayout}]" : "";
            string dirtyMark = (QueryCode.Trim().ToLowerInvariant() != _loadedCode) ? " (未重新加载)" : "";
            return $"{_loadedScheme}{layoutInfo}:{_loadedCode} (rev.{CurrentRevision}){dirtyMark}";
        }
    }

    public bool IsLoading
    {
        get => _isLoading;
        private set => SetProperty(ref _isLoading, value);
    }

    public PinnedItem? SelectedPinned
    {
        get => _selectedPinned;
        set => SetProperty(ref _selectedPinned, value);
    }

    public string CandidateSearchText
    {
        get => _candidateSearchText;
        set => SetProperty(ref _candidateSearchText, value);
    }

    public UserEntryItem? SelectedCandidate
    {
        get => _selectedCandidate;
        set => SetProperty(ref _selectedCandidate, value);
    }

    public AsyncRelayCommand LoadPinnedCommand { get; }
    public AsyncRelayCommand SaveSortOrderCommand { get; }
    public RelayCommand MoveUpCommand { get; }
    public RelayCommand MoveDownCommand { get; }
    public RelayCommand RemovePinnedCommand { get; }
    public AsyncRelayCommand SearchCandidatesCommand { get; }
    public RelayCommand AddCandidateToPinnedCommand { get; }

    public SortingViewModel(IAdminClient client, NotificationBanner banner)
    {
        _client = client;
        _banner = banner;

        LoadPinnedCommand = new AsyncRelayCommand(LoadPinnedAsync);
        SaveSortOrderCommand = new AsyncRelayCommand(SaveSortOrderAsync, () => _isScopeLoaded);
        MoveUpCommand = new RelayCommand(MoveUp, () => SelectedPinned != null && PinnedEntries.IndexOf(SelectedPinned) > 0);
        MoveDownCommand = new RelayCommand(MoveDown, () => SelectedPinned != null && PinnedEntries.IndexOf(SelectedPinned) < PinnedEntries.Count - 1);
        RemovePinnedCommand = new RelayCommand(RemovePinned, () => SelectedPinned != null);
        SearchCandidatesCommand = new AsyncRelayCommand(SearchCandidatesAsync);
        AddCandidateToPinnedCommand = new RelayCommand(AddCandidateToPinned, () => SelectedCandidate != null);
    }

    public async Task LoadPinnedAsync()
    {
        string code = QueryCode.Trim().ToLowerInvariant();
        if (string.IsNullOrEmpty(code))
        {
            _banner.ShowWarning("请输入要查询的编码");
            return;
        }

        string scheme = SelectedScheme;
        string? layout = IsShuangpinSelected ? ShuangpinLayouts.Canonicalize(SelectedLayout) : null;
        long gen = Interlocked.Increment(ref _loadGeneration);
        // A refresh supersedes earlier save callbacks as soon as it starts,
        // including while its response is still pending.
        _loadedDraftGen = Interlocked.Increment(ref _draftGeneration);

        IsLoading = true;
        try
        {
            var res = await _client.GetPinnedAsync(scheme, code, layout);

            // Generation and scope identity check
            if (Interlocked.Read(ref _loadGeneration) != gen ||
                SelectedScheme != scheme ||
                QueryCode.Trim().ToLowerInvariant() != code ||
                (IsShuangpinSelected ? ShuangpinLayouts.Canonicalize(SelectedLayout) : null) != layout)
            {
                return;
            }

            long draftGen = Interlocked.Increment(ref _draftGeneration);
            _loadedDraftGen = draftGen;
            _loadedScheme = scheme;
            _loadedLayout = layout;
            _loadedCode = code;
            _isScopeLoaded = true;

            PinnedEntries.Clear();
            for (int i = 0; i < res.Entries.Count; i++)
            {
                var entry = res.Entries[i];
                entry.OrderRank = i + 1;
                PinnedEntries.Add(entry);
            }
            CurrentRevision = res.Revision;
            SaveSortOrderCommand.RaiseCanExecuteChanged();
            OnPropertyChanged(nameof(RevisionDisplay));
            OnPropertyChanged(nameof(IsQueryCodeDirty));
            OnPropertyChanged(nameof(ScopeDirtyWarning));
            _banner.ShowInfo($"已加载 {PinnedEntries.Count} 个置顶条目 (rev.{CurrentRevision})");

            // Also auto-populate candidate search without keyword filter
            CandidateSearchText = string.Empty;
            await SearchCandidatesAsync();
        }
        catch (TimeoutException)
        {
            if (Interlocked.Read(ref _loadGeneration) == gen)
            {
                _banner.ShowWarning("加载置顶条目超时，结果尚未确认");
            }
        }
        catch (Exception ex)
        {
            if (Interlocked.Read(ref _loadGeneration) == gen)
            {
                _banner.ShowError($"加载失败: {ex.Message}");
            }
        }
        finally
        {
            if (Interlocked.Read(ref _loadGeneration) == gen)
            {
                IsLoading = false;
            }
        }
    }

    public void MoveUp()
    {
        if (SelectedPinned == null) return;
        int idx = PinnedEntries.IndexOf(SelectedPinned);
        if (idx > 0)
        {
            PinnedEntries.Move(idx, idx - 1);
            UpdateRanks();
        }
    }

    public void MoveDown()
    {
        if (SelectedPinned == null) return;
        int idx = PinnedEntries.IndexOf(SelectedPinned);
        if (idx >= 0 && idx < PinnedEntries.Count - 1)
        {
            PinnedEntries.Move(idx, idx + 1);
            UpdateRanks();
        }
    }

    public void RemovePinned()
    {
        if (SelectedPinned == null) return;
        PinnedEntries.Remove(SelectedPinned);
        UpdateRanks();
        SaveSortOrderCommand.RaiseCanExecuteChanged();
    }

    private void UpdateRanks()
    {
        for (int i = 0; i < PinnedEntries.Count; i++)
        {
            PinnedEntries[i].OrderRank = i + 1;
        }
        OnPropertyChanged(nameof(PinnedEntries));
    }

    public async Task SearchCandidatesAsync()
    {
        string kw = CandidateSearchText.Trim();
        string code = _loadedCode ?? QueryCode.Trim().ToLowerInvariant();
        if (string.IsNullOrEmpty(code) && string.IsNullOrEmpty(kw)) return;

        string scheme = _loadedScheme ?? SelectedScheme;
        string? layout = _loadedLayout ?? (IsShuangpinSelected ? ShuangpinLayouts.Canonicalize(SelectedLayout) : null);
        long gen = Interlocked.Increment(ref _searchGeneration);

        try
        {
            var cands = await _client.GetScopeCandidatesAsync(scheme, string.IsNullOrEmpty(code) ? kw : code, layout, kw);
            if (Interlocked.Read(ref _searchGeneration) != gen)
            {
                return;
            }

            AvailableCandidates.Clear();
            foreach (var sc in cands)
            {
                AvailableCandidates.Add(new UserEntryItem
                {
                    Id = sc.EntryId,
                    Text = sc.Text,
                    Codes = [new EntryCodeItem { Scheme = scheme, Code = sc.MatchedCode }]
                });
            }
        }
        catch (Exception)
        {
            // Optional lookup
        }
    }

    public void AddCandidateToPinned()
    {
        if (SelectedCandidate == null) return;

        if (PinnedEntries.Any(p => p.EntryId == SelectedCandidate.Id))
        {
            _banner.ShowWarning("该词条已在置顶列表中");
            return;
        }

        var pinned = new PinnedItem
        {
            EntryId = SelectedCandidate.Id,
            Text = SelectedCandidate.Text,
            OrderRank = PinnedEntries.Count + 1,
            Scheme = _loadedScheme ?? SelectedScheme,
            Code = _loadedCode ?? QueryCode.Trim().ToLowerInvariant(),
            LayoutId = _loadedLayout ?? (IsShuangpinSelected ? ShuangpinLayouts.Canonicalize(SelectedLayout) : null)
        };
        PinnedEntries.Add(pinned);
        SaveSortOrderCommand.RaiseCanExecuteChanged();
    }

    public async Task SaveSortOrderAsync()
    {
        if (!_isScopeLoaded || string.IsNullOrEmpty(_loadedCode))
        {
            _banner.ShowWarning("未加载有效的作用域排序");
            return;
        }

        long draftGen = _loadedDraftGen;
        string code = _loadedCode;
        string scheme = _loadedScheme ?? SelectedScheme;
        string? layout = _loadedLayout;
        ulong expectedRev = CurrentRevision;
        var savedSnapshot = PinnedEntries.Select(p => p.EntryId).ToList();

        IsLoading = true;
        try
        {
            ulong newRev = await _client.ReorderPinnedAsync(
                scheme,
                code,
                savedSnapshot,
                layout,
                expectedRevision: expectedRev);

            // Scope / Draft generation check: was this draft reloaded, switched, or invalidated?
            if (!_isScopeLoaded || _loadedDraftGen != draftGen)
            {
                return;
            }

            CurrentRevision = newRev;
            OnPropertyChanged(nameof(RevisionDisplay));

            bool matchesCurrent = PinnedEntries.Select(p => p.EntryId).SequenceEqual(savedSnapshot);
            if (matchesCurrent)
            {
                _banner.ShowSuccess($"排序保存成功 (更新至 rev.{CurrentRevision})");
            }
            else
            {
                _banner.ShowInfo($"此前提交已确认 (rev.{CurrentRevision})，当前列表包含尚未保存的后续修改");
            }
        }
        catch (AdminException ex) when (ex.IsConflict)
        {
            if (_isScopeLoaded && _loadedDraftGen == draftGen)
            {
                _banner.ShowError("排序数据已被其他管理窗口修改，请刷新后重试 (并发版本冲突)");
            }
        }
        catch (AdminUnconfirmedException ex)
        {
            if (_isScopeLoaded && _loadedDraftGen == draftGen)
            {
                _banner.ShowWarning(ex.Message);
            }
        }
        catch (TimeoutException)
        {
            if (_isScopeLoaded && _loadedDraftGen == draftGen)
            {
                // Preserve draft in list, prompt "结果尚未确认", no auto-retry!
                _banner.ShowWarning("保存排序超时，结果尚未确认");
            }
        }
        catch (Exception ex)
        {
            if (_isScopeLoaded && _loadedDraftGen == draftGen)
            {
                _banner.ShowError($"保存排序失败: {ex.Message}");
            }
        }
        finally
        {
            if (_isScopeLoaded && _loadedDraftGen == draftGen)
            {
                IsLoading = false;
            }
        }
    }
}

