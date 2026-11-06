using System.Windows;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager;

public partial class App : Application
{
    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        string pipeName = AdminIpcClient.DefaultPipeName;
        for (int i = 0; i < e.Args.Length; i++)
        {
            if (e.Args[i] == "--pipe" && i + 1 < e.Args.Length)
            {
                pipeName = e.Args[i + 1];
            }
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
}
