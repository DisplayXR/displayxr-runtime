// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace DisplayXR.Dashboard.Model;

/// <summary>
/// One <c>displayxr-cli</c> invocation as an argument vector. Every CLI run goes
/// through this, never through a command-line string: the process source hands
/// <see cref="Args"/> to <c>ProcessStartInfo.ArgumentList</c>, so an id or a value
/// (a Linux manifest id, a screen key, a hotkey) reaches the CLI as exactly one
/// argument whatever it contains — no quoting rule to get wrong. <see cref="ToString"/>
/// is only the readable form for the log pane and the "run this as an administrator"
/// hints, quoted the way the Windows C runtime splits it back.
/// </summary>
public sealed class CliArgs : IEquatable<CliArgs>
{
    public IReadOnlyList<string> Args { get; }

    public CliArgs(params string[] args) : this((IEnumerable<string>)args) { }

    public CliArgs(IEnumerable<string> args)
    {
        Args = args.Select(a => a ?? "").ToArray();
    }

    /// <summary>
    /// A fixed command split on spaces — for literals the code spells out
    /// (<c>"info --json"</c>), never for anything that carries data.
    /// </summary>
    public static CliArgs Words(string literal) =>
        new(literal.Split(' ', StringSplitOptions.RemoveEmptyEntries));

    /// <summary>This vector with <paramref name="more"/> appended.</summary>
    public CliArgs With(params string[] more) => new(Args.Concat(more));

    /// <summary>The readable command line (without the program name).</summary>
    public override string ToString() => string.Join(" ", Args.Select(Display));

    /// <summary>
    /// One argument as a person would type it: bare when that is unambiguous, else
    /// double-quoted with the CommandLineToArgvW / MSVC CRT escapes (a quote as
    /// <c>\"</c>, backslashes doubled only before a quote or the closing quote).
    /// </summary>
    public static string Display(string arg)
    {
        if (arg.Length > 0 && !arg.Any(c => char.IsWhiteSpace(c) || c == '"')) return arg;
        var sb = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in arg)
        {
            if (c == '\\') { slashes++; continue; }
            if (c == '"')
            {
                sb.Append('\\', slashes * 2 + 1).Append('"');
            }
            else
            {
                sb.Append('\\', slashes).Append(c);
            }
            slashes = 0;
        }
        sb.Append('\\', slashes * 2).Append('"');
        return sb.ToString();
    }

    public bool Equals(CliArgs? other) => other is not null && Args.SequenceEqual(other.Args);
    public override bool Equals(object? obj) => Equals(obj as CliArgs);
    public override int GetHashCode() => Args.Aggregate(17, (h, a) => h * 31 + a.GetHashCode());
}
