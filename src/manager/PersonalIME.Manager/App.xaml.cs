using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager;

public partial class App : Application
{
    private static Mutex? _singleInstanceMutex;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll", EntryPoint = "FindWindowW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindWindowW(string? lpClassName, string lpWindowName);

    private const int SW_RESTORE = 9;

    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        bool isCalendarMode = false;
        string pipeName = AdminIpcClient.DefaultPipeName;
        for (int i = 0; i < e.Args.Length; i++)
        {
            if (e.Args[i] == "--calendar" || (e.Args[i] == "--tool" && i + 1 < e.Args.Length && e.Args[i + 1].Equals("calendar", StringComparison.OrdinalIgnoreCase)))
            {
                isCalendarMode = true;
            }
            if (e.Args[i] == "--pipe" && i + 1 < e.Args.Length)
            {
                pipeName = e.Args[i + 1];
            }
        }

        string mutexName = isCalendarMode
            ? @"Local\PersonalIME_Calendar_SingleInstance_Mutex"
            : @"Local\PersonalIME_Manager_SingleInstance_Mutex";

        _singleInstanceMutex = new Mutex(true, mutexName, out bool createdNew);
        if (!createdNew)
        {
            // Another instance is already running: bring its window forward and terminate
            IntPtr existingWnd = IntPtr.Zero;
            if (isCalendarMode)
            {
                existingWnd = FindWindowW(null, "中华万年历");
            }
            else
            {
                try
                {
                    var currentProc = System.Diagnostics.Process.GetCurrentProcess();
                    var existingProc = System.Diagnostics.Process.GetProcessesByName(currentProc.ProcessName)
                                              .FirstOrDefault(p => p.Id != currentProc.Id && p.MainWindowHandle != IntPtr.Zero);
                    if (existingProc != null)
                    {
                        existingWnd = existingProc.MainWindowHandle;
                    }
                }
                catch
                {
                    // Process enumeration fallback
                }

                if (existingWnd == IntPtr.Zero)
                {
                    existingWnd = FindWindowW(null, "设置");
                }
                if (existingWnd == IntPtr.Zero)
                {
                    existingWnd = FindWindowW(null, "人人 设置");
                }
                if (existingWnd == IntPtr.Zero)
                {
                    existingWnd = FindWindowW(null, "PersonalIME 管理器");
                }
            }

            if (existingWnd != IntPtr.Zero)
            {
                ShowWindowAsync(existingWnd, SW_RESTORE);
                SetForegroundWindow(existingWnd);
            }
            Shutdown();
            return;
        }

        if (isCalendarMode)
        {
            var calWin = new Views.CalendarWindow();
            calWin.Show();
            return;
        }

        var client = new AdminIpcClient(pipeName);
        var mainVm = new MainViewModel(client);
        var win = new MainWindow
        {
            DataContext = mainVm
        };
        win.Show();

        await mainVm.InitializeAsync();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _singleInstanceMutex?.ReleaseMutex();
        _singleInstanceMutex?.Dispose();
        _singleInstanceMutex = null;
        base.OnExit(e);
    }
}
