using RTLzz.Gui.Models;

namespace RTLzz.Gui.Models;

public sealed class GuiSettings
{
    public string OutputDirectory { get; set; } = "";
    public string TopFunction { get; set; } = "hls_main";
    public int UnrollLimit { get; set; } = 4096;
    public string IncludeDir { get; set; } = "";
    public string PredicateExpandPath { get; set; } = "";
    public bool ForceUint { get; set; }
    public bool FormatText { get; set; } = true;
    public bool FormatJson { get; set; } = true;
    public bool FormatStableJson { get; set; } = true;
    public bool FormatSmt { get; set; } = true;

    public IReadOnlyList<OutputFormat> SelectedFormats()
    {
        var formats = new List<OutputFormat>();
        if (FormatText) formats.Add(OutputFormat.Text);
        if (FormatJson) formats.Add(OutputFormat.Json);
        if (FormatStableJson) formats.Add(OutputFormat.StableJson);
        if (FormatSmt) formats.Add(OutputFormat.Smt);
        return formats;
    }
}
