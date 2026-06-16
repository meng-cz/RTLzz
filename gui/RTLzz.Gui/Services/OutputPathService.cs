using RTLzz.Gui.Models;

namespace RTLzz.Gui.Services;

public sealed class OutputPathService
{
    public string DefaultRootDirectory()
    {
        string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        return Path.Combine(desktop, "RTLzz_Output");
    }

    public string DefaultBaseNameForFile(string? sourcePath)
    {
        if (string.IsNullOrWhiteSpace(sourcePath)) return "pasted_code";
        string name = Path.GetFileNameWithoutExtension(sourcePath);
        return SanitizeBaseName(string.IsNullOrWhiteSpace(name) ? "pasted_code" : name);
    }

    public string DefaultOutputDirectory(string baseName)
    {
        return Path.Combine(DefaultRootDirectory(), SanitizeBaseName(baseName));
    }

    public string BuildOutputPath(string directory, string baseName, OutputFormat format)
    {
        return Path.Combine(directory, SanitizeBaseName(baseName) + OutputFormatInfo.Extension(format));
    }

    public static string SanitizeBaseName(string value)
    {
        string trimmed = string.IsNullOrWhiteSpace(value) ? "pasted_code" : value.Trim();
        foreach (char c in Path.GetInvalidFileNameChars())
        {
            trimmed = trimmed.Replace(c, '_');
        }
        return string.IsNullOrWhiteSpace(trimmed) ? "pasted_code" : trimmed;
    }
}
