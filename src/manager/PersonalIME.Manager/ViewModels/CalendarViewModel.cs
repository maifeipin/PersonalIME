using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Windows.Input;
using maifeipin.ChineseCalendar;
using maifeipin.ChineseCalendar.Models;
using PersonalIME.Manager.Models;

namespace PersonalIME.Manager.ViewModels;

public sealed class CalendarViewModel : ViewModelBase, IDisposable
{
    private readonly System.Windows.Threading.DispatcherTimer? _timer;
    private DateTime _lastCheckedDate = DateTime.Today;
    private int _lastCheckedHour = DateTime.Now.Hour;

    private int _currentYear;
    private int _currentMonth;
    private CalendarDayItem? _selectedDay;
    private GeoLocation _selectedCity;

    public ObservableCollection<CalendarDayItem> Days { get; } = [];
    public ObservableCollection<HourGanZhiItem> HourItems { get; } = [];
    public IReadOnlyList<GeoLocation> Cities { get; }

    public int CurrentYear
    {
        get => _currentYear;
        private set => SetProperty(ref _currentYear, value);
    }

    public int CurrentMonth
    {
        get => _currentMonth;
        private set => SetProperty(ref _currentMonth, value);
    }

    public string YearMonthDisplay => $"{CurrentYear}年 {CurrentMonth:D2}月";
    public string YearGanZhiDisplay => $"{CurrentYear} {ChineseCalendarEngine.GetYearGanZhi(CurrentYear)}年【{ChineseCalendarEngine.GetYearGanZhi(CurrentYear).ZodiacName}】";

    public CalendarDayItem? SelectedDay
    {
        get => _selectedDay;
        set
        {
            if (_selectedDay != null) _selectedDay.IsSelected = false;
            if (SetProperty(ref _selectedDay, value))
            {
                if (_selectedDay != null)
                {
                    _selectedDay.IsSelected = true;
                    UpdateSelectionDetails(_selectedDay);
                }
            }
        }
    }

    public GeoLocation SelectedCity
    {
        get => _selectedCity;
        set
        {
            if (SetProperty(ref _selectedCity, value) && _selectedDay != null)
            {
                UpdateSunTimes(_selectedDay.FullDate);
            }
        }
    }

    // Detail Panel properties
    private string _selectedDateDisplay = string.Empty;
    public string SelectedDateDisplay
    {
        get => _selectedDateDisplay;
        private set => SetProperty(ref _selectedDateDisplay, value);
    }

    private string _selectedLunarDisplay = string.Empty;
    public string SelectedLunarDisplay
    {
        get => _selectedLunarDisplay;
        private set => SetProperty(ref _selectedLunarDisplay, value);
    }

    private string _selectedGanZhiDisplay = string.Empty;
    public string SelectedGanZhiDisplay
    {
        get => _selectedGanZhiDisplay;
        private set => SetProperty(ref _selectedGanZhiDisplay, value);
    }

    private string _selectedZodiacDisplay = string.Empty;
    public string SelectedZodiacDisplay
    {
        get => _selectedZodiacDisplay;
        private set => SetProperty(ref _selectedZodiacDisplay, value);
    }

    private string _selectedTermDisplay = string.Empty;
    public string SelectedTermDisplay
    {
        get => _selectedTermDisplay;
        private set => SetProperty(ref _selectedTermDisplay, value);
    }

    private string _selectedFestDisplay = string.Empty;
    public string SelectedFestDisplay
    {
        get => _selectedFestDisplay;
        private set => SetProperty(ref _selectedFestDisplay, value);
    }

    private SunTimeInfo _sunTime = new();
    public SunTimeInfo SunTime
    {
        get => _sunTime;
        private set => SetProperty(ref _sunTime, value);
    }

    // Commands
    public ICommand PrevMonthCommand { get; }
    public ICommand NextMonthCommand { get; }
    public ICommand PrevYearCommand { get; }
    public ICommand NextYearCommand { get; }
    public ICommand GoTodayCommand { get; }
    public ICommand SelectDayCommand { get; }

    public CalendarViewModel()
    {
        Cities = ChineseCalendarConstants.ChineseCities;
        _selectedCity = Cities.FirstOrDefault(c => c.City == "北京") ?? Cities[0];

        var today = DateTime.Today;
        _currentYear = today.Year;
        _currentMonth = today.Month;

        PrevMonthCommand = new RelayCommand(() => ChangeMonth(-1));
        NextMonthCommand = new RelayCommand(() => ChangeMonth(1));
        PrevYearCommand = new RelayCommand(() => ChangeYear(-1));
        NextYearCommand = new RelayCommand(() => ChangeYear(1));
        GoTodayCommand = new RelayCommand(GoToday);
        SelectDayCommand = new RelayCommand(param =>
        {
            if (param is CalendarDayItem item && item.IsCurrentMonth)
            {
                SelectedDay = item;
            }
        });

        if (System.Windows.Application.Current != null)
        {
            _timer = new System.Windows.Threading.DispatcherTimer
            {
                Interval = TimeSpan.FromSeconds(10)
            };
            _timer.Tick += OnTimerTick;
            _timer.Start();
        }

        LoadMonthGrid(_currentYear, _currentMonth, today.Day);
    }

    public void ChangeMonth(int delta)
    {
        int y = _currentYear;
        int m = _currentMonth + delta;
        while (m > 12) { m -= 12; y++; }
        while (m < 1) { m += 12; y--; }

        if (y < 1900) { y = 1900; m = 1; }
        else if (y > 2100) { y = 2100; m = 12; }

        if (y == _currentYear && m == _currentMonth)
        {
            return;
        }

        _currentYear = y;
        _currentMonth = m;
        OnPropertyChanged(nameof(CurrentYear));
        OnPropertyChanged(nameof(CurrentMonth));
        OnPropertyChanged(nameof(YearMonthDisplay));
        OnPropertyChanged(nameof(YearGanZhiDisplay));

        int defaultDay = (y == 1900 && m == 1) ? 31 : 1;
        LoadMonthGrid(y, m, defaultDay);
    }

    public void ChangeYear(int delta)
    {
        int y = _currentYear + delta;
        if (y < 1900) y = 1900;
        if (y > 2100) y = 2100;

        if (y == _currentYear) return;

        _currentYear = y;
        OnPropertyChanged(nameof(CurrentYear));
        OnPropertyChanged(nameof(YearMonthDisplay));
        OnPropertyChanged(nameof(YearGanZhiDisplay));

        int defaultDay = (y == 1900 && _currentMonth == 1) ? 31 : 1;
        LoadMonthGrid(y, _currentMonth, defaultDay);
    }

    public void GoToday()
    {
        var today = DateTime.Today;
        _currentYear = today.Year;
        _currentMonth = today.Month;
        OnPropertyChanged(nameof(CurrentYear));
        OnPropertyChanged(nameof(CurrentMonth));
        OnPropertyChanged(nameof(YearMonthDisplay));
        OnPropertyChanged(nameof(YearGanZhiDisplay));

        LoadMonthGrid(_currentYear, _currentMonth, today.Day);
    }

    private void LoadMonthGrid(int year, int month, int selectDay)
    {
        Days.Clear();
        var firstDay = new DateTime(year, month, 1);
        int startDow = (int)firstDay.DayOfWeek; // 0=Sunday
        int daysInMonth = DateTime.DaysInMonth(year, month);
        var today = DateTime.Today;

        if (year == 1900 && month == 1 && selectDay < 31)
        {
            selectDay = 31;
        }

        CalendarDayItem? targetSelection = null;

        for (int i = 0; i < 42; i++)
        {
            int cellDate = i - startDow + 1;
            var item = new CalendarDayItem();

            if (cellDate > 0 && cellDate <= daysInMonth)
            {
                var dt = new DateTime(year, month, cellDate);
                LunarDate? lunar = null;
                if (dt >= ChineseCalendarEngine.MinSupportedDate && dt <= ChineseCalendarEngine.MaxSupportedDate)
                {
                    lunar = ChineseCalendarEngine.GetLunarDate(dt);
                }

                item.DayNumber = cellDate;
                item.IsCurrentMonth = true;
                item.FullDate = dt;
                item.LunarInfo = lunar;
                item.IsToday = (dt == today);
                item.IsWeekend = (i % 7 == 0 || i % 7 == 6);

                if (lunar != null)
                {
                    if (lunar.SolarTerm != null)
                    {
                        item.LunarText = lunar.SolarTerm.Name;
                        item.IsFestivalOrTerm = true;
                    }
                    else if (!string.IsNullOrEmpty(lunar.Festivals))
                    {
                        item.LunarText = lunar.Festivals.Split(' ')[0];
                        item.IsFestivalOrTerm = true;
                    }
                    else if (lunar.Day == 1)
                    {
                        item.LunarText = lunar.MonthName;
                    }
                    else
                    {
                        item.LunarText = lunar.DayName;
                    }
                }
                else
                {
                    item.LunarText = string.Empty;
                }

                if (cellDate == selectDay)
                {
                    targetSelection = item;
                }
            }
            else
            {
                item.IsCurrentMonth = false;
            }

            Days.Add(item);
        }

        if (targetSelection == null && Days.Any(d => d.IsCurrentMonth && d.LunarInfo != null))
        {
            targetSelection = Days.First(d => d.IsCurrentMonth && d.LunarInfo != null);
        }
        else if (targetSelection == null && Days.Any(d => d.IsCurrentMonth))
        {
            targetSelection = Days.First(d => d.IsCurrentMonth);
        }

        SelectedDay = targetSelection;
    }

    private void OnTimerTick(object? sender, EventArgs e)
    {
        var now = DateTime.Now;
        if (now.Date != _lastCheckedDate)
        {
            _lastCheckedDate = now.Date;
            foreach (var d in Days)
            {
                d.IsToday = (d.FullDate.Date == now.Date);
            }
            if (SelectedDay != null)
            {
                UpdateSelectionDetails(SelectedDay);
            }
        }
        else if (now.Hour != _lastCheckedHour)
        {
            _lastCheckedHour = now.Hour;
            if (SelectedDay != null && SelectedDay.FullDate.Date == now.Date)
            {
                UpdateSelectionDetails(SelectedDay);
            }
        }
    }

    public void Dispose()
    {
        if (_timer != null)
        {
            _timer.Stop();
            _timer.Tick -= OnTimerTick;
        }
    }

    private void RefreshGridSelection()
    {
        OnPropertyChanged(nameof(Days));
    }

    private static readonly string[] DayOfWeekNames = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };

    private void UpdateSelectionDetails(CalendarDayItem item)
    {
        if (item.LunarInfo == null) return;
        var lunar = item.LunarInfo;
        var dt = item.FullDate;

        SelectedDateDisplay = $"{dt:yyyy年MM月dd日} {DayOfWeekNames[(int)dt.DayOfWeek]}";

        string moonSuffix = !string.IsNullOrEmpty(lunar.MoonPhase) ? $" ({lunar.MoonPhase})" : "";
        SelectedLunarDisplay = $"{lunar.FullChineseDate}{moonSuffix}";

        // 四柱八字统一基于选定日正午 12:00（消除立春换年及各节交节临界点的时辰不确定性）
        var noonPillars = ChineseCalendarEngine.GetFourPillars(dt.Date.AddHours(12));
        SelectedGanZhiDisplay = $"{noonPillars.YearPillar.Name}年 {noonPillars.MonthPillar.Name}月 {noonPillars.DayPillar.Name}日";
        SelectedZodiacDisplay = $"【生肖: {lunar.ZodiacName}】 {lunar.ConstellationName}";

        if (lunar.SolarTerm != null)
        {
            SelectedTermDisplay = $"节气: {lunar.SolarTerm.Name} (约 {lunar.SolarTerm.ExactTime:yyyy-MM-dd HH:mm})";
        }
        else
        {
            // Show upcoming/neighboring terms in current month
            var (t1, t2) = ChineseCalendarEngine.GetMonthSolarTerms(dt.Year, dt.Month);
            SelectedTermDisplay = $"本月节气: {t1.Day}日 {t1.Name} · {t2.Day}日 {t2.Name} (估算)";
        }

        SelectedFestDisplay = !string.IsNullOrEmpty(lunar.Festivals) ? $"节日: {lunar.Festivals}" : string.Empty;

        // Update 12 Hours Table
        HourItems.Clear();
        var hours = ChineseCalendarEngine.GetDayHourTable(dt);
        foreach (var h in hours)
        {
            HourItems.Add(h);
        }

        UpdateSunTimes(dt);
    }

    private void UpdateSunTimes(DateTime dt)
    {
        SunTime = ChineseCalendarEngine.GetSunTimes(dt, SelectedCity.Latitude, SelectedCity.Longitude);
    }
}
