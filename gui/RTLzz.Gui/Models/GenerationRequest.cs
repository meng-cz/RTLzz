namespace RTLzz.Gui.Models;

public sealed class GenerationRequest
{
    public required string PredicateExpandPath { get; init; }
    public required string SourcePath { get; init; }
    public required string TopFunction { get; init; }
    public required string OutputDirectory { get; init; }
    public required string OutputBaseName { get; init; }
    public required IReadOnlyList<OutputFormat> Formats { get; init; }
    public int UnrollLimit { get; init; } = 4096;
    public string IncludeDir { get; init; } = "";
    public bool ForceUint { get; init; }
}
