namespace RTLzz.Gui.Models;

public enum OutputFormat
{
    Text,
    Json,
    StableJson,
    Smt
}

public static class OutputFormatInfo
{
    public static string CliName(OutputFormat format) => format switch
    {
        OutputFormat.Text => "text",
        OutputFormat.Json => "json",
        OutputFormat.StableJson => "stable_json",
        OutputFormat.Smt => "smt",
        _ => throw new ArgumentOutOfRangeException(nameof(format), format, null)
    };

    public static string DisplayName(OutputFormat format) => format switch
    {
        OutputFormat.Text => "Text",
        OutputFormat.Json => "JSON",
        OutputFormat.StableJson => "Stable JSON",
        OutputFormat.Smt => "SMT",
        _ => throw new ArgumentOutOfRangeException(nameof(format), format, null)
    };

    public static string Extension(OutputFormat format) => format switch
    {
        OutputFormat.Text => ".txt",
        OutputFormat.Json => ".json",
        OutputFormat.StableJson => ".stable.json",
        OutputFormat.Smt => ".smt",
        _ => throw new ArgumentOutOfRangeException(nameof(format), format, null)
    };
}
