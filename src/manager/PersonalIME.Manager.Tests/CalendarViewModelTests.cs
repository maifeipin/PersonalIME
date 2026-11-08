using System;
using System.Linq;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class CalendarViewModelTests
{
    [TestMethod]
    public void TestCalendarViewModelInitialization()
    {
        var vm = new CalendarViewModel();

        Assert.AreEqual(42, vm.Days.Count);
        Assert.IsNotNull(vm.SelectedDay);
        Assert.IsTrue(vm.SelectedDay.IsCurrentMonth);
        Assert.AreEqual("北京", vm.SelectedCity.City);
        Assert.IsFalse(string.IsNullOrEmpty(vm.YearMonthDisplay));
        Assert.IsFalse(string.IsNullOrEmpty(vm.YearGanZhiDisplay));
        Assert.IsFalse(string.IsNullOrEmpty(vm.SelectedLunarDisplay));
        Assert.AreEqual(12, vm.HourItems.Count);
    }

    [TestMethod]
    public void TestMonthNavigation()
    {
        var vm = new CalendarViewModel();
        int origMonth = vm.CurrentMonth;
        int origYear = vm.CurrentYear;

        vm.NextMonthCommand.Execute(null);

        if (origMonth == 12)
        {
            Assert.AreEqual(1, vm.CurrentMonth);
            Assert.AreEqual(origYear + 1, vm.CurrentYear);
        }
        else
        {
            Assert.AreEqual(origMonth + 1, vm.CurrentMonth);
            Assert.AreEqual(origYear, vm.CurrentYear);
        }

        Assert.AreEqual(42, vm.Days.Count);
        Assert.IsNotNull(vm.SelectedDay);
    }

    [TestMethod]
    public void TestSelectDayUpdatesDetails()
    {
        var vm = new CalendarViewModel();
        var targetDay = vm.Days.First(d => d.IsCurrentMonth && d.DayNumber == 15);

        vm.SelectDayCommand.Execute(targetDay);

        Assert.AreEqual(targetDay, vm.SelectedDay);
        Assert.IsTrue(targetDay.IsSelected);
        Assert.IsTrue(vm.SelectedDateDisplay.Contains("15日"));
        Assert.IsFalse(string.IsNullOrEmpty(vm.SelectedGanZhiDisplay));
        Assert.AreEqual(12, vm.HourItems.Count);
    }

    [TestMethod]
    public void TestCityChangeUpdatesSunTimes()
    {
        var vm = new CalendarViewModel();
        var sh = vm.Cities.FirstOrDefault(c => c.City == "上海");
        Assert.IsNotNull(sh);

        vm.SelectedCity = sh;
        Assert.AreEqual("上海", vm.SelectedCity.City);
        Assert.AreNotEqual("--:--", vm.SunTime.Sunrise);
        Assert.AreNotEqual("--:--", vm.SunTime.Sunset);
    }

    [TestMethod]
    public void TestDateSelectionNotifiesPropertyChange()
    {
        using var vm = new CalendarViewModel();
        var day1 = vm.Days.First(d => d.IsCurrentMonth && d.DayNumber == 10);
        var day2 = vm.Days.First(d => d.IsCurrentMonth && d.DayNumber == 20);

        bool day1Notified = false;
        bool day2Notified = false;

        day1.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(CalendarDayItem.IsSelected)) day1Notified = true;
        };
        day2.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(CalendarDayItem.IsSelected)) day2Notified = true;
        };

        vm.SelectDayCommand.Execute(day1);
        Assert.IsTrue(day1.IsSelected);
        Assert.IsTrue(day1Notified);

        day1Notified = false;
        vm.SelectDayCommand.Execute(day2);
        Assert.IsFalse(day1.IsSelected);
        Assert.IsTrue(day2.IsSelected);
        Assert.IsTrue(day1Notified);
        Assert.IsTrue(day2Notified);
    }

    [TestMethod]
    public void TestBoundaryNavigationNoWrap()
    {
        using var vm = new CalendarViewModel();
        while (vm.CurrentYear > 1900) vm.PrevYearCommand.Execute(null);
        while (vm.CurrentMonth > 1) vm.PrevMonthCommand.Execute(null);

        Assert.AreEqual(1900, vm.CurrentYear);
        Assert.AreEqual(1, vm.CurrentMonth);
        Assert.IsNotNull(vm.SelectedDay);
        Assert.AreEqual(31, vm.SelectedDay.DayNumber);

        // 向前翻月不能反向跳转到 1900-12
        vm.PrevMonthCommand.Execute(null);
        Assert.AreEqual(1900, vm.CurrentYear);
        Assert.AreEqual(1, vm.CurrentMonth);
    }
}

