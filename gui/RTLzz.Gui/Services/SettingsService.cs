using System.Text.Json;
using RTLzz.Gui.Models;

namespace RTLzz.Gui.Services;

public sealed class SettingsService
{
    private readonly JsonSerializerOptions _jsonOptions = new() { WriteIndented = true };

    public string SettingsPath
    {
        get
        {
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            return Path.Combine(local, "RTLzz", "settings.json");
        }
    }

    public GuiSettings Load()
    {
        try
        {
            if (!File.Exists(SettingsPath)) return new GuiSettings();
            string json = File.ReadAllText(SettingsPath);
            return JsonSerializer.Deserialize<GuiSettings>(json) ?? new GuiSettings();
        }
        catch
        {
            return new GuiSettings();
        }
    }

    public void Save(GuiSettings settings)
    {
        string? dir = Path.GetDirectoryName(SettingsPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(SettingsPath, JsonSerializer.Serialize(settings, _jsonOptions));
    }
}
