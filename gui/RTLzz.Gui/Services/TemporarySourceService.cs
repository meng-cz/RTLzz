namespace RTLzz.Gui.Services;

public sealed class TemporarySourceService : IDisposable
{
    private readonly List<string> _ownedDirectories = new();

    public string CreateTemporaryCpp(string code, string baseName)
    {
        string dir = Path.Combine(Path.GetTempPath(), "RTLzz-GUI", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        _ownedDirectories.Add(dir);
        string file = Path.Combine(dir, OutputPathService.SanitizeBaseName(baseName) + ".cpp");
        File.WriteAllText(file, code);
        return file;
    }

    public void Cleanup()
    {
        foreach (string dir in _ownedDirectories.ToArray())
        {
            try
            {
                if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
            }
            catch
            {
                // Temporary cleanup must not hide the generation result.
            }
            _ownedDirectories.Remove(dir);
        }
    }

    public void Dispose() => Cleanup();
}
