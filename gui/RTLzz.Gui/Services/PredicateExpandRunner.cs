using System.Diagnostics;
using System.Text;
using System.Text.Json;
using RTLzz.Gui.Models;

namespace RTLzz.Gui.Services;

public sealed class PredicateExpandRunner
{
    private readonly OutputPathService _paths;

    public PredicateExpandRunner(OutputPathService paths)
    {
        _paths = paths;
    }

    public IReadOnlyList<string> BuildArguments(GenerationRequest request, OutputFormat format, string outputPath)
    {
        var args = new List<string>
        {
            request.SourcePath,
            "--top", request.TopFunction,
            "--format", OutputFormatInfo.CliName(format),
            "--unroll-limit", request.UnrollLimit.ToString(),
            "--clang-arg", "-std=c++20"
        };

        if (!string.IsNullOrWhiteSpace(request.IncludeDir))
        {
            args.Add("--clang-arg");
            args.Add("-I" + request.IncludeDir);
        }

        if (request.ForceUint)
        {
            args.Add("--clang-arg");
            args.Add("-include");
            args.Add("--clang-arg");
            args.Add("uint.hpp");
        }

        args.Add("-o");
        args.Add(outputPath);
        return args;
    }

    public async Task<FormatRunResult> RunFormatAsync(
        GenerationRequest request,
        OutputFormat format,
        IProgress<string>? log,
        CancellationToken cancellationToken)
    {
        string outputPath = _paths.BuildOutputPath(request.OutputDirectory, request.OutputBaseName, format);
        Directory.CreateDirectory(request.OutputDirectory);

        var psi = new ProcessStartInfo
        {
            FileName = request.PredicateExpandPath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };

        foreach (string arg in BuildArguments(request, format, outputPath))
        {
            psi.ArgumentList.Add(arg);
        }

        AddRuntimePath(psi);

        log?.Report("[开始] " + OutputFormatInfo.DisplayName(format));
        using var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            stdout.AppendLine(e.Data);
            log?.Report("[stdout] " + e.Data);
        };
        process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            stderr.AppendLine(e.Data);
            log?.Report("[stderr] " + e.Data);
        };

        try
        {
            if (!process.Start())
            {
                return Fail(format, outputPath, -1, "", "", "无法启动 predicate-expand");
            }
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            TryKill(process);
            return Fail(format, outputPath, -1, stdout.ToString(), stderr.ToString(), "用户取消运行");
        }

        string outText = stdout.ToString();
        string errText = stderr.ToString();
        int exitCode = process.ExitCode;
        log?.Report("[exit] " + exitCode);

        if (exitCode != 0)
        {
            return Fail(format, outputPath, exitCode, outText, errText, "predicate-expand 返回非零退出码");
        }
        if (!File.Exists(outputPath))
        {
            return Fail(format, outputPath, exitCode, outText, errText, "输出文件没有生成");
        }
        if (new FileInfo(outputPath).Length <= 0)
        {
            return Fail(format, outputPath, exitCode, outText, errText, "输出文件为空");
        }
        if (format is OutputFormat.Json or OutputFormat.StableJson)
        {
            try
            {
                using FileStream fs = File.OpenRead(outputPath);
                using JsonDocument _ = await JsonDocument.ParseAsync(fs, cancellationToken: cancellationToken);
            }
            catch (Exception ex)
            {
                return Fail(format, outputPath, exitCode, outText, errText, "JSON 解析失败: " + ex.Message);
            }
        }

        string msg = $"{OutputFormatInfo.DisplayName(format)}: {outputPath}";
        log?.Report("[PASS] " + msg);
        return new FormatRunResult
        {
            Format = format,
            OutputPath = outputPath,
            ExitCode = exitCode,
            StdOut = outText,
            StdErr = errText,
            Success = true,
            Message = msg
        };
    }

    private static FormatRunResult Fail(OutputFormat format, string outputPath, int exitCode, string stdout, string stderr, string message)
    {
        return new FormatRunResult
        {
            Format = format,
            OutputPath = outputPath,
            ExitCode = exitCode,
            StdOut = stdout,
            StdErr = stderr,
            Success = false,
            Message = message
        };
    }

    private static void AddRuntimePath(ProcessStartInfo psi)
    {
        string baseDir = AppContext.BaseDirectory;
        var extra = new List<string>
        {
            Path.Combine(baseDir, "runtime"),
            Path.Combine(baseDir, "runtime", "bin")
        };
        string llvmBin = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "LLVM", "bin");
        if (Directory.Exists(llvmBin)) extra.Add(llvmBin);
        string current = psi.Environment.TryGetValue("PATH", out string? path) ? path ?? "" : "";
        psi.Environment["PATH"] = string.Join(Path.PathSeparator, extra.Where(Directory.Exists)) +
                                  Path.PathSeparator + current;
    }

    private static void TryKill(Process process)
    {
        try
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
        }
        catch
        {
            // Best effort cancellation.
        }
    }
}
