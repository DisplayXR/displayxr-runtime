// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// "Current values": every setting the Settings page writes, by its variable name, with its value and
/// where it comes from (<c>perf list --json</c>). Technical, so it is shown in Troubleshooting's
/// Advanced part, not on Settings.
/// </summary>
public static class CurrentValuesCard
{
    public static Control Build(PerfState? perf, string? error)
    {
        var body = U.VStack(12, U.CardTitle("Current values",
            "What a 3D app starting now would use for each setting on Settings: a setting outside the Dashboard first, then yours, then this PC's default."));
        if (perf is null)
        {
            body.Children.Add(U.Wrapped(error ?? "Reading the settings…", error is null ? "empty" : "soft"));
            return U.Card(body);
        }
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto") };
        g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        string[] heads = { "Setting", "Value", "Comes from" };
        for (int i = 0; i < heads.Length; i++)
        {
            var h = U.Text(heads[i], "meta");
            h.Margin = new Thickness(0, 0, i < 2 ? 18 : 0, 4);
            Grid.SetColumn(h, i);
            g.Children.Add(h);
        }
        int row = 1;
        foreach (var l in perf.Levers)
        {
            g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var name = U.Wrapped(l.Name, "mono");
            var value = U.Text(l.Value ?? "default", "mono");
            value.Foreground = l.IsSet ? Tokens.For(Level.Warn) : Tokens.Muted;
            var src = U.Chip(SourceWord(l.Source), l.Source == "env" ? Level.Warn : l.Source == "default" ? Level.Plain : Level.Info);
            ToolTip.SetTip(src, l.Source); // the CLI's own word
            name.Margin = new Thickness(0, 3, 18, 3);
            value.Margin = new Thickness(0, 3, 18, 3);
            src.Margin = new Thickness(0, 2, 0, 2);
            value.VerticalAlignment = VerticalAlignment.Center;
            name.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetRow(name, row); Grid.SetRow(value, row); Grid.SetRow(src, row);
            Grid.SetColumn(value, 1); Grid.SetColumn(src, 2);
            g.Children.Add(name); g.Children.Add(value); g.Children.Add(src);
            row++;
        }
        body.Children.Add(g);
        if (perf.UserFile.Length > 0)
            body.Children.Add(U.Wrapped($"Your settings file: {perf.UserFile}{(perf.UserWritten.Length > 0 ? $" (written {perf.UserWritten})" : "")}", "desc"));
        return U.Card(body);
    }

    /// <summary>Where a value comes from, in plain words (env / user / machine / default).</summary>
    public static string SourceWord(string source) => source switch
    {
        "env" => "outside setting",
        "user" => "you",
        "machine" => "this PC",
        "default" => "default",
        var other => other,
    };
}
