using RTLzz.Gui.Models;
using RTLzz.Gui.Services;
using System.Drawing;
using System.Windows.Forms;

namespace RTLzz.Gui.Tests;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            RunUnitTests();
            if (args.Length >= 1 && args[0] == "--runner-smoke")
            {
                if (args.Length != 4) throw new InvalidOperationException("Usage: --runner-smoke <predicate-expand.exe> <includeDir> <outputDir>");
                RunRunnerSmokeAsync(args[1], args[2], args[3]).GetAwaiter().GetResult();
            }
            Console.WriteLine("GUI service tests passed.");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("GUI service tests failed: " + ex);
            return 1;
        }
    }

    private static void RunUnitTests()
    {
        Check(OutputFormatInfo.CliName(OutputFormat.Text) == "text", "text cli");
        Check(OutputFormatInfo.CliName(OutputFormat.Json) == "json", "json cli");
        Check(OutputFormatInfo.CliName(OutputFormat.StableJson) == "stable_json", "stable json cli");
        Check(OutputFormatInfo.CliName(OutputFormat.Smt) == "smt", "smt cli");
        Check(OutputFormatInfo.Extension(OutputFormat.StableJson) == ".stable.json", "stable extension");
        using (var form = new MainForm())
        {
            Check(form.WindowState == FormWindowState.Maximized, "default window maximized");
            Check(form.FormBorderStyle == FormBorderStyle.Sizable, "sizable border");
            Check(form.MaximizeBox, "maximize box enabled");
            Check(form.MinimumSize.Width >= 640 && form.MinimumSize.Width <= 1000, "reasonable minimum width");
            Check(form.MinimumSize.Height >= 480 && form.MinimumSize.Height <= 650, "reasonable minimum height");
            var working = Screen.FromControl(form).WorkingArea;
            Check(form.InitialNormalBoundsForTests.Width <= working.Width, "normal width within working area");
            Check(form.InitialNormalBoundsForTests.Height <= working.Height, "normal height within working area");
            Check(form.InitialNormalBoundsForTests.Width >= Math.Min(form.MinimumSize.Width, working.Width), "normal width respects minimum");
            Check(form.InitialNormalBoundsForTests.Height >= Math.Min(form.MinimumSize.Height, working.Height), "normal height respects minimum");

            var split = form.MainSplitForTests;
            Check(split.Orientation == Orientation.Vertical, "vertical split");
            Check(split.Panel1MinSize >= 450, "left min size");
            Check(split.Panel2MinSize >= 400, "right min size");
            Check(split.Panel1.Controls.Contains(form.LeftScrollPanelForTests), "left scroll panel in split");
            Check(form.LeftScrollPanelForTests.Dock == DockStyle.Fill, "left scroll dock fill");
            Check(form.LeftScrollPanelForTests.AutoScroll, "left autoscroll panel");
            Check(form.LeftContentTableForTests.Parent == form.LeftScrollPanelForTests, "left content inside scroll panel");
            Check(form.LeftContentTableForTests.Dock == DockStyle.Top, "left content dock top");
            Check(form.LeftContentTableForTests.Dock != DockStyle.Fill, "left content not dock fill");
            Check(form.LeftContentTableForTests.AutoSize, "left content autosize");
            Check(form.LeftContentTableForTests.AutoSizeMode == AutoSizeMode.GrowAndShrink, "left content grows");
            Check(form.LeftContentTableForTests.ColumnCount == 1, "single left content column");
            Check(form.LeftContentTableForTests.Controls.Contains(form.RunGroupForTests), "run group in left content");
            Check(form.ChooseIncludeDirButtonForTests.MinimumSize.Width >= 80, "long path select button width");
            Check(form.ChooseIncludeDirButtonForTests.Dock == DockStyle.Fill, "select button remains visible");

            var scrollMethod = typeof(MainForm).GetMethod("UpdateLeftScrollLayout", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
            Check(scrollMethod != null, "UpdateLeftScrollLayout exists");
            scrollMethod!.Invoke(form, null);
            Check(form.LeftScrollPanelForTests.AutoScrollMinSize.Height > 0, "scroll min size updated");
            Check(form.ScrollLayoutUpdatedForTests, "manual scroll layout update tracked");

            form.ResetScrollLayoutUpdatedForTests();
            form.Show();
            Application.DoEvents();
            Check(form.ScrollLayoutUpdatedForTests, "shown recalculates scroll range");
            Check(form.LeftScrollPanelForTests.AutoScrollPosition.Y == 0, "startup scroll at top");

            form.ResetScrollLayoutUpdatedForTests();
            form.WindowState = FormWindowState.Normal;
            Application.DoEvents();
            form.ResetScrollLayoutUpdatedForTests();
            form.Size = new Size(form.Width + 1, form.Height + 1);
            Application.DoEvents();
            Check(form.ScrollLayoutUpdatedForTests, "resize updates scroll range");

            form.ResetScrollLayoutUpdatedForTests();
            if (split.Width > split.Panel1MinSize + split.Panel2MinSize + split.SplitterWidth + 10)
            {
                split.SplitterDistance = Math.Min(split.SplitterDistance + 1, split.Width - split.Panel2MinSize - split.SplitterWidth);
                Application.DoEvents();
                Check(form.ScrollLayoutUpdatedForTests, "splitter moved updates scroll range");
            }

            form.Hide();
            Check(ContainsText(form, "使用文件"), "full file radio text");
            Check(ContainsText(form, "使用粘贴代码"), "full pasted radio text");
            Check(ContainsText(form, "Stable JSON"), "stable json text");
            Check(ContainsText(form, "ForceUint"), "force uint text");
            Check(ContainsText(form, "开始生成"), "run button text");
            Check(ContainsText(form, "取消"), "cancel button text");
        }
        SynchronizationContext.SetSynchronizationContext(null);

        var pathService = new OutputPathService();
        string desktopRoot = pathService.DefaultRootDirectory();
        Check(desktopRoot.Contains("RTLzz_Output"), "default output root");
        string baseName = pathService.DefaultBaseNameForFile(@"C:\tmp\中文 路径 (case)\demo.test.cpp");
        Check(baseName == "demo.test", "base name");
        string output = pathService.BuildOutputPath(@"C:\tmp\out dir", "中文 name (1)", OutputFormat.Json);
        Check(output.EndsWith(@"中文 name (1).json", StringComparison.Ordinal), "path keeps unicode and parens");

        var temp = new TemporarySourceService();
        string tmp = temp.CreateTemporaryCpp("void hls_main() {}", "粘贴 代码");
        Check(File.Exists(tmp), "temp source exists");
        temp.Cleanup();
        Check(!File.Exists(tmp), "temp source cleanup");

        string fakeExe = Path.Combine(Path.GetTempPath(), "predicate-expand.exe");
        File.WriteAllText(fakeExe, "");
        try
        {
            var locator = new PredicateExpandLocator();
            Check(locator.Find(fakeExe) == Path.GetFullPath(fakeExe), "manual locator");
        }
        finally
        {
            File.Delete(fakeExe);
        }

        var req = new GenerationRequest
        {
            PredicateExpandPath = @"C:\Program Files\RTLzz\predicate-expand.exe",
            SourcePath = @"C:\输入 目录\case(1).cpp",
            TopFunction = "hls_main",
            OutputDirectory = @"C:\输出 目录",
            OutputBaseName = "case(1)",
            Formats = new[] { OutputFormat.Text },
            UnrollLimit = 4096,
            IncludeDir = @"C:\include path\中文",
            ForceUint = true
        };
        var runner = new PredicateExpandRunner(pathService);
        var args = runner.BuildArguments(req, OutputFormat.Text, pathService.BuildOutputPath(req.OutputDirectory, req.OutputBaseName, OutputFormat.Text)).ToArray();
        Check(args.Contains(req.SourcePath), "source arg");
        Check(args.Contains("--top") && args.Contains("hls_main"), "top arg");
        Check(args.Contains("--unroll-limit") && args.Contains("4096"), "unroll arg");
        Check(args.Contains("-I" + req.IncludeDir), "include arg");
        Check(args.Contains("-include") && args.Contains("uint.hpp"), "force uint args");
        Check(args.Contains("-o"), "output arg");

        bool missingFormatRejected = false;
        try
        {
            RequestValidator.ValidateCommon("hls_main", 1, Array.Empty<OutputFormat>(), "", false, req.PredicateExpandPath);
        }
        catch
        {
            missingFormatRejected = true;
        }
        Check(missingFormatRejected, "no format rejected");
    }

    private static async Task RunRunnerSmokeAsync(string predicateExe, string includeDir, string outputDir)
    {
        Directory.CreateDirectory(outputDir);
        string source = Path.Combine(outputDir, "gui_smoke.cpp");
        await File.WriteAllTextAsync(source, """
#include <uint.hpp>
void hls_main(Int<8> a, Int<8>& out) {
    out = a;
}
""");
        var request = new GenerationRequest
        {
            PredicateExpandPath = predicateExe,
            SourcePath = source,
            TopFunction = "hls_main",
            OutputDirectory = outputDir,
            OutputBaseName = "gui_smoke",
            Formats = new[] { OutputFormat.Text, OutputFormat.Json },
            UnrollLimit = 4096,
            IncludeDir = includeDir,
            ForceUint = false
        };
        var runner = new PredicateExpandRunner(new OutputPathService());
        foreach (var format in request.Formats)
        {
            Console.WriteLine("Runner smoke format: " + format);
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60));
            var result = await runner.RunFormatAsync(request, format, new Progress<string>(line => Console.WriteLine(line)), cts.Token)
                .ConfigureAwait(false);
            Check(result.Success, "runner smoke " + format + ": " + result.Message + " " + result.StdErr);
            Check(File.Exists(result.OutputPath) && new FileInfo(result.OutputPath).Length > 0, "runner output nonempty");
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("Check failed: " + message);
    }

    private static T? FindControl<T>(Control parent) where T : Control
    {
        foreach (Control child in parent.Controls)
        {
            if (child is T matched) return matched;
            var nested = FindControl<T>(child);
            if (nested != null) return nested;
        }
        return null;
    }

    private static bool ContainsText(Control parent, string text)
    {
        foreach (Control child in parent.Controls)
        {
            if (child.Text == text) return true;
            if (ContainsText(child, text)) return true;
        }
        return false;
    }
}
