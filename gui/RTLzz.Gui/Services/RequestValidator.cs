using RTLzz.Gui.Models;

namespace RTLzz.Gui.Services;

public static class RequestValidator
{
    private static readonly HashSet<string> AllowedExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".cpp", ".cc", ".cxx", ".c"
    };

    public static void ValidateSourceFile(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) throw new InvalidOperationException("没有选择输入文件。");
        if (!File.Exists(path)) throw new FileNotFoundException("输入文件不存在。", path);
        string ext = Path.GetExtension(path);
        if (!AllowedExtensions.Contains(ext)) throw new InvalidOperationException("不支持的文件扩展名: " + ext);
    }

    public static void ValidateCommon(
        string top,
        int unrollLimit,
        IReadOnlyList<OutputFormat> formats,
        string includeDir,
        bool forceUint,
        string predicateExpandPath)
    {
        if (string.IsNullOrWhiteSpace(top)) throw new InvalidOperationException("顶层函数不能为空。");
        if (unrollLimit <= 0 || unrollLimit > 1_000_000) throw new InvalidOperationException("UnrollLimit 必须是 1 到 1000000 之间的整数。");
        if (formats.Count == 0) throw new InvalidOperationException("至少选择一种输出格式。");
        if (!string.IsNullOrWhiteSpace(includeDir) && !Directory.Exists(includeDir)) throw new DirectoryNotFoundException("IncludeDir 不存在: " + includeDir);
        if (forceUint)
        {
            string uintPath = string.IsNullOrWhiteSpace(includeDir) ? "" : Path.Combine(includeDir, "uint.hpp");
            if (string.IsNullOrWhiteSpace(uintPath) || !File.Exists(uintPath)) throw new FileNotFoundException("ForceUint 已启用，但 IncludeDir 中找不到 uint.hpp。", uintPath);
        }
        if (string.IsNullOrWhiteSpace(predicateExpandPath) || !File.Exists(predicateExpandPath)) throw new FileNotFoundException("找不到 predicate-expand.exe。", predicateExpandPath);
    }
}
