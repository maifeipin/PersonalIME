using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.Models;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class RealWpfUiOperationTests
{
    private static string FindRepoRoot()
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            if (File.Exists(Path.Combine(dir, "CMakeLists.txt")) && Directory.Exists(Path.Combine(dir, "src", "ime_service")))
            {
                return dir;
            }
            dir = Path.GetDirectoryName(dir);
        }
        return @"C:\Projects\AITools\PersonalIME";
    }

    private static void DoEvents()
    {
        var frame = new DispatcherFrame();
        Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.Background, new DispatcherOperationCallback(f =>
        {
            ((DispatcherFrame)f).Continue = false;
            return null;
        }), frame);
        Dispatcher.PushFrame(frame);
    }

    private static void AwaitWithPump(Task task)
    {
        while (!task.IsCompleted)
        {
            DoEvents();
            Thread.Sleep(5);
        }
        task.GetAwaiter().GetResult();
    }

    private static T AwaitWithPump<T>(Task<T> task)
    {
        while (!task.IsCompleted)
        {
            DoEvents();
            Thread.Sleep(5);
        }
        return task.GetAwaiter().GetResult();
    }

    private static void RenderWindowToPng(Window window, string filePath)
    {
        window.Measure(new Size(950, 700));
        window.Arrange(new Rect(0, 0, 950, 700));
        window.UpdateLayout();
        DoEvents();

        int width = (int)window.ActualWidth;
        int height = (int)window.ActualHeight;
        if (width <= 0) width = 950;
        if (height <= 0) height = 700;

        var rtb = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        rtb.Render(window);

        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(rtb));

        Directory.CreateDirectory(Path.GetDirectoryName(filePath)!);
        using var stream = File.Create(filePath);
        encoder.Save(stream);
    }

    [TestMethod]
    public void TestRealWpfFourPagesOperationAndEvidenceCapture()
    {
        string repoRoot = FindRepoRoot();
        string serviceExe = Path.Combine(repoRoot, @"build\Release\ime-service.exe");
        if (!File.Exists(serviceExe))
        {
            Assert.Inconclusive("ime-service.exe not found at " + serviceExe);
            return;
        }

        string testPipe = $"PersonalIME_WPF_UI_{Environment.ProcessId}_{Guid.NewGuid():N}";
        string testDb = Path.Combine(repoRoot, $"out/wpf_ui_test_{Environment.ProcessId}_{Guid.NewGuid():N}.db");
        string dictDir = Path.Combine(repoRoot, @"data\dictionaries");
        Directory.CreateDirectory(Path.Combine(repoRoot, "out"));
        if (File.Exists(testDb)) File.Delete(testDb);

        var psi = new ProcessStartInfo
        {
            FileName = serviceExe,
            Arguments = $"--db \"{testDb}\" --admin-pipe \"{testPipe}\" --dict \"{dictDir}\"",
            WorkingDirectory = repoRoot,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        var serviceProcess = Process.Start(psi);
        Assert.IsNotNull(serviceProcess, "Failed to start isolated ime-service process");

        Exception? staException = null;
        string evidenceDir = Path.Combine(repoRoot, "out", "ui-evidence");
        Directory.CreateDirectory(evidenceDir);

        var thread = new Thread(() =>
        {
            try
            {
                var app = Application.Current ?? new Application();
                SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext(Dispatcher.CurrentDispatcher));
                if (!app.Resources.Contains("BooleanToVisibilityConverter"))
                {
                    app.Resources.Add("BooleanToVisibilityConverter", new System.Windows.Controls.BooleanToVisibilityConverter());
                }
                var client = new AdminIpcClient(testPipe);
                var vm = new MainViewModel(client);
                var window = new MainWindow
                {
                    DataContext = vm,
                    Width = 950,
                    Height = 700
                };
                window.Show();

                // Initialize connection
                AwaitWithPump(vm.InitializeAsync());
                Assert.IsTrue(vm.IsConnected, "MainViewModel must successfully connect to live service");

                // -------------------------------------------------------------
                // Page 1: WordbookView (词库)
                // -------------------------------------------------------------
                vm.SelectedTabIndex = 0;
                DoEvents();

                // 1. Add Entry with leading/trailing whitespaces and newlines
                vm.Wordbook.OpenAddDialogCommand.Execute(null);
                Assert.IsTrue(vm.Wordbook.IsEditorOpen);
                string textWithEdges = "  [真实WPF操作测试词条]\r\n";
                vm.Wordbook.EditorText = textWithEdges;
                vm.Wordbook.EditorCodes.Clear();
                vm.Wordbook.EditorCodes.Add(new EntryCodeItem
                {
                    Scheme = InputSchemes.Custom,
                    Code = ";wpf"
                });
                AwaitWithPump(vm.Wordbook.SaveEditorAsync());
                Assert.IsFalse(vm.Wordbook.IsEditorOpen, $"Editor should be closed, Banner: {vm.Banner.Message}");

                // Check entries list
                var found = vm.Wordbook.Entries.FirstOrDefault(e => e.SummaryText.Contains("[真实WPF操作测试词条]"));
                Assert.IsNotNull(found, $"Entry not found in Wordbook. Entries count: {vm.Wordbook.Entries.Count}, TotalCount: {vm.Wordbook.TotalCount}, Banner: {vm.Banner.Message}");
                Assert.AreEqual(textWithEdges, found.Text);

                RenderWindowToPng(window, Path.Combine(evidenceDir, "01_wordbook_tab.png"));

                // -------------------------------------------------------------
                // Page 2: SortingView (排序)
                // -------------------------------------------------------------
                vm.SelectedTabIndex = 1;
                DoEvents();

                // Query scope wubi86 'gh'
                vm.Sorting.SelectedScheme = InputSchemes.Wubi86;
                vm.Sorting.QueryCode = "gh";
                AwaitWithPump(vm.Sorting.LoadPinnedAsync());

                // Candidate picker search & add builtin candidate
                vm.Sorting.CandidateSearchText = string.Empty;
                AwaitWithPump(vm.Sorting.SearchCandidatesAsync());
                var candToPin = vm.Sorting.AvailableCandidates.FirstOrDefault(c => c.Id.StartsWith("b_wb_gh_"));
                Assert.IsNotNull(candToPin, "Must find candidate for wubi86 gh");
                vm.Sorting.SelectedCandidate = candToPin;
                vm.Sorting.AddCandidateToPinned();

                // Save sort order
                AwaitWithPump(vm.Sorting.SaveSortOrderAsync());
                Assert.IsTrue(vm.Sorting.CurrentRevision >= 1);
                Assert.AreEqual(1, vm.Sorting.PinnedEntries.Count);
                Assert.AreEqual("下", vm.Sorting.PinnedEntries[0].Text);

                // Edit query code without reload: verify scope dirty warning
                vm.Sorting.QueryCode = "different_code";
                DoEvents();
                Assert.IsTrue(vm.Sorting.IsQueryCodeDirty);
                StringAssert.Contains(vm.Sorting.RevisionDisplay, "(未重新加载)");
                StringAssert.Contains(vm.Sorting.ScopeDirtyWarning, "gh");

                RenderWindowToPng(window, Path.Combine(evidenceDir, "02_sorting_dirty_warning.png"));

                // Revert back and save
                vm.Sorting.QueryCode = "gh";
                DoEvents();
                Assert.IsFalse(vm.Sorting.IsQueryCodeDirty);
                RenderWindowToPng(window, Path.Combine(evidenceDir, "03_sorting_saved.png"));

                // -------------------------------------------------------------
                // Page 3: SettingsView (设置)
                // -------------------------------------------------------------
                vm.SelectedTabIndex = 2;
                DoEvents();
                AwaitWithPump(vm.Settings.LoadPolicyAsync());
                Assert.IsTrue(vm.Settings.IsLoaded);

                // Adjust policy settings via VM
                vm.Settings.CandidatePageSize = 8;
                vm.Settings.MixedInputEnabled = true;
                vm.Settings.InitialChinese = true;
                vm.Settings.InitialFullPunct = true;

                // Move input method priority (move pinyin up)
                var pinyinItem = vm.Settings.MethodPriorities.FirstOrDefault(m => m.Scheme == InputSchemes.Pinyin);
                if (pinyinItem != null)
                {
                    vm.Settings.SelectedMethodPriority = pinyinItem;
                    vm.Settings.MoveMethodUpCommand.Execute(null);
                }

                AwaitWithPump(vm.Settings.SavePolicyAsync());
                Assert.IsTrue(vm.Settings.CurrentRevision >= 1);
                RenderWindowToPng(window, Path.Combine(evidenceDir, "04_settings_tab.png"));

                // -------------------------------------------------------------
                // Page 4: MaintenanceView (维护)
                // -------------------------------------------------------------
                vm.SelectedTabIndex = 3;
                DoEvents();
                AwaitWithPump(vm.Maintenance.RefreshStatusAsync());
                Assert.IsFalse(string.IsNullOrEmpty(vm.Maintenance.Status.ServiceVersion));
                Assert.IsFalse(string.IsNullOrEmpty(vm.Maintenance.Status.DatabasePath));

                // Export backup to file
                string exportPath = Path.Combine(repoRoot, $"out/wpf_ui_export_{Environment.ProcessId}.json");
                AwaitWithPump(client.ExportDataAsync(exportPath));
                Assert.IsTrue(File.Exists(exportPath));
                File.Delete(exportPath);

                RenderWindowToPng(window, Path.Combine(evidenceDir, "05_maintenance_tab.png"));

                // -------------------------------------------------------------
                // Window Closing & In-flight Cancellation Verification
                // -------------------------------------------------------------
                window.Close();
                Assert.IsTrue(vm.IsDisposed, "Closing window must trigger DataContext disposal and cancel CTS");
            }
            catch (Exception ex)
            {
                staException = ex;
            }
        });

        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();

        // Cleanup service process
        if (serviceProcess != null && !serviceProcess.HasExited)
        {
            serviceProcess.Kill();
            serviceProcess.WaitForExit(3000);
            serviceProcess.Dispose();
        }

        try
        {
            if (File.Exists(testDb)) File.Delete(testDb);
        }
        catch { }

        if (staException != null)
        {
            Assert.Fail($"WPF UI operation failed on STA thread: {staException}");
        }

        // Verify that evidence PNG files were created
        Assert.IsTrue(File.Exists(Path.Combine(evidenceDir, "01_wordbook_tab.png")), "01_wordbook_tab.png must be created");
        Assert.IsTrue(File.Exists(Path.Combine(evidenceDir, "02_sorting_dirty_warning.png")), "02_sorting_dirty_warning.png must be created");
        Assert.IsTrue(File.Exists(Path.Combine(evidenceDir, "03_sorting_saved.png")), "03_sorting_saved.png must be created");
        Assert.IsTrue(File.Exists(Path.Combine(evidenceDir, "04_settings_tab.png")), "04_settings_tab.png must be created");
        Assert.IsTrue(File.Exists(Path.Combine(evidenceDir, "05_maintenance_tab.png")), "05_maintenance_tab.png must be created");
    }
}
