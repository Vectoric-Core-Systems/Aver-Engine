// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// The engine log as a script sees it, routed to the host's sink or to the console.
namespace Aver.Scripting;

/// <summary>Script-facing logging. Goes to the host's sink, or to the console when there is none.</summary>
public static class Log
{
    /// <summary>Levels match <c>aver::LogLevel</c> and the AVER_SCRIPT_LOG_* codes in scripting_abi.h.</summary>
    public enum Level
    {
        Trace = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
        /// <summary>Threatens process stability. Wakes the crash reporter on the engine side.</summary>
        Critical = 4,
        /// <summary>Death is imminent. The engine writes a crash report and terminates; this does not return.</summary>
        Fatal = 5,
    }

    /// <summary>Receives every log call. Installed by the host bridge; not for script use.</summary>
    public delegate void Sink(Level level, string message);

    private static Sink? s_sink;

    /// <summary>Installs (or clears, with null) the host's log sink.</summary>
    public static void SetSink(Sink? sink) => s_sink = sink;

    /// <summary>True when a host is receiving these messages rather than the console.</summary>
    public static bool HasHost => s_sink is not null;

    /// <summary>Logs at Trace.</summary>
    public static void Trace(string message) => Write(Level.Trace, message);
    /// <summary>Logs at Info.</summary>
    public static void Info(string message) => Write(Level.Info, message);
    /// <summary>Logs at Warn.</summary>
    public static void Warn(string message) => Write(Level.Warn, message);
    /// <summary>Logs at Error.</summary>
    public static void Error(string message) => Write(Level.Error, message);
    /// <summary>Logs at Critical: the process is still running but is now a candidate to die.</summary>
    public static void Critical(string message) => Write(Level.Critical, message);
    /// <summary>
    /// Logs at Fatal. The engine writes a crash report and TERMINATES THE PROCESS -- this call does
    /// not return, and no code after it runs. Use it only where continuing is genuinely impossible.
    /// </summary>
    public static void Fatal(string message) => Write(Level.Fatal, message);

    /// <summary>Logs one message at a level.</summary>
    public static void Write(Level level, string message)
    {
        Sink? sink = s_sink;
        if (sink is not null)
            sink(level, message);
        else
            Console.WriteLine($"[{level}] {message}");
    }
}
