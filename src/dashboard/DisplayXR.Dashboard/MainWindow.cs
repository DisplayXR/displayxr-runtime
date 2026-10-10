// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Pages;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard;

/// <summary>
/// The shell: a left navigation rail with badges, a header (page title and
/// caption, the feed's source pill, Refresh, Copy diagnostics), the feed
/// banners, and the scrolling page host. Built in code, like its pages.
/// </summary>
public sealed class MainWindow : Window
{
    private const double RailWidth = 236;
    private const double DesignMinWidth = 1100, DesignMinHeight = 700;

    private readonly StatusFeed _feed;
    private readonly DashboardContext _ctx;
    private readonly List<Page> _pages;
    private Page? _current;

    private readonly Dictionary<string, RadioButton> _nav = new();
    private readonly Dictionary<string, Border> _navBadges = new();
    private readonly TextBlock _title = U.Text("", "h1");
    private readonly TextBlock _caption = U.Text("", "sub");
    private readonly ContentControl _pageActions = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly Border _pill = new();
    private readonly Avalonia.Controls.Shapes.Ellipse _pillDot = U.Dot(Level.Plain, 8);
    private readonly TextBlock _pillText = U.Text("");
    private readonly TextBlock _readNote = U.Text("", "meta");
    private readonly TextBlock _toast = U.Text("", "meta");
    private readonly StackPanel _banners = U.VStack(10);
    private readonly ScrollViewer _scroll;
    private readonly ContentControl _host = new();
    private readonly TextBlock _railFoot = U.Text("", "meta");
    private string _bannerKey = "";
    private IDisposable? _toastTimer;
    private bool _shutdown;

    public MainWindow(string startPage)
    {
        Title = "DisplayXR Dashboard";
        Width = 1280;
        Height = 820;
        MinWidth = 800;
        MinHeight = 560;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Background = Tokens.Brush("WindowBackgroundBrush");
        ExtendClientAreaToDecorationsHint = true;
        ExtendClientAreaChromeHints = ExtendClientAreaChromeHints.PreferSystemChrome;
        ExtendClientAreaTitleBarHeightHint = 36;
        try { Icon = new WindowIcon(AssetLoader.Open(new Uri("avares://displayxr-dashboard/Assets/displayxr.ico"))); }
        catch (Exception ex) { DashboardLog.Warn($"icon: {ex.Message}"); }

        var cli = new CliProcessSource();
        DashboardLog.Info($"CLI: {cli.CliPath} (exists={cli.CliExists})");
        IProcessSource feedSource = cli;
        if (Program.Fixture is { } fx)
        {
            try { feedSource = new FixtureProcessSource(fx, cli); }
            catch (Exception ex) { DashboardLog.Error("fixture", ex); }
        }
        _feed = new StatusFeed(feedSource, new DispatcherScheduler());
        _ctx = new DashboardContext(_feed, cli, feedSource)
        {
            Navigate = Navigate,
            CopyText = CopyAsync,
            Toast = ShowToast,
        };

        _pages = new List<Page> { new HomePage(), new DisplaysPage(), new WindowsPage(), new ComponentsPage(), new PerformancePage(), new DeveloperPage() };
        foreach (var p in _pages)
        {
            p.Bind(_ctx);
            p.Rebuilt += () => { if (ReferenceEquals(p, _current)) RefreshHeader(); };
        }

        _scroll = new ScrollViewer
        {
            Content = _host,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        _host.Margin = new Thickness(36, 6, 36, 36);
        Content = BuildShell();

        _feed.Changed += OnStateChanged;
        _ctx.Changed += OnStateChanged;
        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty) _feed.SetPaused(WindowState == WindowState.Minimized);
            else if (e.Property == IsVisibleProperty) _feed.SetPaused(!IsVisible || WindowState == WindowState.Minimized);
        };
        Opened += (_, _) =>
        {
            FitToScreen();
            Navigate(_pages.Any(p => p.Id == startPage) ? startPage : "home");
        };
        // Moving onto a monitor with another scale: Windows rescales the window by
        // the DPI ratio, which can push it past that monitor's working area (a
        // 250 % -> 300 % move does). Fit once the move has settled.
        ScalingChanged += (_, _) => DispatcherTimer.RunOnce(FitToScreen, TimeSpan.FromMilliseconds(250));
        Closing += (_, _) => ShutdownFeed();
    }

    // ── Shell ───────────────────────────────────────────────────────────────

    private Control BuildShell()
    {
        var root = new Grid { ColumnDefinitions = new ColumnDefinitions($"{RailWidth},*") };
        root.Children.Add(BuildRail());
        var main = BuildMain();
        Grid.SetColumn(main, 1);
        root.Children.Add(main);
        return root;
    }

    private Control BuildRail()
    {
        var mark = new Image { Width = 34, Height = 34, VerticalAlignment = VerticalAlignment.Center };
        try { mark.Source = new Bitmap(AssetLoader.Open(new Uri("avares://displayxr-dashboard/Assets/displayxr.ico"))); }
        catch (Exception ex) { DashboardLog.Warn($"rail mark: {ex.Message}"); }
        var word = U.VStack(-2, U.Text("DisplayXR"), U.Text("Dashboard"));
        var w1 = (TextBlock)word.Children[0];
        w1.FontSize = 18; w1.FontWeight = FontWeight.Bold;
        var w2 = (TextBlock)word.Children[1];
        w2.FontSize = 13.5; w2.Foreground = Tokens.AccentText;
        word.VerticalAlignment = VerticalAlignment.Center;
        var brand = U.HStack(12, mark, word);
        brand.Margin = new Thickness(26, 46, 16, 30);
        brand.Cursor = new Cursor(StandardCursorType.Hand);
        brand.PointerPressed += (_, e) =>
        {
            if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) { e.Handled = true; Navigate("home"); }
        };
        ToolTip.SetTip(brand, "Home");

        var nav = U.VStack(2);
        foreach (var p in _pages) nav.Children.Add(NavButton(p));

        _railFoot.Margin = new Thickness(26, 0, 16, 22);
        _railFoot.TextWrapping = TextWrapping.Wrap;
        _railFoot.Text = $"Dashboard {typeof(MainWindow).Assembly.GetName().Version?.ToString(3)}";

        var dock = new DockPanel();
        DockPanel.SetDock(brand, Dock.Top);
        DockPanel.SetDock(_railFoot, Dock.Bottom);
        dock.Children.Add(brand);
        dock.Children.Add(_railFoot);
        dock.Children.Add(nav);

        var rail = new Border
        {
            Background = Tokens.Brush("RailBackgroundBrush"),
            BorderBrush = Tokens.Line,
            BorderThickness = new Thickness(0, 0, 1, 0),
            Child = dock,
        };
        // The rail is a grab handle, as the vendor dashboard's is.
        rail.PointerPressed += (_, e) =>
        {
            if (!e.Handled && e.GetCurrentPoint(this).Properties.IsLeftButtonPressed && e.Source is Border or DockPanel or StackPanel)
                BeginMoveDrag(e);
        };
        return rail;
    }

    private RadioButton NavButton(Page p)
    {
        var rb = new RadioButton { GroupName = "nav", Tag = p.Id };
        rb.Classes.Add("nav");
        Avalonia.Automation.AutomationProperties.SetName(rb, p.Title);
        var label = new TextBlock { Text = p.Title, VerticalAlignment = VerticalAlignment.Center };
        label.Bind(TextBlock.ForegroundProperty, rb.GetObservable(ForegroundProperty));
        label.Bind(TextBlock.FontSizeProperty, rb.GetObservable(FontSizeProperty));
        label.Bind(TextBlock.FontWeightProperty, rb.GetObservable(FontWeightProperty));
        var badgeText = U.Text("");
        badgeText.FontSize = 11;
        badgeText.FontWeight = FontWeight.Bold;
        badgeText.HorizontalAlignment = HorizontalAlignment.Center;
        badgeText.VerticalAlignment = VerticalAlignment.Center;
        var badge = new Border
        {
            Child = badgeText, MinWidth = 22, Height = 20, CornerRadius = new CornerRadius(10), Padding = new Thickness(6, 0),
            VerticalAlignment = VerticalAlignment.Center, IsVisible = false,
        };
        _navBadges[p.Id] = badge;
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        row.Children.Add(label);
        Grid.SetColumn(badge, 1);
        row.Children.Add(badge);
        rb.Content = row;
        rb.IsCheckedChanged += (_, _) => { if (rb.IsChecked == true) Navigate(p.Id); };
        _nav[p.Id] = rb;
        return rb;
    }

    private Control BuildMain()
    {
        var refresh = U.Button("Refresh", Refresh, "outline", "sm");
        ToolTip.SetTip(refresh, "Restart the status feed and re-read the runtime's facts");
        var copy = U.Button("Copy diagnostics", () => _ = CopyDiagnostics(), "outline", "sm");
        ToolTip.SetTip(copy, "Copy the full status snapshot (JSON) to the clipboard");

        _pill.Classes.Add("field");
        _pill.Padding = new Thickness(11, 5);
        _pill.CornerRadius = new CornerRadius(14);
        _pillText.FontSize = 12;
        _pillText.Foreground = Tokens.Soft;
        _pillText.VerticalAlignment = VerticalAlignment.Center;
        _pill.Child = U.HStack(8, _pillDot, _pillText);
        _pill.VerticalAlignment = VerticalAlignment.Center;

        var globalActions = U.HStack(10, _pill, refresh, copy);
        globalActions.VerticalAlignment = VerticalAlignment.Center;
        _title.TextTrimming = TextTrimming.CharacterEllipsis;
        _title.VerticalAlignment = VerticalAlignment.Center;
        var row0 = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        row0.Children.Add(_title);
        Grid.SetColumn(globalActions, 1);
        row0.Children.Add(globalActions);

        _caption.Margin = new Thickness(0, 0, 20, 0);
        _caption.VerticalAlignment = VerticalAlignment.Center;
        var notes = U.HStack(14, _readNote, _toast);
        notes.VerticalAlignment = VerticalAlignment.Center;
        notes.HorizontalAlignment = HorizontalAlignment.Right;
        _toast.Foreground = Tokens.For(Level.Ok);
        var right = U.HStack(14, notes, _pageActions);
        right.VerticalAlignment = VerticalAlignment.Center;
        var row1 = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(0, 6, 0, 0), MinHeight = 30 };
        row1.Children.Add(_caption);
        Grid.SetColumn(right, 1);
        row1.Children.Add(right);

        // Top padding clears the system caption buttons drawn over the extended client area.
        var header = U.VStack(0, row0, row1);
        header.Margin = new Thickness(36, 50, 36, 14);

        _banners.Margin = new Thickness(36, 0, 36, 0);

        var main = new Grid { RowDefinitions = new RowDefinitions("Auto,Auto,*") };
        main.Children.Add(header);
        Grid.SetRow(_banners, 1);
        main.Children.Add(_banners);
        var scrollWrap = new Border { Child = _scroll, Margin = new Thickness(0, 10, 0, 0) };
        Grid.SetRow(scrollWrap, 2);
        main.Children.Add(scrollWrap);
        return main;
    }

    // ── Navigation ──────────────────────────────────────────────────────────

    public void Navigate(string id)
    {
        var page = _pages.FirstOrDefault(p => p.Id == id);
        if (page is null || _shutdown) return;
        if (_nav.TryGetValue(id, out var rb) && rb.IsChecked != true)
        {
            rb.IsChecked = true; // re-enters through IsCheckedChanged
            return;
        }
        if (ReferenceEquals(page, _current)) return;
        var old = _current;
        _current = page;
        if (old is not null)
        {
            try { old.OnHidden(); } catch (Exception ex) { DashboardLog.Error("OnHidden", ex); }
        }
        // Hold the new page's feed before releasing the old one's, so moving
        // between two status pages keeps the one child running.
        if (page.HoldsFeed) _feed.Hold();
        if (old is { HoldsFeed: true }) _feed.Release();
        _host.Content = page;
        try { page.OnShown(); } catch (Exception ex) { DashboardLog.Error("OnShown", ex); }
        page.Update(force: true);
        _scroll.Offset = new Vector(0, 0);
        RefreshHeader();
        RefreshChrome();
    }

    // ── State ───────────────────────────────────────────────────────────────

    private void OnStateChanged()
    {
        if (_shutdown) return;
        try
        {
            _current?.Update();
            RefreshHeader();
            RefreshChrome();
        }
        catch (Exception ex)
        {
            DashboardLog.Error("state change", ex);
        }
    }

    private void RefreshHeader()
    {
        if (_current is null) return;
        _title.Text = _current.Title;
        _caption.Text = _current.Caption;
        var actions = _current.HeaderActions;
        if (!ReferenceEquals(_pageActions.Content, actions)) _pageActions.Content = actions;
    }

    private void RefreshChrome()
    {
        var s = _feed.Snapshot;

        // Source pill.
        (Level level, string text, string tip) pill = _feed.Mode switch
        {
            FeedMode.Idle => (Level.Plain, "feed paused", "No status page is on screen, so no status child runs."),
            _ when s is null => (Level.Info, "connecting…", "Waiting for the first snapshot from displayxr-cli."),
            FeedMode.Polling => (Level.Warn, "headless · every 30 s", _feed.FallbackReason ?? "The live feed is unavailable."),
            _ when s.IsService => (Level.Ok, StatusText.SourceLine(s), "Live: one long-lived 'displayxr-cli status --watch --json' child over the service's diagnostics connection."),
            _ => (Level.Warn, "source: headless", "No service reached: a snapshot built by the CLI itself."),
        };
        _pill.IsVisible = _current?.HoldsFeed == true;
        _pillDot.Fill = Tokens.For(pill.level);
        _pillText.Text = pill.text;
        ToolTip.SetTip(_pill, pill.tip);

        _readNote.Text = _feed.LastReadFailed && _feed.Mode != FeedMode.Idle ? "last read failed, retrying" : "";
        ToolTip.SetTip(_readNote, _feed.LastReadError);
        _readNote.Foreground = Tokens.For(Level.Warn);

        // Banners: only status pages show the feed's state.
        bool statusPage = _current?.HoldsFeed == true;
        string elevated = _ctx.ElevationNote;
        string key = $"{statusPage}|{_feed.Mode}|{s?.Source}|{_feed.FallbackReason}|{s is null}";
        if (key != _bannerKey)
        {
            _bannerKey = key;
            _banners.Children.Clear();
            if (statusPage && _feed.Mode == FeedMode.Polling)
                _banners.Children.Add(U.Notice(Level.Warn, "Live feed unavailable — showing a headless snapshot",
                    $"{_feed.FallbackReason ?? "status --watch would not stay up"}. The dashboard re-reads 'displayxr-cli status --json' every 30 s and returns to the live feed when the service answers. {elevated}".Trim()));
            else if (statusPage && s is { IsService: false })
                _banners.Children.Add(U.Notice(Level.Info, "No service reached — headless snapshot",
                    $"This is what a process starting now would get: live rows (windows, tracking, bound display processors) are absent. The dashboard keeps trying to reach the service. {elevated}".Trim()));
            _banners.Margin = _banners.Children.Count > 0 ? new Thickness(36, 0, 36, 4) : new Thickness(36, 0, 36, 0);
        }

        // Nav badges (warn / critical only on Displays; a client count on Windows; a dot on Performance).
        SetBadge("displays", StatusText.DisplaysBadge(s));
        SetBadge("windows", StatusText.WindowsBadge(s));
        SetBadge("performance", _ctx.PerfNonDefault ? new NavBadge(_ctx.Perf!.Levers.Count(l => l.IsSet), Level.Warn) : null);
        SetBadge("home", _ctx.PreferredPlugin is not null ? new NavBadge(1, Level.Warn) : null);

        string rt = s?.Runtime.Version is { Length: > 0 } v ? $"Runtime {v}\n" : "";
        _railFoot.Text = $"{rt}Dashboard {typeof(MainWindow).Assembly.GetName().Version?.ToString(3)}";
    }

    private void SetBadge(string id, NavBadge? badge)
    {
        if (!_navBadges.TryGetValue(id, out var b)) return;
        if (badge is null)
        {
            b.IsVisible = false;
            return;
        }
        b.IsVisible = true;
        var t = (TextBlock)b.Child!;
        t.Text = badge.Count > 99 ? "99+" : badge.Count.ToString(System.Globalization.CultureInfo.InvariantCulture);
        if (badge.Level == Level.Plain)
        {
            b.Background = Tokens.Line;
            t.Foreground = Tokens.Soft;
        }
        else
        {
            b.Background = Tokens.Alpha(Tokens.ColorKey(badge.Level), 0.22);
            t.Foreground = Tokens.For(badge.Level);
        }
        ToolTip.SetTip(b, id switch
        {
            "displays" => $"{badge.Count} warning{(badge.Count == 1 ? "" : "s")} on the screens",
            "windows" => $"{badge.Count} client app{(badge.Count == 1 ? "" : "s")}",
            "performance" => "Non-default performance settings are in force",
            _ => "A display-processor override is active",
        });
    }

    // ── Actions ─────────────────────────────────────────────────────────────

    private void Refresh()
    {
        DashboardLog.Info($"Refresh on {_current?.Id}");
        _feed.Refresh();
        switch (_current?.Id)
        {
            case "home":
                _ = _ctx.LoadInfoAsync(force: true);
                _ = _ctx.LoadDpAsync(force: true);
                break;
            case "components":
                _ = _ctx.LoadInfoAsync(force: true);
                break;
            case "performance":
                _ = _ctx.LoadPerfAsync(force: true);
                break;
            case "developer":
            case "displays":
                _ = _ctx.LoadDpAsync(force: true);
                break;
        }
        ShowToast("Refreshed");
    }

    private async Task CopyDiagnostics()
    {
        var s = _feed.Snapshot;
        string text;
        if (s is null)
        {
            text = "{ \"note\": \"no status snapshot yet\" }";
        }
        else
        {
            try
            {
                using var doc = JsonDocument.Parse(s.RawJson);
                using var ms = new MemoryStream();
                using (var w = new Utf8JsonWriter(ms, new JsonWriterOptions { Indented = true }))
                    doc.WriteTo(w);
                text = Encoding.UTF8.GetString(ms.ToArray());
            }
            catch (Exception)
            {
                text = s.RawJson;
            }
        }
        await CopyAsync(text);
        ShowToast(s is null ? "No snapshot yet" : "Snapshot JSON copied");
    }

    private async Task CopyAsync(string text)
    {
        try
        {
            if (Clipboard is { } cb) await cb.SetTextAsync(text);
        }
        catch (Exception ex)
        {
            DashboardLog.Error("clipboard", ex);
            ShowToast("Could not reach the clipboard");
        }
    }

    private void ShowToast(string text)
    {
        _toast.Text = text;
        _toastTimer?.Dispose();
        _toastTimer = DispatcherTimer.RunOnce(() => _toast.Text = "", TimeSpan.FromSeconds(2.5));
    }

    // ── Window ──────────────────────────────────────────────────────────────

    /// <summary>
    /// 1100 × 700 is the design minimum, but a 3840 × 2160 monitor at 300 %
    /// has a 1280 × ~680 working area: the minimum is clamped to the screen
    /// the window is on, and the window is fitted inside it.
    /// </summary>
    private void FitToScreen()
    {
        try
        {
            var screen = Screens.ScreenFromWindow(this) ?? Screens.Primary;
            if (screen is null) return;
            if (WindowState != WindowState.Normal) return; // maximised / minimised: the OS owns the size
            double scale = screen.Scaling > 0 ? screen.Scaling : 1;
            double workW = screen.WorkingArea.Width / scale, workH = screen.WorkingArea.Height / scale;
            MinWidth = Math.Min(DesignMinWidth, Math.Max(640, workW - 24));
            MinHeight = Math.Min(DesignMinHeight, Math.Max(480, workH - 24));
            if (Width > workW - 24) Width = Math.Max(MinWidth, workW - 24);
            if (Height > workH - 24) Height = Math.Max(MinHeight, workH - 24);
            // Pull the window back inside the working area if it hangs off it.
            var wa = screen.WorkingArea;
            int w = (int)Math.Round(Width * scale), h = (int)Math.Round(Height * scale);
            int x = Math.Clamp(Position.X, wa.X, Math.Max(wa.X, wa.Right - w));
            int y = Math.Clamp(Position.Y, wa.Y, Math.Max(wa.Y, wa.Bottom - h));
            if (x != Position.X || y != Position.Y) Position = new PixelPoint(x, y);
        }
        catch (Exception ex)
        {
            DashboardLog.Warn($"fit to screen: {ex.Message}");
        }
    }

    public void ShutdownFeed()
    {
        if (_shutdown) return;
        _shutdown = true;
        _feed.Changed -= OnStateChanged;
        _ctx.Changed -= OnStateChanged;
        _feed.Dispose();
        DashboardLog.Info("feed stopped");
    }
}
