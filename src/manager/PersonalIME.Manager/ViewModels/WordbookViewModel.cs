using System.Collections.ObjectModel;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public class WordbookViewModel : ViewModelBase
{
    private readonly IAdminClient _client;
    private readonly NotificationBanner _banner;

    private string _searchKeyword = string.Empty;
    private string _selectedSchemeFilter = string.Empty;
    private int _currentPage = 0;
    private int _totalPages = 1;
    private int _totalCount = 0;
    private const int PageSize = 20;
    private bool _isLoading;

    private UserEntryItem? _selectedEntry;
    private bool _isEditorOpen;
    private string _editorTitle = "新增自定义词条";
    private string _editorEntryId = string.Empty;
    private string _editorText = string.Empty;
    private string _newCodeScheme = InputSchemes.Wubi86;
    private string _newCodeText = string.Empty;
    private string _reverseLookupPreview = string.Empty;

    private CancellationTokenSource? _searchCts;
    private CancellationTokenSource? _reverseLookupCts;
    private long _editorSessionToken = 0;
    private long _reverseLookupGeneration = 0;

    public ObservableCollection<UserEntryItem> Entries { get; } = [];
    public ObservableCollection<EntryCodeItem> EditorCodes { get; } = [];

    public string SearchKeyword
    {
        get => _searchKeyword;
        set
        {
            if (SetProperty(ref _searchKeyword, value))
            {
                _currentPage = 0;
                _ = LoadEntriesAsync();
            }
        }
    }

    public string SelectedSchemeFilter
    {
        get => _selectedSchemeFilter;
        set
        {
            if (SetProperty(ref _selectedSchemeFilter, value))
            {
                _currentPage = 0;
                _ = LoadEntriesAsync();
            }
        }
    }

    public int CurrentPage
    {
        get => _currentPage;
        private set
        {
            if (SetProperty(ref _currentPage, value))
            {
                OnPropertyChanged(nameof(PageDisplay));
            }
        }
    }

    public int TotalPages
    {
        get => _totalPages;
        private set
        {
            if (SetProperty(ref _totalPages, value))
            {
                OnPropertyChanged(nameof(PageDisplay));
            }
        }
    }

    public int TotalCount
    {
        get => _totalCount;
        private set
        {
            if (SetProperty(ref _totalCount, value))
            {
                OnPropertyChanged(nameof(PageDisplay));
            }
        }
    }

    public string PageDisplay => $"第 {CurrentPage + 1} / {Math.Max(1, TotalPages)} 页 (共 {TotalCount} 条)";

    public bool IsLoading
    {
        get => _isLoading;
        private set => SetProperty(ref _isLoading, value);
    }

    public UserEntryItem? SelectedEntry
    {
        get => _selectedEntry;
        set => SetProperty(ref _selectedEntry, value);
    }

    public bool IsEditorOpen
    {
        get => _isEditorOpen;
        set => SetProperty(ref _isEditorOpen, value);
    }

    public string EditorTitle
    {
        get => _editorTitle;
        set => SetProperty(ref _editorTitle, value);
    }

    public string EditorEntryId
    {
        get => _editorEntryId;
        set => SetProperty(ref _editorEntryId, value);
    }

    public string EditorText
    {
        get => _editorText;
        set
        {
            if (SetProperty(ref _editorText, value))
            {
                Interlocked.Increment(ref _reverseLookupGeneration);
                _reverseLookupCts?.Cancel();
                ReverseLookupPreview = string.Empty;
            }
        }
    }

    public string NewCodeScheme
    {
        get => _newCodeScheme;
        set
        {
            if (SetProperty(ref _newCodeScheme, value))
            {
                Interlocked.Increment(ref _reverseLookupGeneration);
                _reverseLookupCts?.Cancel();
                ReverseLookupPreview = string.Empty;
            }
        }
    }

    public string NewCodeText
    {
        get => _newCodeText;
        set => SetProperty(ref _newCodeText, value);
    }

    public string ReverseLookupPreview
    {
        get => _reverseLookupPreview;
        set => SetProperty(ref _reverseLookupPreview, value);
    }

    public AsyncRelayCommand SearchCommand { get; }
    public AsyncRelayCommand RefreshCommand { get; }
    public AsyncRelayCommand NextPageCommand { get; }
    public AsyncRelayCommand PrevPageCommand { get; }
    public RelayCommand OpenAddDialogCommand { get; }
    public RelayCommand OpenEditDialogCommand { get; }
    public RelayCommand CloseEditorCommand { get; }
    public RelayCommand AddCodeToDraftCommand { get; }
    public RelayCommand RemoveCodeFromDraftCommand { get; }
    public AsyncRelayCommand CheckReverseLookupCommand { get; }
    public AsyncRelayCommand SaveEditorCommand { get; }
    public AsyncRelayCommand DeleteEntryCommand { get; }

    public WordbookViewModel(IAdminClient client, NotificationBanner banner)
    {
        _client = client;
        _banner = banner;

        SearchCommand = new AsyncRelayCommand(LoadEntriesAsync);
        RefreshCommand = new AsyncRelayCommand(LoadEntriesAsync);
        NextPageCommand = new AsyncRelayCommand(async () =>
        {
            if (CurrentPage + 1 < TotalPages)
            {
                CurrentPage++;
                await LoadEntriesAsync();
            }
        }, () => CurrentPage + 1 < TotalPages);

        PrevPageCommand = new AsyncRelayCommand(async () =>
        {
            if (CurrentPage > 0)
            {
                CurrentPage--;
                await LoadEntriesAsync();
            }
        }, () => CurrentPage > 0);

        OpenAddDialogCommand = new RelayCommand(OpenAddDialog);
        OpenEditDialogCommand = new RelayCommand(OpenEditDialog, () => SelectedEntry != null);
        CloseEditorCommand = new RelayCommand(CloseEditor);
        AddCodeToDraftCommand = new RelayCommand(AddCodeToDraft);
        RemoveCodeFromDraftCommand = new RelayCommand(RemoveCodeFromDraft);
        CheckReverseLookupCommand = new AsyncRelayCommand(CheckReverseLookupAsync);
        SaveEditorCommand = new AsyncRelayCommand(SaveEditorAsync);
        DeleteEntryCommand = new AsyncRelayCommand(DeleteEntryAsync, () => SelectedEntry != null);
    }

    public async Task LoadEntriesAsync()
    {
        _searchCts?.Cancel();
        _searchCts?.Dispose();
        _searchCts = new CancellationTokenSource();
        var ct = _searchCts.Token;

        IsLoading = true;
        try
        {
            var res = await _client.GetEntriesAsync(
                string.IsNullOrWhiteSpace(_searchKeyword) ? null : _searchKeyword,
                string.IsNullOrWhiteSpace(_selectedSchemeFilter) ? null : _selectedSchemeFilter,
                CurrentPage,
                PageSize,
                ct);

            if (ct.IsCancellationRequested) return;

            Entries.Clear();
            foreach (var item in res.Entries)
            {
                Entries.Add(item);
            }
            TotalCount = res.Total;
            TotalPages = (int)Math.Ceiling((double)res.Total / PageSize);
        }
        catch (OperationCanceledException)
        {
            // Discard superseded query quietly
        }
        catch (TimeoutException)
        {
            _banner.ShowWarning("加载词库超时，结果尚未确认");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"加载词库失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    private void CloseEditor()
    {
        Interlocked.Increment(ref _editorSessionToken);
        Interlocked.Increment(ref _reverseLookupGeneration);
        _reverseLookupCts?.Cancel();
        ReverseLookupPreview = string.Empty;
        IsEditorOpen = false;
    }

    private void OpenAddDialog()
    {
        Interlocked.Increment(ref _editorSessionToken);
        Interlocked.Increment(ref _reverseLookupGeneration);
        _reverseLookupCts?.Cancel();
        EditorTitle = "新增自定义词条";
        EditorEntryId = string.Empty;
        EditorText = string.Empty;
        EditorCodes.Clear();
        NewCodeScheme = InputSchemes.Wubi86;
        NewCodeText = string.Empty;
        ReverseLookupPreview = string.Empty;
        IsEditorOpen = true;
    }

    private void OpenEditDialog()
    {
        if (SelectedEntry == null) return;
        Interlocked.Increment(ref _editorSessionToken);
        Interlocked.Increment(ref _reverseLookupGeneration);
        _reverseLookupCts?.Cancel();
        EditorTitle = "编辑自定义词条";
        EditorEntryId = SelectedEntry.Id;
        EditorText = SelectedEntry.Text;
        EditorCodes.Clear();
        foreach (var c in SelectedEntry.Codes)
        {
            EditorCodes.Add(new EntryCodeItem { Scheme = c.Scheme, Code = c.Code });
        }
        NewCodeScheme = InputSchemes.Wubi86;
        NewCodeText = string.Empty;
        ReverseLookupPreview = string.Empty;
        IsEditorOpen = true;
    }

    private void AddCodeToDraft()
    {
        string c = NewCodeText.Trim();
        if (string.IsNullOrEmpty(c)) return;

        if (EditorCodes.Any(item => item.Scheme == NewCodeScheme && item.Code.Equals(c, StringComparison.OrdinalIgnoreCase)))
        {
            _banner.ShowWarning("该方案编码已存在");
            return;
        }

        EditorCodes.Add(new EntryCodeItem { Scheme = NewCodeScheme, Code = c });
        NewCodeText = string.Empty;
    }

    private void RemoveCodeFromDraft(object? parameter)
    {
        if (parameter is EntryCodeItem item)
        {
            EditorCodes.Remove(item);
        }
    }

    public async Task CheckReverseLookupAsync()
    {
        // P2-2: Do not Trim()! Preserve raw text with leading/trailing spaces and special characters.
        if (string.IsNullOrWhiteSpace(EditorText))
        {
            ReverseLookupPreview = "请先输入词条正文";
            return;
        }

        string text = EditorText;
        string targetScheme = NewCodeScheme;
        long session = Interlocked.Read(ref _editorSessionToken);
        long generation = Interlocked.Increment(ref _reverseLookupGeneration);

        _reverseLookupCts?.Cancel();
        _reverseLookupCts?.Dispose();
        _reverseLookupCts = new CancellationTokenSource();
        var ct = _reverseLookupCts.Token;

        ReverseLookupPreview = "正在反查...";

        try
        {
            var res = await _client.ReverseLookupAsync(text, targetScheme, null, ct);

            // P2-1: Strictly bind preview response to active editor session, generation, and state snapshot
            if (Interlocked.Read(ref _editorSessionToken) != session ||
                Interlocked.Read(ref _reverseLookupGeneration) != generation ||
                EditorText != text ||
                NewCodeScheme != targetScheme)
            {
                return; // Stale or superseded response cleanly discarded!
            }

            if (res.Found)
            {
                ReverseLookupPreview = $"反查命中: {res.DisplayCode} ({res.ModeDescription})";
            }
            else
            {
                ReverseLookupPreview = "反查无结果 (查不到不猜造)";
            }
        }
        catch (OperationCanceledException)
        {
            // Discard cancelled request without error banner
        }
        catch (Exception ex)
        {
            if (Interlocked.Read(ref _editorSessionToken) == session &&
                Interlocked.Read(ref _reverseLookupGeneration) == generation &&
                EditorText == text &&
                NewCodeScheme == targetScheme)
            {
                ReverseLookupPreview = $"反查异常: {ex.Message}";
            }
        }
    }

    public async Task SaveEditorAsync()
    {
        if (string.IsNullOrWhiteSpace(EditorText))
        {
            _banner.ShowWarning("词条正文不能为空");
            return;
        }
        string text = EditorText;
        string pendingCode = NewCodeText.Trim();
        string pendingScheme = NewCodeScheme;

        var codesToSave = new List<EntryCodeItem>(EditorCodes);
        bool hasNewPendingCode = false;
        if (!string.IsNullOrEmpty(pendingCode))
        {
            bool alreadyExists = codesToSave.Any(item =>
                item.Scheme == pendingScheme &&
                item.Code.Equals(pendingCode, StringComparison.OrdinalIgnoreCase));
            if (!alreadyExists)
            {
                codesToSave.Add(new EntryCodeItem { Scheme = pendingScheme, Code = pendingCode });
                hasNewPendingCode = true;
            }
        }

        if (codesToSave.Count == 0)
        {
            _banner.ShowWarning("必须至少指定一个编码");
            return;
        }

        long session = Interlocked.Read(ref _editorSessionToken);
        IsLoading = true;
        try
        {
            if (string.IsNullOrEmpty(EditorEntryId))
            {
                await _client.AddEntryAsync(text, codesToSave);
                _banner.ShowSuccess($"成功添加词条: {text}");
            }
            else
            {
                await _client.UpdateEntryAsync(EditorEntryId, text, codesToSave);
                _banner.ShowSuccess($"成功更新词条: {text}");
            }

            if (hasNewPendingCode &&
                !EditorCodes.Any(item => item.Scheme == pendingScheme && item.Code.Equals(pendingCode, StringComparison.OrdinalIgnoreCase)))
            {
                EditorCodes.Add(new EntryCodeItem { Scheme = pendingScheme, Code = pendingCode });
            }
            if (!string.IsNullOrEmpty(pendingCode))
            {
                NewCodeText = string.Empty;
            }

            if (Interlocked.Read(ref _editorSessionToken) == session)
            {
                IsEditorOpen = false;
            }
            await LoadEntriesAsync();
        }
        catch (AdminUnconfirmedException ex)
        {
            // Preserves draft in editor, prompts "结果尚未确认", no auto-retry!
            _banner.ShowWarning(ex.Message);
        }
        catch (TimeoutException)
        {
            // Preserves draft in editor, prompts "结果尚未确认", no auto-retry!
            _banner.ShowWarning("请求已发出但连接中断/超时，结果尚未确认");
        }
        catch (AdminException ex) when (ex.IsBusy)
        {
            _banner.ShowError("服务正忙（存在处理中的提交），请稍后重试");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"保存失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }

    public async Task DeleteEntryAsync()
    {
        if (SelectedEntry == null) return;
        string id = SelectedEntry.Id;
        string text = SelectedEntry.Text;

        IsLoading = true;
        try
        {
            await _client.DeleteEntryAsync(id);
            _banner.ShowSuccess($"已删除词条: {text}");
            await LoadEntriesAsync();
        }
        catch (TimeoutException)
        {
            _banner.ShowWarning("删除超时，结果尚未确认");
        }
        catch (AdminException ex) when (ex.IsBusy)
        {
            _banner.ShowError("条目有在途提交，暂无法删除");
        }
        catch (Exception ex)
        {
            _banner.ShowError($"删除失败: {ex.Message}");
        }
        finally
        {
            IsLoading = false;
        }
    }
}

