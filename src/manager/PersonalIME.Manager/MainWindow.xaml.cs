using System;
using System.Windows;

namespace PersonalIME.Manager;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);
        if (DataContext is IDisposable disposable)
        {
            disposable.Dispose();
        }
    }
}
