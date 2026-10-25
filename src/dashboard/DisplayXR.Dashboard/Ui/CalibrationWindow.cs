// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Ui;

/// <summary>
/// The camera-pose calibration window (tracking-provider-discovery.md §6, design §10): borderless
/// and full screen on the target monitor, black, a bright ring at the edges to light the face (+ / - adjust it), a white
/// cross on each target in turn, the solve result, and the live-check dot. All the logic is in
/// <see cref="CalibrationSession"/>; this class draws it, feeds it keys and the CLI's output, and owns
/// the one long-lived <c>tracking calibrate --watch --json</c> child (stopped on Esc, on close and as
/// soon as the session no longer needs it).
/// </summary>
public sealed class CalibrationWindow : Window
{
    private const double CrossArm = 22, CrossThickness = 2, LiveDot = 14;

    private readonly IProcessSource _cli;
    private readonly CalibrationSession _session;
    private readonly Screen? _screen;
    private readonly Action _onApplied;
    private IWatchProcess? _child;
    private bool _busy;

    private readonly Canvas _canvas = new();
    private readonly Border _ring = new() { IsHitTestVisible = false };
    // The ring lights the face so its reflection is visible: bright by default, + / - to adjust.
    private byte _ringLevel = 0xE0;
    private readonly Rectangle _crossH = new() { Fill = Brushes.White };
    private readonly Rectangle _crossV = new() { Fill = Brushes.White };
    private readonly Ellipse _hold = new() { Stroke = new SolidColorBrush(Color.FromRgb(0x50, 0x50, 0x50)), StrokeThickness = 1.5, Width = 34, Height = 34 };
    private readonly Ellipse _live = new() { Fill = new SolidColorBrush(Color.FromRgb(0x3c, 0xd0, 0x70)), Width = LiveDot, Height = LiveDot };
    private readonly TextBlock _prompt = Txt(20, 0xE0);
    private readonly TextBlock _status = Txt(14, 0x90);
    private readonly TextBlock _error = Txt(14, 0xE0, Color.FromRgb(0xF0, 0x90, 0x60));
    private readonly StackPanel _textPanel;
    private readonly Border _review = new()
    {
        Background = new SolidColorBrush(Color.FromRgb(0x14, 0x14, 0x14)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(0x40, 0x40, 0x40)),
        BorderThickness = new Thickness(1),
        CornerRadius = new CornerRadius(8),
        Padding = new Thickness(24, 18),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        MaxWidth = 720,
        IsVisible = false,
    };
    private string _reviewKey = "";

    public CalibrationWindow(IProcessSource cli, string screenKey, Screen? screen, string screenName, Action onApplied)
    {
        _cli = cli;
        _screen = screen;
        _onApplied = onApplied;
        _session = new CalibrationSession(screenKey, screen);
        Title = $"Calibrate camera — {screenName}";
        SystemDecorations = SystemDecorations.None;
        Background = Brushes.Black;
        CanResize = false;
        Topmost = true;
        ShowInTaskbar = false;
        Cursor = new Cursor(StandardCursorType.None);

        _textPanel = new StackPanel { Spacing = 6, MaxWidth = 900, HorizontalAlignment = HorizontalAlignment.Center, Children = { _prompt, _status, _error } };
        _canvas.Children.Add(_hold);
        _canvas.Children.Add(_crossH);
        _canvas.Children.Add(_crossV);
        _canvas.Children.Add(_live);
        Content = new Grid { Children = { _ring, _canvas, _textPanel, _review } };

        AddHandler(KeyDownEvent, OnKey, RoutingStrategies.Tunnel);
        // macOS delivers no KeyDown for Space here (seen on a MacBook: every other key
        // arrives, Space never does), but it does arrive as text input; take it there.
        AddHandler(TextInputEvent, (_, e) =>
        {
            if (e.Text == " ")
            {
                DashboardLog.Info($"calibration: space via text input (phase {_session.Phase})");
                e.Handled = true;
                _session.Space();
                Step();
            }
        }, RoutingStrategies.Tunnel);
        // A click anywhere also captures: on macOS a full-screen window may not hold
        // keyboard focus until it is clicked, and the click then also gives it focus.
        PointerPressed += (_, _) =>
        {
            Activate();
            Focus();
            _session.Space();
            Step();
        };
        Opened += (_, _) => OnOpened();
        Closed += (_, _) => StopChild();
        SizeChanged += (_, _) => Render();
    }

    private static TextBlock Txt(double size, byte grey, Color? color = null) => new()
    {
        FontSize = size,
        Foreground = new SolidColorBrush(color ?? Color.FromRgb(grey, grey, grey)),
        TextWrapping = TextWrapping.Wrap,
        TextAlignment = TextAlignment.Center,
        HorizontalAlignment = HorizontalAlignment.Center,
    };

    private void OnOpened()
    {
        PlaceOnTarget();
        WindowState = WindowState.FullScreen;
        Activate();
        Focus();
        // macOS animates a full-screen window into its own Space and the activation
        // above is lost during that transition (the first key beeps). Take key focus
        // again once it has settled.
        DispatcherTimer.RunOnce(() => { Activate(); Focus(); }, TimeSpan.FromMilliseconds(600));
        try
        {
            _child = _cli.StartStream(CalibrationCommands.Watch(_session.ScreenKey),
                l => Dispatcher.UIThread.Post(() => { _session.OnLine(l); Step(); }),
                code => Dispatcher.UIThread.Post(() => { _session.OnExit(code); Step(); }),
                n => DashboardLog.Info($"tracking calibrate says: {n}"));
        }
        catch (Exception ex)
        {
            DashboardLog.Warn($"tracking calibrate: {ex.Message}");
            _session.OnExit(null);
        }
        Step();
    }

    /// <summary>Move onto the monitor that best matches the screen's desktop rect (status feed).</summary>
    private void PlaceOnTarget()
    {
        try
        {
            var all = Screens.All;
            if (_screen is null || all.Count == 0) return;
            var bounds = all.Select(s => new Model.PixelRect(s.Bounds.X, s.Bounds.Y, s.Bounds.Width, s.Bounds.Height)).ToArray();
            int i = MonitorMatch.Pick(bounds, _screen.Desktop.Rect, _screen.Desktop.Scale);
            if (i < 0) { DashboardLog.Warn("calibration: no monitor matches the screen's desktop rect; using the current one"); return; }
            var b = all[i].Bounds;
            Position = new PixelPoint(b.X, b.Y);
            Width = b.Width / all[i].Scaling;
            Height = b.Height / all[i].Scaling;
        }
        catch (Exception ex) { DashboardLog.Warn($"calibration placement: {ex.Message}"); }
    }

    private void OnKey(object? sender, KeyEventArgs e)
    {
        DashboardLog.Info($"calibration: key {e.Key} (phase {_session.Phase}, stable {_session.Stable})");
        e.Handled = true;
        switch (e.Key)
        {
        case Key.Escape:
            if (_session.Phase is CalibPhase.Done or CalibPhase.Failed or CalibPhase.Cancelled) { Close(); return; }
            _session.Escape();
            StopChild();
            Close();
            return;
        case Key.Space: _session.Space(); break;
        case Key.Enter: _session.Enter(); break;
        case Key.Back: _session.Back(); break;
        case Key.R: _session.RetakeFlagged(); break;
        case Key.A: _session.RetakeAll(); break;
        case Key.Left: _session.Nudge(-1, 0); break;
        case Key.Right: _session.Nudge(1, 0); break;
        case Key.Up: _session.Nudge(0, 1); break;
        case Key.Down: _session.Nudge(0, -1); break;
        case Key.OemPlus or Key.Add: _ringLevel = (byte)Math.Min(0xFF, _ringLevel + 0x20); break;
        case Key.OemMinus or Key.Subtract: _ringLevel = (byte)Math.Max(0x20, _ringLevel - 0x20); break;
        default: e.Handled = false; return;
        }
        Step();
    }

    /// <summary>After every input: run what the session asked for, stop the child when it is done, redraw.</summary>
    private void Step()
    {
        if (!_session.WantsStream) StopChild();
        if (!_busy && _session.TakeRequest() is { } req) _ = RunAsync(req);
        Render();
    }

    private async Task RunAsync(CalibRequest req)
    {
        _busy = true;
        try
        {
            if (req.IsSolve)
            {
                // Kept (overwritten by the next solve) so a run can be re-solved and inspected afterwards.
                string path = CalibrationCommands.LastSolveInputPath;
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
                await File.WriteAllTextAsync(path, req.SolveInput ?? "");
                var args = CalibrationCommands.Solve(path, req.Mode ?? CalibrationCommands.Mode);
                DashboardLog.Info($"calibration: displayxr-cli {args}");
                var r = await _cli.RunAsync(args, DashboardContext.ActionTimeout);
                bool parsed = SolveResult.TryParse(r.Json, out var result);
                _session.SolveFinished(r.Started && !r.TimedOut, r.ExitCode, r.Json, r.Summary);
                if (parsed) DashboardLog.Info("calibration: " + CalibrationCommands.Describe(result!));
                if (_session.Phase == CalibPhase.Solving && _session.FallbackNote is { } note)
                    DashboardLog.Warn("calibration: " + note);
                var near = _session.PassDistanceM(CalibPass.Near);
                var far = _session.PassDistanceM(CalibPass.Far);
                if (near is not null && far is not null && _session.Phase != CalibPhase.Solving)
                    DashboardLog.Info(string.Format(CultureInfo.InvariantCulture,
                        "calibration: estimated pass distances near {0:0.000} m, far {1:0.000} m; input kept at {2}", near, far, path));
            }
            else if (req.Args is { } args)
            {
                DashboardLog.Info($"calibration: displayxr-cli {args}");
                var r = await _cli.RunAsync(args, DashboardContext.ActionTimeout);
                bool ok = r.Ok & CalibrationCommands.ParseApply(r.Json, out var report);
                _session.ApplyFinished(ok, report.Length > 0 ? report : r.Report);
                if (ok) _onApplied();
            }
        }
        catch (Exception ex)
        {
            DashboardLog.Error("calibration", ex);
            if (req.IsSolve) _session.SolveFinished(false, -1, "", ex.Message);
            else _session.ApplyFinished(false, ex.Message);
        }
        finally { _busy = false; }
        Step();
    }

    private void StopChild()
    {
        var c = _child;
        _child = null;
        try { c?.Stop(); } catch { }
    }

    private void Render()
    {
        double w = Bounds.Width, h = Bounds.Height;
        if (w <= 0 || h <= 0) return;
        double m = Math.Min(w, h);
        _ring.Margin = new Thickness(m * 0.02);
        _ring.BorderThickness = new Thickness(m * 0.05);
        _ring.CornerRadius = new CornerRadius(m * 0.06);
        _ring.BorderBrush = new SolidColorBrush(Color.FromRgb(_ringLevel, _ringLevel, _ringLevel));

        var g = _session.Geometry;
        var dot = _session.CurrentDot;
        bool showCross = dot is not null && g is not null;
        _crossH.IsVisible = _crossV.IsVisible = _hold.IsVisible = showCross;
        double cy = h / 2;
        if (showCross)
        {
            var (fx, fy) = g!.Fraction(dot!);
            double x = fx * w, y = fy * h;
            cy = y;
            Place(_crossH, x - CrossArm, y - CrossThickness / 2, CrossArm * 2, CrossThickness);
            Place(_crossV, x - CrossThickness / 2, y - CrossArm, CrossThickness, CrossArm * 2);
            // The hold ring brightens with the still run (0.5 s of stable samples).
            byte v = (byte)(0x40 + (int)(0xA0 * _session.StableProgress));
            _hold.Stroke = new SolidColorBrush(_session.Stable ? Color.FromRgb(0x3c, 0xd0, 0x70) : Color.FromRgb(v, v, v));
            Place(_hold, x - _hold.Width / 2, y - _hold.Height / 2, _hold.Width, _hold.Height);
        }
        var foot = _session.LiveFoot;
        _live.IsVisible = foot is not null;
        if (foot is { } f) Place(_live, f.Fx * w - LiveDot / 2, f.Fy * h - LiveDot / 2, LiveDot, LiveDot);

        _prompt.Text = _session.Prompt;
        _status.Text = _session.Status + (_session.Phase == CalibPhase.LiveCheck && _session.LivePoint is { } p
            ? string.Format(CultureInfo.InvariantCulture, "{0}eye midpoint ({1:0.0}, {2:0.0}, {3:0.0}) mm", StatusText.Sep, p.X * 1000, p.Y * 1000, p.Z * 1000)
            : "");
        _error.Text = _session.Phase is CalibPhase.LiveCheck or CalibPhase.Review or CalibPhase.Aligning && _session.Error is { } err ? err : "";
        _error.IsVisible = _error.Text.Length > 0;
        // Keep the text away from the cross: below it when the cross is in the top half, above otherwise.
        bool below = cy < h / 2;
        _textPanel.VerticalAlignment = below ? VerticalAlignment.Bottom : VerticalAlignment.Top;
        _textPanel.Margin = new Thickness(m * 0.1, below ? 0 : m * 0.1, m * 0.1, below ? m * 0.1 : 0);

        bool review = _session.Phase == CalibPhase.Review;
        _review.IsVisible = review;
        if (review) BuildReview();
    }

    private static void Place(Control c, double x, double y, double w, double h)
    {
        c.Width = w;
        c.Height = h;
        Canvas.SetLeft(c, x);
        Canvas.SetTop(c, y);
    }

    private void BuildReview()
    {
        var s = _session.Solve;
        string key = s is null ? "none" + _session.Error : s.Status + s.Mode + s.RmsMm + s.Setting + s.Samples.Count;
        if (key == _reviewKey) return;
        _reviewKey = key;
        var col = new StackPanel { Spacing = 6 };
        void Line(string t, byte grey = 0xC8, double size = 14, bool mono = false)
        {
            var tb = new TextBlock
            {
                Text = t, FontSize = size, TextWrapping = TextWrapping.Wrap,
                Foreground = new SolidColorBrush(Color.FromRgb(grey, grey, grey)),
            };
            if (mono) tb.FontFamily = new FontFamily("Cascadia Mono, Consolas, Menlo, DejaVu Sans Mono, monospace");
            col.Children.Add(tb);
        }
        var ci = CultureInfo.InvariantCulture;
        if (s is null)
        {
            Line("No result", 0xF0, 18);
            Line(_session.Error ?? "The solver gave no result.");
        }
        else if (!s.Ok)
        {
            Line($"Refused: {s.Status}", 0xF0, 18);
            Line(s.Reason);
            Line("Press A to start over (or R to retake the samples).", 0x90);
        }
        else
        {
            Line(string.Format(ci, "RMS {0:0.00} mm · max {1:0.00} mm · iris scale {2:0.000}", s.RmsMm, s.MaxMm, s.IrisScale), 0xF0, 18);
            if (s.HighRms) Line("Warning: RMS above 5 mm. Retake the flagged samples (R) or start over (A).", 0xF0);
            if (_session.FallbackNote is { } note) Line(note, 0xF0);
            if (s.IllConditioned) Line("Warning: the fit is ill-conditioned (the samples barely constrain a parameter). Check near and far in the live check; start over (A) with a bigger distance change if the dot drifts.", 0xF0);
            Line(string.Format(ci, "Camera at ({0:0.0}, {1:0.0}) mm from the screen centre, pitch {2:0.0}°, yaw {3:0.0}°, roll {4:0.0}° (mode {5}; depth from the glass is not observable and assumed 0).",
                               s.Pose!.T.X * 1000, s.Pose.T.Y * 1000, s.PitchDeg, s.YawDeg, s.RollDeg, s.Mode), 0xA0);
            Line(string.Format(ci, "{0} of {1} samples used, {2} distinct targets{3}.", s.UsedCount, s.SampleCount, s.DistinctDots,
                               s.IllConditioned ? "; ill-conditioned" : ""), 0xA0);
            if (_session.PassDistanceM(CalibPass.Near) is { } dn && _session.PassDistanceM(CalibPass.Far) is { } df)
                Line(string.Format(ci, "Estimated head distance: near pass {0:0} cm, far pass {1:0} cm (before the iris scale).", dn * 100, df * 100), 0xA0);
            var flagged = s.Flagged;
            foreach (var r in s.Samples)
            {
                string name = r.Index < CalibGeometry.Dots.Count ? CalibGeometry.Dots[r.Index].Label : $"#{r.Index}";
                string mark = r.Rejected ? "  REJECTED" : flagged.Contains(r.Index) ? "  over 5 mm" : "";
                Line(string.Format(ci, "  {0,-22} residual {1,6:0.00} mm   distance {2:0.000} m (estimated {3:0.000}){4}",
                                   name, r.ResidualMm, r.DistanceM, r.EstimatedDistanceM, mark), r.Rejected || mark.Length > 0 ? (byte)0xF0 : (byte)0xA0, 13, true);
            }
        }
        _review.Child = col;
    }
}
