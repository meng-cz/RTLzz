namespace RTLzz.Gui.Models;

public sealed class FormatRunResult
{
    public required OutputFormat Format { get; init; }
    public required string OutputPath { get; init; }
    public required int ExitCode { get; init; }
    public required string StdOut { get; init; }
    public required string StdErr { get; init; }
    public required bool Success { get; init; }
    public required string Message { get; init; }
}
