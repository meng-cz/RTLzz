namespace RTLzz.Gui.Services;

public sealed class PredicateExpandLocator
{
    public string? Find(string? userSpecifiedPath = null)
    {
        foreach (string candidate in CandidatePaths(userSpecifiedPath))
        {
            if (File.Exists(candidate)) return Path.GetFullPath(candidate);
        }
        return null;
    }

    public IReadOnlyList<string> CandidatePaths(string? userSpecifiedPath = null)
    {
        var candidates = new List<string>();
        string baseDir = AppContext.BaseDirectory;
        candidates.Add(Path.Combine(baseDir, "predicate-expand.exe"));
        candidates.Add(Path.Combine(baseDir, "runtime", "predicate-expand.exe"));

        if (!string.IsNullOrWhiteSpace(userSpecifiedPath))
        {
            candidates.Add(userSpecifiedPath);
        }

        string? projectRoot = FindProjectRoot(baseDir) ?? FindProjectRoot(Directory.GetCurrentDirectory());
        if (projectRoot != null)
        {
            candidates.Add(Path.Combine(projectRoot, "build", "Release", "predicate-expand.exe"));
            candidates.Add(Path.Combine(projectRoot, "build", "predicate-expand.exe"));
            candidates.Add(Path.Combine(projectRoot, "build", "Debug", "predicate-expand.exe"));
        }

        return candidates;
    }

    private static string? FindProjectRoot(string start)
    {
        var dir = new DirectoryInfo(start);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")) &&
                Directory.Exists(Path.Combine(dir.FullName, "src")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return null;
    }
}
