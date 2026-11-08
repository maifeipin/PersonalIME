using System;
using maifeipin.ChineseCalendar.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Models;

public sealed class CalendarDayItem : ViewModelBase
{
    private bool _isSelected;
    private bool _isToday;

    public int DayNumber { get; set; }
    public string DayString => DayNumber > 0 ? DayNumber.ToString() : string.Empty;
    public string LunarText { get; set; } = string.Empty;
    public bool IsCurrentMonth { get; set; }
    
    public bool IsToday
    {
        get => _isToday;
        set => SetProperty(ref _isToday, value);
    }

    public bool IsSelected
    {
        get => _isSelected;
        set => SetProperty(ref _isSelected, value);
    }

    public bool IsWeekend { get; set; }
    public bool IsFestivalOrTerm { get; set; }
    public DateTime FullDate { get; set; }
    public LunarDate? LunarInfo { get; set; }

    public override string ToString() => $"{DayString} ({LunarText})";
}

