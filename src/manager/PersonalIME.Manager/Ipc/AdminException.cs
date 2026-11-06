namespace PersonalIME.Manager.Ipc;

public class AdminException : Exception
{
    public AdminErrorCode ErrorCode { get; }

    public AdminException(AdminErrorCode errorCode, string message)
        : base(message)
    {
        ErrorCode = errorCode;
    }

    public bool IsConflict => ErrorCode == AdminErrorCode.RevisionConflict;
    public bool IsBusy => ErrorCode == AdminErrorCode.Busy;
    public bool IsNotFound => ErrorCode == AdminErrorCode.EntryNotFound;
    public bool IsValidationFailed => ErrorCode == AdminErrorCode.ValidationFailed;
}

public class AdminUnconfirmedException : Exception
{
    public AdminUnconfirmedException(string message, Exception? innerException = null)
        : base(message, innerException)
    {
    }
}


