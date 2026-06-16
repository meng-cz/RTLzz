using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;
using RTLzz.Gui.Models;
using RTLzz.Gui.Services;

namespace RTLzz.Gui;

public partial class MainForm : Form
{
    private readonly OutputPathService _outputPaths = new();
    private readonly SettingsService _settingsService = new();
    private readonly PredicateExpandLocator _locator = new();
    private readonly PredicateExpandRunner _runner;
    private readonly TemporarySourceService _temporarySources = new();
    private CancellationTokenSource? _cancelSource;
    private string? _selectedFile;

    public MainForm()
    {
        _runner = new PredicateExpandRunner(_outputPaths);
        InitializeComponent();
        WireEvents();
        LoadSettings();
        ApplyDefaultPaths();
        ConfigureInitialWindowBounds();
        WindowState = FormWindowState.Maximized;
    }

    public SplitContainer MainSplitForTests => mainSplit;
    public Panel LeftScrollPanelForTests => leftScrollPanel;
    public TableLayoutPanel LeftContentTableForTests => leftContentTable;
    public GroupBox RunGroupForTests => runGroup;
    public Button ChooseIncludeDirButtonForTests => chooseIncludeDirButton;
    public Rectangle InitialNormalBoundsForTests { get; private set; }
    public bool ScrollLayoutUpdatedForTests { get; private set; }

    public void ResetScrollLayoutUpdatedForTests()
    {
        ScrollLayoutUpdatedForTests = false;
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        SaveSettings();
        _temporarySources.Cleanup();
        base.OnFormClosing(e);
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        SetInitialSplitterDistance();
        BeginInvoke(new Action(() =>
        {
            leftScrollPanel.AutoScrollPosition = Point.Empty;
            UpdateLeftScrollLayout();
        }));
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        ScheduleLeftScrollLayoutUpdate();
    }

    protected override void OnDpiChanged(DpiChangedEventArgs e)
    {
        base.OnDpiChanged(e);
        ConfigureInitialWindowBounds();
        ScheduleLeftScrollLayoutUpdate(resetScrollToTop: true);
    }

    private void ConfigureInitialWindowBounds()
    {
        Rectangle working = Screen.FromControl(this).WorkingArea;
        int minWidth = Math.Min(1000, Math.Max(640, working.Width));
        int minHeight = Math.Min(650, Math.Max(480, working.Height));
        MinimumSize = new Size(minWidth, minHeight);

        int width = Math.Min(working.Width, Math.Max(minWidth, (int)Math.Round(working.Width * 0.90)));
        int height = Math.Min(working.Height, Math.Max(minHeight, (int)Math.Round(working.Height * 0.90)));
        int x = working.Left + Math.Max(0, (working.Width - width) / 2);
        int y = working.Top + Math.Max(0, (working.Height - height) / 2);
        InitialNormalBoundsForTests = new Rectangle(x, y, width, height);

        if (WindowState == FormWindowState.Normal)
        {
            Bounds = InitialNormalBoundsForTests;
        }
    }

    private void SetInitialSplitterDistance()
    {
        if (mainSplit.ClientSize.Width <= 0) return;
        int desired = Math.Max(mainSplit.Panel1MinSize, (int)(mainSplit.ClientSize.Width * 0.42));
        int max = Math.Max(mainSplit.Panel1MinSize, mainSplit.ClientSize.Width - mainSplit.Panel2MinSize - mainSplit.SplitterWidth);
        if (max >= mainSplit.Panel1MinSize)
        {
            mainSplit.SplitterDistance = Math.Min(desired, max);
        }
    }

    private void ScheduleLeftScrollLayoutUpdate(bool resetScrollToTop = false)
    {
        if (!IsHandleCreated) return;
        BeginInvoke(new Action(() =>
        {
            if (resetScrollToTop)
            {
                leftScrollPanel.AutoScrollPosition = Point.Empty;
            }
            UpdateLeftScrollLayout();
        }));
    }

    private void UpdateLeftScrollLayout()
    {
        if (leftScrollPanel == null || leftContentTable == null) return;

        leftContentTable.SuspendLayout();
        try
        {
            leftScrollPanel.HorizontalScroll.Enabled = false;
            leftScrollPanel.HorizontalScroll.Visible = false;

            int reserve = SystemInformation.VerticalScrollBarWidth
                + leftContentTable.Margin.Horizontal
                + leftScrollPanel.Padding.Horizontal
                + 8;
            int availableWidth = Math.Max(0, leftScrollPanel.ClientSize.Width - reserve);
            leftContentTable.Width = availableWidth;

            leftContentTable.PerformLayout();
            int preferredHeight = leftContentTable.GetPreferredSize(new Size(availableWidth, 0)).Height;
            leftScrollPanel.AutoScrollMinSize = new Size(0, preferredHeight + 20);
            ScrollLayoutUpdatedForTests = true;
        }
        finally
        {
            leftContentTable.ResumeLayout(performLayout: true);
        }
    }

    private void WireEvents()
    {
        chooseFileButton.Click += (_, _) => ChooseFile();
        clearFileButton.Click += (_, _) => ClearFile();
        clearCodeButton.Click += (_, _) => codeText.Clear();
        chooseIncludeDirButton.Click += (_, _) => ChooseFolder(includeDirText);
        chooseOutputDirButton.Click += (_, _) => ChooseFolder(outputDirText);
        resetOutputDirButton.Click += (_, _) => ApplyDefaultOutputDirectory();
        choosePredicateButton.Click += (_, _) => ChoosePredicateExpand();
        openOutputDirButton.Click += (_, _) => OpenOutputDirectory();
        selectAllFormatsButton.Click += (_, _) => SetAllFormats(true);
        clearFormatsButton.Click += (_, _) => SetAllFormats(false);
        runButton.Click += async (_, _) => await RunAsync();
        cancelButton.Click += (_, _) => _cancelSource?.Cancel();
        useFileRadio.CheckedChanged += (_, _) => UpdateInputMode();
        usePastedRadio.CheckedChanged += (_, _) => UpdateInputMode();
        mainSplit.SplitterMoved += (_, _) => ScheduleLeftScrollLayoutUpdate();
        DragEnter += MainForm_DragEnter;
        DragDrop += MainForm_DragDrop;
        dropLabel.DragEnter += MainForm_DragEnter;
        dropLabel.DragDrop += MainForm_DragDrop;
        filePathText.TextChanged += (_, _) =>
        {
            toolTip.SetToolTip(filePathText, filePathText.Text);
            if (useFileRadio.Checked)
            {
                outputBaseText.Text = _outputPaths.DefaultBaseNameForFile(filePathText.Text);
                ApplyDefaultOutputDirectory();
            }
        };
        includeDirText.TextChanged += (_, _) => toolTip.SetToolTip(includeDirText, includeDirText.Text);
        predicatePathText.TextChanged += (_, _) => toolTip.SetToolTip(predicatePathText, predicatePathText.Text);
        outputDirText.TextChanged += (_, _) => toolTip.SetToolTip(outputDirText, outputDirText.Text);
    }

    private void LoadSettings()
    {
        var settings = _settingsService.Load();
        topFunctionText.Text = string.IsNullOrWhiteSpace(settings.TopFunction) ? "hls_main" : settings.TopFunction;
        unrollLimitBox.Value = Math.Clamp(settings.UnrollLimit, 1, 1_000_000);
        includeDirText.Text = settings.IncludeDir;
        predicatePathText.Text = settings.PredicateExpandPath;
        forceUintCheck.Checked = settings.ForceUint;
        textCheck.Checked = settings.FormatText;
        jsonCheck.Checked = settings.FormatJson;
        stableJsonCheck.Checked = settings.FormatStableJson;
        smtCheck.Checked = settings.FormatSmt;
        if (!string.IsNullOrWhiteSpace(settings.OutputDirectory)) outputDirText.Text = settings.OutputDirectory;
    }

    private void SaveSettings()
    {
        _settingsService.Save(new GuiSettings
        {
            OutputDirectory = outputDirText.Text,
            TopFunction = topFunctionText.Text,
            UnrollLimit = (int)unrollLimitBox.Value,
            IncludeDir = includeDirText.Text,
            PredicateExpandPath = predicatePathText.Text,
            ForceUint = forceUintCheck.Checked,
            FormatText = textCheck.Checked,
            FormatJson = jsonCheck.Checked,
            FormatStableJson = stableJsonCheck.Checked,
            FormatSmt = smtCheck.Checked
        });
    }

    private void ApplyDefaultPaths()
    {
        string? packagedInclude = FindPackagedIncludeDir();
        if (string.IsNullOrWhiteSpace(includeDirText.Text) || !Directory.Exists(includeDirText.Text))
        {
            if (packagedInclude != null)
            {
                includeDirText.Text = packagedInclude;
            }
            else
            {
                string? projectRoot = FindProjectRoot();
                if (projectRoot != null)
                {
                    string include = Path.Combine(projectRoot, "third_party", "vulsim", "vullib");
                    if (Directory.Exists(include)) includeDirText.Text = include;
                }
            }
        }
        if (string.IsNullOrWhiteSpace(predicatePathText.Text) || !File.Exists(predicatePathText.Text))
        {
            predicatePathText.Text = _locator.Find() ?? "";
        }
        if (string.IsNullOrWhiteSpace(outputBaseText.Text)) outputBaseText.Text = "pasted_code";
        if (string.IsNullOrWhiteSpace(outputDirText.Text) || !IsUsableOutputDirectory(outputDirText.Text)) ApplyDefaultOutputDirectory();
        UpdateInputMode();
    }

    private static string? FindPackagedIncludeDir()
    {
        string include = Path.Combine(AppContext.BaseDirectory, "third_party", "vulsim", "vullib");
        return Directory.Exists(include) ? include : null;
    }

    private static bool IsUsableOutputDirectory(string path)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(path)) return false;
            string full = Path.GetFullPath(path);
            string? root = Path.GetPathRoot(full);
            return !string.IsNullOrWhiteSpace(root) && Directory.Exists(root);
        }
        catch
        {
            return false;
        }
    }

    private void ApplyDefaultOutputDirectory()
    {
        string baseName = OutputPathService.SanitizeBaseName(outputBaseText.Text);
        outputDirText.Text = _outputPaths.DefaultOutputDirectory(baseName);
    }

    private void ChooseFile()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "C/C++ source (*.cpp;*.cc;*.cxx;*.c)|*.cpp;*.cc;*.cxx;*.c|All files (*.*)|*.*"
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            SetSelectedFile(dialog.FileName);
        }
    }

    private void SetSelectedFile(string path)
    {
        try
        {
            RequestValidator.ValidateSourceFile(path);
            _selectedFile = path;
            filePathText.Text = path;
            useFileRadio.Checked = true;
            AppendLog("[INFO] 已选择文件: " + path);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "输入文件错误", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void ClearFile()
    {
        _selectedFile = null;
        filePathText.Clear();
        if (useFileRadio.Checked)
        {
            outputBaseText.Text = "pasted_code";
            ApplyDefaultOutputDirectory();
        }
    }

    private void ChooseFolder(TextBox target)
    {
        using var dialog = new FolderBrowserDialog { SelectedPath = target.Text };
        if (dialog.ShowDialog(this) == DialogResult.OK) target.Text = dialog.SelectedPath;
    }

    private void ChoosePredicateExpand()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "predicate-expand.exe|predicate-expand.exe|Executable (*.exe)|*.exe|All files (*.*)|*.*"
        };
        if (dialog.ShowDialog(this) == DialogResult.OK) predicatePathText.Text = dialog.FileName;
    }

    private void OpenOutputDirectory()
    {
        try
        {
            Directory.CreateDirectory(outputDirText.Text);
            Process.Start(new ProcessStartInfo { FileName = outputDirText.Text, UseShellExecute = true });
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "无法打开输出目录", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void SetAllFormats(bool value)
    {
        textCheck.Checked = value;
        jsonCheck.Checked = value;
        stableJsonCheck.Checked = value;
        smtCheck.Checked = value;
    }

    private void UpdateInputMode()
    {
        bool useFile = useFileRadio.Checked;
        filePathText.Enabled = useFile;
        chooseFileButton.Enabled = useFile;
        clearFileButton.Enabled = useFile;
        codeText.Enabled = !useFile;
        clearCodeButton.Enabled = !useFile;
        if (!useFile && outputBaseText.Text == _outputPaths.DefaultBaseNameForFile(filePathText.Text))
        {
            outputBaseText.Text = "pasted_code";
            ApplyDefaultOutputDirectory();
        }
        ScheduleLeftScrollLayoutUpdate();
    }

    private void MainForm_DragEnter(object? sender, DragEventArgs e)
    {
        e.Effect = e.Data?.GetDataPresent(DataFormats.FileDrop) == true ? DragDropEffects.Copy : DragDropEffects.None;
    }

    private void MainForm_DragDrop(object? sender, DragEventArgs e)
    {
        if (e.Data?.GetData(DataFormats.FileDrop) is string[] files && files.Length > 0)
        {
            SetSelectedFile(files[0]);
        }
    }

    private IReadOnlyList<OutputFormat> SelectedFormats()
    {
        var formats = new List<OutputFormat>();
        if (textCheck.Checked) formats.Add(OutputFormat.Text);
        if (jsonCheck.Checked) formats.Add(OutputFormat.Json);
        if (stableJsonCheck.Checked) formats.Add(OutputFormat.StableJson);
        if (smtCheck.Checked) formats.Add(OutputFormat.Smt);
        return formats;
    }

    private async Task RunAsync()
    {
        _cancelSource = new CancellationTokenSource();
        SetRunning(true);
        int pass = 0;
        int fail = 0;
        string? tempSource = null;

        try
        {
            logText.Clear();
            summaryLabel.Text = "成功 0 项，失败 0 项";
            string source;
            if (useFileRadio.Checked)
            {
                source = _selectedFile ?? filePathText.Text;
                RequestValidator.ValidateSourceFile(source);
            }
            else
            {
                if (string.IsNullOrWhiteSpace(codeText.Text)) throw new InvalidOperationException("没有粘贴代码。");
                tempSource = _temporarySources.CreateTemporaryCpp(codeText.Text, OutputPathService.SanitizeBaseName(outputBaseText.Text));
                source = tempSource;
            }

            var formats = SelectedFormats();
            string predicate = _locator.Find(predicatePathText.Text) ?? "";
            RequestValidator.ValidateCommon(topFunctionText.Text, (int)unrollLimitBox.Value, formats, includeDirText.Text, forceUintCheck.Checked, predicate);
            predicatePathText.Text = predicate;
            Directory.CreateDirectory(outputDirText.Text);

            var request = new GenerationRequest
            {
                PredicateExpandPath = predicate,
                SourcePath = source,
                TopFunction = topFunctionText.Text.Trim(),
                OutputDirectory = outputDirText.Text,
                OutputBaseName = OutputPathService.SanitizeBaseName(outputBaseText.Text),
                Formats = formats,
                UnrollLimit = (int)unrollLimitBox.Value,
                IncludeDir = includeDirText.Text,
                ForceUint = forceUintCheck.Checked
            };

            progressBar.Maximum = formats.Count;
            progressBar.Value = 0;
            foreach (var format in formats)
            {
                statusLabel.Text = "运行 " + OutputFormatInfo.DisplayName(format);
                var result = await _runner.RunFormatAsync(request, format, new Progress<string>(AppendLog), _cancelSource.Token);
                if (result.Success) pass++;
                else
                {
                    fail++;
                    AppendLog("[FAIL] " + OutputFormatInfo.DisplayName(format) + ": " + result.Message);
                    if (!string.IsNullOrWhiteSpace(result.StdErr)) AppendLog(result.StdErr);
                }
                summaryLabel.Text = $"成功 {pass} 项，失败 {fail} 项";
                progressBar.Value = Math.Min(progressBar.Value + 1, progressBar.Maximum);
            }

            AppendLog($"完成：成功 {pass} 项，失败 {fail} 项");
            statusLabel.Text = fail == 0 ? "完成" : "完成，有失败项";
        }
        catch (OperationCanceledException)
        {
            AppendLog("[CANCEL] 用户取消运行");
            statusLabel.Text = "已取消";
        }
        catch (Exception ex)
        {
            fail++;
            AppendLog("[FAIL] " + ex.Message);
            statusLabel.Text = "失败";
            MessageBox.Show(this, ex.Message, "运行失败", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        finally
        {
            if (tempSource != null) _temporarySources.Cleanup();
            SetRunning(false);
            _cancelSource?.Dispose();
            _cancelSource = null;
        }
    }

    private void SetRunning(bool running)
    {
        runButton.Enabled = !running;
        cancelButton.Enabled = running;
        useFileRadio.Enabled = !running;
        usePastedRadio.Enabled = !running;
        chooseFileButton.Enabled = !running && useFileRadio.Checked;
        clearFileButton.Enabled = !running && useFileRadio.Checked;
        codeText.Enabled = !running && usePastedRadio.Checked;
    }

    private void AppendLog(string line)
    {
        if (InvokeRequired)
        {
            BeginInvoke(new Action<string>(AppendLog), line);
            return;
        }
        logText.AppendText(line + Environment.NewLine);
    }

    private static string? FindProjectRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")) && Directory.Exists(Path.Combine(dir.FullName, "src")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        dir = new DirectoryInfo(Directory.GetCurrentDirectory());
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")) && Directory.Exists(Path.Combine(dir.FullName, "src")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return null;
    }
}
