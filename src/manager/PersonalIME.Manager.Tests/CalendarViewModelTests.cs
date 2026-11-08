using System;
using System.Linq;
using Microsoft.VisualStudio.TestTools.UnitTesting;
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
}
