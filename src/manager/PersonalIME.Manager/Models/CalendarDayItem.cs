using System;
using maifeipin.ChineseCalendar.Models;

namespace PersonalIME.Manager.Models;

public sealed class CalendarDayItem
{
    public int DayNumber { get; set; }
    public string DayString => DayNumber > 0 ? DayNumber.ToString() : string.Empty;
    public string LunarText { get; set; } = string.Empty;
    public bool IsCurrentMonth { get; set; }
    public bool IsToday { get; set; }
    public bool IsSelected { get; set; }
    public bool IsWeekend { get; set; }
    public bool IsFestivalOrTerm { get; set; }
    public DateTime FullDate { get; set; }
    public LunarDate? LunarInfo { get; set; }

    public override string ToString() => $"{DayString} ({LunarText})";
}
