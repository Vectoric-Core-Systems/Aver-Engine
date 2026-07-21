namespace Aver.Scripting;

/// <summary>
/// The engine log, as a script sees it.
/// </summary>
/// <remarks>
/// A hosted CLR runs inside a GUI process with no console attached, so <c>Console.WriteLine</c>
/// from a behaviour goes nowhere a user will ever look. Everything here is routed to the engine's
/// own log through a sink the host bridge installs at bootstrap.
///
/// When there is no host — the standalone <c>Aver.Scripting.Sample</c> process, or a unit test —
/// the sink is null and output falls back to the console, so the same binding assembly is usable
/// both in-process and out.
/// </remarks>
public static class Log
{
    /// <summary>Levels match <c>aver::LogLevel</c> and the AVER_SCRIPT_LOG_* codes in scripting_abi.h.</summary>
    public enum Level
    {
        Trace = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
    }

    /// <summary>Receives every log call. Installed by the host bridge; not for script use.</summary>
    public delegate void Sink(Level level, string message);

    private static Sink? s_sink;

    /// <summary>
    /// Installs (or clears, with null) the host's log sink. Called by <c>Aver.Scripting.Bridge</c>
    /// during bootstrap. Public only because the bridge is a separate assembly.
    /// </summary>
    public static void SetSink(Sink? sink) => s_sink = sink;

    /// <summary>True when a host is receiving these messages rather than the console.</summary>
    public static bool HasHost => s_sink is not null;

    public static void Trace(string message) => Write(Level.Trace, message);
    public static void Info(string message) => Write(Level.Info, message);
    public static void Warn(string message) => Write(Level.Warn, message);
    public static void Error(string message) => Write(Level.Error, message);

    public static void Write(Level level, string message)
    {
        Sink? sink = s_sink;
        if (sink is not null)
            sink(level, message);
        else
            Console.WriteLine($"[{level}] {message}");
    }
}
