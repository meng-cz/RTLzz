using System.Drawing;
using System.Windows.Forms;

namespace RTLzz.Gui;

public partial class MainForm
{
    private RadioButton useFileRadio = null!;
    private RadioButton usePastedRadio = null!;
    private TextBox filePathText = null!;
    private Button chooseFileButton = null!;
    private Button clearFileButton = null!;
    private Label dropLabel = null!;
    private TextBox codeText = null!;
    private Button clearCodeButton = null!;
    private TextBox topFunctionText = null!;
    private CheckBox textCheck = null!;
    private CheckBox jsonCheck = null!;
    private CheckBox stableJsonCheck = null!;
    private CheckBox smtCheck = null!;
    private Button selectAllFormatsButton = null!;
    private Button clearFormatsButton = null!;
    private CheckBox forceUintCheck = null!;
    private NumericUpDown unrollLimitBox = null!;
    private TextBox includeDirText = null!;
    private Button chooseIncludeDirButton = null!;
    private TextBox predicatePathText = null!;
    private Button choosePredicateButton = null!;
    private TextBox outputBaseText = null!;
    private TextBox outputDirText = null!;
    private Button chooseOutputDirButton = null!;
    private Button resetOutputDirButton = null!;
    private Button openOutputDirButton = null!;
    private Button runButton = null!;
    private Button cancelButton = null!;
    private ProgressBar progressBar = null!;
    private Label statusLabel = null!;
    private Label summaryLabel = null!;
    private TextBox logText = null!;
    private ToolTip toolTip = null!;
    private SplitContainer mainSplit = null!;
    private Panel leftScrollPanel = null!;
    private TableLayoutPanel leftContentTable = null!;
    private GroupBox runGroup = null!;

    private void InitializeComponent()
    {
        SuspendLayout();
        Text = "RTLzz-GUI";
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
        MinimumSize = new Size(1000, 650);
        Size = new Size(1200, 760);
        AllowDrop = true;
        toolTip = new ToolTip { AutoPopDelay = 15000, InitialDelay = 400, ReshowDelay = 100 };

        mainSplit = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Size = new Size(1180, 720),
            Orientation = Orientation.Vertical,
            SplitterWidth = 6,
            Panel1MinSize = 480,
            Panel2MinSize = 420
        };
        Controls.Add(mainSplit);

        leftScrollPanel = new Panel
        {
            Dock = DockStyle.Fill,
            AutoScroll = true,
            Padding = new Padding(10)
        };
        mainSplit.Panel1.Controls.Add(leftScrollPanel);

        leftContentTable = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
            RowCount = 7
        };
        leftContentTable.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        leftScrollPanel.Controls.Add(leftContentTable);

        var right = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1, Padding = new Padding(10) };
        right.RowStyles.Add(new RowStyle(SizeType.Absolute, 95));
        right.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        mainSplit.Panel2.Controls.Add(right);

        leftContentTable.Controls.Add(BuildFileGroup(), 0, 0);
        leftContentTable.Controls.Add(BuildCodeGroup(), 0, 1);
        leftContentTable.Controls.Add(BuildTopGroup(), 0, 2);
        leftContentTable.Controls.Add(BuildFormatsGroup(), 0, 3);
        leftContentTable.Controls.Add(BuildAdvancedGroup(), 0, 4);
        leftContentTable.Controls.Add(BuildOutputGroup(), 0, 5);
        leftContentTable.Controls.Add(BuildRunGroup(), 0, 6);

        right.Controls.Add(BuildStatusGroup(), 0, 0);
        right.Controls.Add(BuildLogGroup(), 0, 1);
        ResumeLayout(performLayout: true);
    }

    private Control BuildFileGroup()
    {
        var group = new GroupBox { Text = "输入来源", Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0, 0, 0, 10) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 4, RowCount = 3, Padding = new Padding(10) };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 135));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 62));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        group.Controls.Add(panel);

        useFileRadio = new RadioButton { Text = "使用文件", Checked = true, Dock = DockStyle.Fill, AutoSize = true, MinimumSize = new Size(120, 28) };
        usePastedRadio = new RadioButton { Text = "使用粘贴代码", Dock = DockStyle.Fill, AutoSize = true, MinimumSize = new Size(140, 28) };
        panel.Controls.Add(useFileRadio, 0, 0);
        panel.Controls.Add(usePastedRadio, 0, 2);

        filePathText = new TextBox { Dock = DockStyle.Fill, ReadOnly = true };
        panel.Controls.Add(filePathText, 1, 0);
        panel.SetColumnSpan(filePathText, 1);
        chooseFileButton = new Button { Text = "选择文件", Dock = DockStyle.Fill, MinimumSize = new Size(100, 30), AutoSize = true };
        clearFileButton = new Button { Text = "清除文件", Dock = DockStyle.Fill, MinimumSize = new Size(90, 30), AutoSize = true };
        panel.Controls.Add(chooseFileButton, 2, 0);
        panel.Controls.Add(clearFileButton, 3, 0);

        dropLabel = new Label
        {
            Text = "将 .cpp / .cc / .cxx / .c 文件拖放到这里",
            Dock = DockStyle.Fill,
            BorderStyle = BorderStyle.FixedSingle,
            TextAlign = ContentAlignment.MiddleCenter,
            BackColor = Color.WhiteSmoke,
            MinimumSize = new Size(0, 52)
        };
        panel.Controls.Add(dropLabel, 0, 1);
        panel.SetColumnSpan(dropLabel, 4);
        return group;
    }

    private Control BuildCodeGroup()
    {
        var group = new GroupBox { Text = "代码粘贴", Dock = DockStyle.Top, Height = 265, MinimumSize = new Size(0, 250), Margin = new Padding(0, 0, 0, 10) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Padding = new Padding(10) };
        panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        group.Controls.Add(panel);

        codeText = new TextBox
        {
            Multiline = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            AcceptsTab = true,
            Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point),
            Dock = DockStyle.Fill,
            MinimumSize = new Size(0, 180)
        };
        clearCodeButton = new Button { Text = "清空代码", Dock = DockStyle.Right, Width = 110, MinimumSize = new Size(100, 32), AutoSize = true };
        panel.Controls.Add(codeText, 0, 0);
        panel.Controls.Add(clearCodeButton, 0, 1);
        return group;
    }

    private Control BuildTopGroup()
    {
        var group = new GroupBox { Text = "顶层函数", Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, MinimumSize = new Size(0, 78), Margin = new Padding(0, 0, 0, 10) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 2, RowCount = 1, Padding = new Padding(10) };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        group.Controls.Add(panel);
        panel.Controls.Add(new Label { Text = "函数名", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
        topFunctionText = new TextBox { Text = "hls_main", Dock = DockStyle.Fill };
        panel.Controls.Add(topFunctionText, 1, 0);
        return group;
    }

    private Control BuildFormatsGroup()
    {
        var group = new GroupBox { Text = "输出格式", Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, MinimumSize = new Size(0, 108), Margin = new Padding(0, 0, 0, 10) };
        var table = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, RowCount = 2, Padding = new Padding(10) };
        table.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        table.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        group.Controls.Add(table);
        var checks = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        textCheck = new CheckBox { Text = "Text", Checked = true, AutoSize = true, MinimumSize = new Size(70, 28) };
        jsonCheck = new CheckBox { Text = "JSON", Checked = true, AutoSize = true, MinimumSize = new Size(75, 28) };
        stableJsonCheck = new CheckBox { Text = "Stable JSON", Checked = true, AutoSize = true, MinimumSize = new Size(120, 28) };
        smtCheck = new CheckBox { Text = "SMT", Checked = true, AutoSize = true, MinimumSize = new Size(70, 28) };
        checks.Controls.AddRange(new Control[] { textCheck, jsonCheck, stableJsonCheck, smtCheck });
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = false };
        selectAllFormatsButton = new Button { Text = "全选", AutoSize = true, MinimumSize = new Size(85, 32) };
        clearFormatsButton = new Button { Text = "取消全选", AutoSize = true, MinimumSize = new Size(105, 32) };
        buttons.Controls.AddRange(new Control[] { selectAllFormatsButton, clearFormatsButton });
        table.Controls.Add(checks, 0, 0);
        table.Controls.Add(buttons, 0, 1);
        return group;
    }

    private Control BuildAdvancedGroup()
    {
        var group = new GroupBox { Text = "高级选项", Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, MinimumSize = new Size(0, 170), Margin = new Padding(0, 0, 0, 10) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 3, RowCount = 4, Padding = new Padding(10) };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 125));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 95));
        for (int i = 0; i < 4; ++i) panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        group.Controls.Add(panel);

        panel.Controls.Add(new Label { Text = "ForceUint", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
        var forcePanel = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = false };
        forceUintCheck = new CheckBox { Text = "ForceUint", AutoSize = true, MinimumSize = new Size(100, 28) };
        forcePanel.Controls.Add(forceUintCheck);
        forcePanel.Controls.Add(new Label { Text = "强制包含 uint.hpp", AutoSize = true, TextAlign = ContentAlignment.MiddleLeft, Padding = new Padding(4, 6, 0, 0) });
        panel.Controls.Add(forcePanel, 1, 0);
        panel.SetColumnSpan(forcePanel, 2);
        panel.Controls.Add(new Label { Text = "UnrollLimit", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 1);
        unrollLimitBox = new NumericUpDown { Minimum = 1, Maximum = 1000000, Value = 4096, Dock = DockStyle.Left, Width = 150, MinimumSize = new Size(130, 28) };
        panel.Controls.Add(unrollLimitBox, 1, 1);

        panel.Controls.Add(new Label { Text = "IncludeDir", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 2);
        includeDirText = new TextBox { Dock = DockStyle.Fill };
        chooseIncludeDirButton = new Button { Text = "选择", Dock = DockStyle.Fill, MinimumSize = new Size(80, 30), AutoSize = true };
        panel.Controls.Add(includeDirText, 1, 2);
        panel.Controls.Add(chooseIncludeDirButton, 2, 2);

        panel.Controls.Add(new Label { Text = "predicate-expand", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 3);
        predicatePathText = new TextBox { Dock = DockStyle.Fill };
        choosePredicateButton = new Button { Text = "选择", Dock = DockStyle.Fill, MinimumSize = new Size(80, 30), AutoSize = true };
        panel.Controls.Add(predicatePathText, 1, 3);
        panel.Controls.Add(choosePredicateButton, 2, 3);
        toolTip.SetToolTip(includeDirText, "IncludeDir");
        toolTip.SetToolTip(predicatePathText, "predicate-expand.exe");
        return group;
    }

    private Control BuildOutputGroup()
    {
        var group = new GroupBox { Text = "输出设置", Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, MinimumSize = new Size(0, 135), Margin = new Padding(0, 0, 0, 10) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 3, RowCount = 3, Padding = new Padding(10) };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 130));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        group.Controls.Add(panel);

        panel.Controls.Add(new Label { Text = "基本名称", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
        outputBaseText = new TextBox { Dock = DockStyle.Fill };
        panel.Controls.Add(outputBaseText, 1, 0);
        panel.SetColumnSpan(outputBaseText, 2);

        panel.Controls.Add(new Label { Text = "输出目录", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 1);
        outputDirText = new TextBox { Dock = DockStyle.Fill };
        chooseOutputDirButton = new Button { Text = "选择目录", Dock = DockStyle.Fill, MinimumSize = new Size(110, 30), AutoSize = true };
        resetOutputDirButton = new Button { Text = "恢复默认", AutoSize = true, MinimumSize = new Size(105, 32) };
        panel.Controls.Add(outputDirText, 1, 1);
        panel.Controls.Add(chooseOutputDirButton, 2, 1);

        var outputButtons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = false };
        openOutputDirButton = new Button { Text = "打开输出目录", AutoSize = true, MinimumSize = new Size(125, 32) };
        outputButtons.Controls.Add(resetOutputDirButton);
        outputButtons.Controls.Add(openOutputDirButton);
        panel.Controls.Add(new Label { Text = "", Dock = DockStyle.Fill }, 0, 2);
        panel.Controls.Add(outputButtons, 1, 2);
        panel.SetColumnSpan(outputButtons, 2);
        toolTip.SetToolTip(outputDirText, "输出目录");
        return group;
    }

    private Control BuildRunGroup()
    {
        runGroup = new GroupBox { Text = "运行", Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, MinimumSize = new Size(0, 105), Margin = new Padding(0, 0, 0, 10) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 4, RowCount = 2, Padding = new Padding(10) };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 125));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 170));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        runGroup.Controls.Add(panel);
        runButton = new Button { Text = "开始生成", Dock = DockStyle.Fill, MinimumSize = new Size(110, 32), AutoSize = true };
        cancelButton = new Button { Text = "取消", Dock = DockStyle.Fill, Enabled = false, MinimumSize = new Size(80, 32), AutoSize = true };
        progressBar = new ProgressBar { Dock = DockStyle.Fill };
        statusLabel = new Label { Text = "就绪", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
        summaryLabel = new Label { Text = "成功 0 项，失败 0 项", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
        panel.Controls.Add(runButton, 0, 0);
        panel.Controls.Add(cancelButton, 1, 0);
        panel.Controls.Add(progressBar, 2, 0);
        panel.Controls.Add(statusLabel, 3, 0);
        panel.Controls.Add(new Label { Text = "", Dock = DockStyle.Fill }, 0, 1);
        panel.Controls.Add(summaryLabel, 1, 1);
        panel.SetColumnSpan(summaryLabel, 3);
        return runGroup;
    }

    private Control BuildStatusGroup()
    {
        var group = new GroupBox { Text = "状态", Dock = DockStyle.Fill };
        var label = new Label
        {
            Text = "选择文件或粘贴代码，填写顶层函数后点击开始生成。",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Padding = new Padding(10),
            AutoEllipsis = false
        };
        group.MinimumSize = new Size(0, 85);
        group.Controls.Add(label);
        return group;
    }

    private Control BuildLogGroup()
    {
        var group = new GroupBox { Text = "日志", Dock = DockStyle.Fill };
        logText = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point),
            Dock = DockStyle.Fill
        };
        group.Controls.Add(logText);
        return group;
    }
}
