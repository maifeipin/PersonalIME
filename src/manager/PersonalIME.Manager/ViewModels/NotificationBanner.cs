namespace PersonalIME.Manager.ViewModels;

public enum BannerType
{
    Info,
    Success,
    Warning,
    Error
}

public class NotificationBanner : ViewModelBase
{
    private string _message = string.Empty;
    private bool _isVisible;
    private BannerType _type = BannerType.Info;

    public string Message
    {
        get => _message;
        set => SetProperty(ref _message, value);
    }

    public bool IsVisible
    {
        get => _isVisible;
        set => SetProperty(ref _isVisible, value);
    }

    public BannerType Type
    {
        get => _type;
        set => SetProperty(ref _type, value);
    }

    public RelayCommand DismissCommand { get; }

    public NotificationBanner()
    {
        DismissCommand = new RelayCommand(Dismiss);
    }

    public void Show(string message, BannerType type = BannerType.Info)
    {
        Message = message;
        Type = type;
        IsVisible = true;
    }

    public void ShowSuccess(string message) => Show(message, BannerType.Success);
    public void ShowError(string message) => Show(message, BannerType.Error);
    public void ShowWarning(string message) => Show(message, BannerType.Warning);
    public void ShowInfo(string message) => Show(message, BannerType.Info);

    public void Dismiss()
    {
        IsVisible = false;
        Message = string.Empty;
    }
}

