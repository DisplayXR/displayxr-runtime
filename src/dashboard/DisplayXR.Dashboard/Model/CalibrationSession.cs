// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace DisplayXR.Dashboard.Model;

public enum CalibPhase
{
    /// <summary>The watch child is up; waiting for its hello.</summary>
    Starting,
    /// <summary>A cross is shown; the user aligns and presses Space.</summary>
    Aligning,
    /// <summary>The samples went to <c>calibrate solve</c>.</summary>
    Solving,
    /// <summary>The solve result (or its refusal) is shown.</summary>
    Review,
    /// <summary>The live-check dot follows the eye midpoint through the solved pose; arrows nudge T.</summary>
    LiveCheck,
    /// <summary><c>calibrate apply</c> is running.</summary>
    Applying,
    /// <summary>Committed.</summary>
    Done,
    /// <summary>Cannot continue (no provider, no camera-space samples, the stream ended…).</summary>
    Failed,
    /// <summary>Esc.</summary>
    Cancelled,
}

/// <summary>What the window must run next; it reports back through <see cref="CalibrationSession.SolveFinished"/> / <see cref="CalibrationSession.ApplyFinished"/>.</summary>
public sealed record CalibRequest(bool IsSolve, CliArgs? Args, string? SolveInput);

/// <summary>
/// The calibration window's state machine (design §10 procedure). Pure: no process, no UI, no clock
/// (sample time is the samples' own <c>capture_ns</c>). The window feeds it the watch lines, keys and
/// CLI results, and draws <see cref="Phase"/>, <see cref="CurrentDot"/>, <see cref="Prompt"/> and
/// <see cref="LiveFoot"/>.
/// </summary>
public sealed class CalibrationSession
{
    public const string AlignPrompt =
        "Close your left eye. With your right eye, find the reflection of your closed eye and move your head until the cross sits on it. Press Space (or click).";

    private readonly Screen? _screen;
    private readonly CalibSample?[] _captured = new CalibSample?[CalibGeometry.Dots.Count];
    private readonly List<int> _queue = new();
    private readonly StabilityTracker _stable = new();
    private CalibRequest? _pending;

    public CalibrationSession(string screenKey, Screen? screen)
    {
        ScreenKey = screenKey;
        _screen = screen;
        _queue.AddRange(CalibGeometry.Dots.Select(d => d.Index));
    }

    public string ScreenKey { get; }
    public CalibPhase Phase { get; private set; } = CalibPhase.Starting;
    public CalibHello? Hello { get; private set; }
    public CalibGeometry? Geometry { get; private set; }
    public CalibSample? Latest { get; private set; }

    /// <summary>Space was pressed while the head was moving: capture as soon as it is still.</summary>
    public bool Armed { get; private set; }
    public bool Stable => _stable.Stable;
    public double StableProgress => _stable.Progress;

    /// <summary>The failure (or the last soft problem: a refused solve, a failed apply).</summary>
    public string? Error { get; private set; }

    public SolveResult? Solve { get; private set; }
    /// <summary>The pose being checked / applied: the solved one plus the arrow-key nudge.</summary>
    public CameraPose? Pose { get; private set; }
    public double NudgeXmm { get; private set; }
    public double NudgeYmm { get; private set; }
    public string? ApplyReport { get; private set; }
    public bool StreamEnded { get; private set; }

    /// <summary>The dot the user is aligning on (Aligning only).</summary>
    public CalibDot? CurrentDot => Phase == CalibPhase.Aligning && _queue.Count > 0 ? CalibGeometry.Dots[_queue[0]] : null;

    public int CapturedCount => _captured.Count(c => c is not null);
    public IReadOnlyList<CalibSample?> Captured => _captured;

    /// <summary>The watch child is still needed (the window stops it otherwise).</summary>
    public bool WantsStream => Phase is not (CalibPhase.Done or CalibPhase.Failed or CalibPhase.Cancelled);

    /// <summary>The live-check dot: the predicted foot of the eye midpoint, as a fraction of the screen; null without a lock.</summary>
    public (double Fx, double Fy)? LiveFoot
    {
        get
        {
            if (Phase != CalibPhase.LiveCheck || Pose is null || Geometry is null || Latest is not { Usable: true } s) return null;
            var p = Pose.ToDisplay(s.Camera!.MidPoint, Solve?.IrisScale ?? 1, Solve?.FocalScale ?? 1);
            return Geometry.FromMetres(p.X, p.Y);
        }
    }

    /// <summary>The live-check eye midpoint in display space (metres), for the readout.</summary>
    public Vec3? LivePoint =>
        Phase == CalibPhase.LiveCheck && Pose is not null && Latest is { Usable: true } s
            ? Pose.ToDisplay(s.Camera!.MidPoint, Solve?.IrisScale ?? 1, Solve?.FocalScale ?? 1)
            : null;

    /// <summary>The next CLI run the window owes the session, once (null when none).</summary>
    public CalibRequest? TakeRequest()
    {
        var r = _pending;
        _pending = null;
        return r;
    }

    public string Prompt => Phase switch
    {
        CalibPhase.Starting => "Starting the tracking provider…",
        CalibPhase.Aligning when CurrentDot is { LeanBack: true } =>
            "Lean back, then: " + AlignPrompt,
        CalibPhase.Aligning => AlignPrompt,
        CalibPhase.Solving => "Solving the camera pose…",
        CalibPhase.Review when Solve is { Ok: false } s => $"The solver refused the samples ({s.Status}): {s.Reason}",
        CalibPhase.Review when Solve is { HighRms: true } => "The fit is loose (RMS above 5 mm). Retake the flagged samples, or continue to the live check.",
        CalibPhase.Review => "Fit done. Press Space for the live check.",
        CalibPhase.LiveCheck =>
            "Live check: keep your left eye closed and move your head. The dot should stay on the reflection of your closed eye. Arrow keys nudge it by 1 mm; Enter applies.",
        CalibPhase.Applying => "Saving the calibration…",
        CalibPhase.Done => "Calibration saved. It applies at the provider's next start (the next app launch). Press Esc to close.",
        CalibPhase.Failed => Error ?? "Calibration stopped.",
        CalibPhase.Cancelled => "Cancelled.",
        _ => "",
    };

    /// <summary>The status line under the prompt (capture state, stability, the hint keys).</summary>
    public string Status
    {
        get
        {
            switch (Phase)
            {
            case CalibPhase.Aligning:
                string lockText = Latest is null ? "waiting for a sample" : !Latest.Tracking ? "no face lock" : Latest.Camera is null ? "no camera-space sample" : Stable ? "still" : "hold still";
                return $"Target {CapturedCount + 1} of {CalibGeometry.Dots.Count}: {CurrentDot?.Name}{StatusText.Sep}{lockText}"
                       + (Armed ? $"{StatusText.Sep}captures when still" : "") + $"{StatusText.Sep}Esc cancels";
            case CalibPhase.Review:
                return Solve is { Ok: true } ? "Space: live check · R: retake flagged · A: start over · Esc: cancel"
                                             : "R: retake flagged · A: start over · Esc: cancel";
            case CalibPhase.LiveCheck:
                return string.Format(CultureInfo.InvariantCulture, "Nudge {0:+0;-0;0} mm, {1:+0;-0;0} mm{2}Enter: apply · Backspace: back to the result · Esc: cancel",
                                     NudgeXmm, NudgeYmm, StatusText.Sep)
                       + (Latest is { Usable: false } ? $"{StatusText.Sep}no face lock" : "");
            default:
                return Phase is CalibPhase.Failed or CalibPhase.Done ? "Esc closes." : "Esc cancels.";
            }
        }
    }

    /// <summary>One watch line.</summary>
    public void OnLine(string line)
    {
        if (!WantsStream) return;
        switch (CalibLineParser.Parse(line))
        {
        case CalibHello h:
            Hello = h;
            if (!h.TracksCameraSpace)
            {
                Fail($"The tracking provider '{h.Provider}' reports no camera-space samples, so there is nothing to calibrate.");
                return;
            }
            Geometry = CalibGeometry.Choose(_screen, h.Display);
            if (Geometry is null)
            {
                Fail("The screen's physical size is not known, so the target positions cannot be computed.");
                return;
            }
            if (Phase == CalibPhase.Starting) Phase = CalibPhase.Aligning;
            break;
        case CalibSample s:
            Latest = s;
            _stable.Add(s);
            if (Phase == CalibPhase.Aligning && Armed && Stable) Capture();
            break;
        case CalibError e:
            Fail(ErrorText(e));
            break;
        }
    }

    /// <summary>The watch child ended.</summary>
    public void OnExit(int? code)
    {
        StreamEnded = true;
        if (Phase is CalibPhase.Starting or CalibPhase.Aligning or CalibPhase.LiveCheck)
            Fail(Error ?? (code is null ? "Could not start 'displayxr-cli tracking calibrate'." : $"The tracking stream ended (exit code {code})."));
    }

    public static string ErrorText(CalibError e) => e.Error switch
    {
        "no_provider" => "No viewer-tracking provider is running: " + e.Reason,
        "start_failed" => "The tracking provider failed to start: " + e.Reason,
        "no_samples" => "The tracking provider sent no sample: " + e.Reason,
        _ => $"{e.Error}: {e.Reason}",
    };

    /// <summary>Space: capture (Aligning), go to the live check (Review).</summary>
    public void Space()
    {
        switch (Phase)
        {
        case CalibPhase.Aligning:
            if (Stable && Latest is { Usable: true }) Capture();
            else Armed = true;
            break;
        case CalibPhase.Review when Solve is { Ok: true, Pose: not null }:
            StartLiveCheck();
            break;
        }
    }

    /// <summary>Enter: apply (LiveCheck); like Space otherwise.</summary>
    public void Enter()
    {
        if (Phase == CalibPhase.LiveCheck) RequestApply();
        else if (Phase == CalibPhase.Review) Space();
    }

    /// <summary>Backspace in the live check: back to the result (to retake).</summary>
    public void Back()
    {
        if (Phase == CalibPhase.LiveCheck) Phase = CalibPhase.Review;
    }

    public void Escape()
    {
        if (Phase is CalibPhase.Done or CalibPhase.Failed or CalibPhase.Cancelled) return;
        Phase = CalibPhase.Cancelled;
        _pending = null;
    }

    /// <summary>Arrow keys in the live check: nudge T_x / T_y by whole millimetres.</summary>
    public void Nudge(int dxMm, int dyMm)
    {
        if (Phase != CalibPhase.LiveCheck || Solve?.Pose is not { } solved) return;
        NudgeXmm += dxMm;
        NudgeYmm += dyMm;
        Pose = solved.Nudged(NudgeXmm / 1000.0, NudgeYmm / 1000.0);
    }

    /// <summary>R: retake the flagged samples (all of them when nothing is flagged).</summary>
    public void RetakeFlagged()
    {
        if (Phase != CalibPhase.Review) return;
        var f = Solve?.Flagged ?? Array.Empty<int>();
        Retake(f.Count > 0 ? f : CalibGeometry.Dots.Select(d => d.Index).ToArray());
    }

    /// <summary>A: start over.</summary>
    public void RetakeAll()
    {
        if (Phase == CalibPhase.Review) Retake(CalibGeometry.Dots.Select(d => d.Index).ToArray());
    }

    private void Retake(IReadOnlyList<int> indices)
    {
        if (StreamEnded) { Fail("The tracking stream ended; close and start the calibration again."); return; }
        _queue.Clear();
        foreach (int i in indices.Where(i => i >= 0 && i < _captured.Length).Distinct().OrderBy(i => i))
        {
            _captured[i] = null;
            _queue.Add(i);
        }
        Solve = null;
        Pose = null;
        Error = null;
        Armed = false;
        Phase = _queue.Count > 0 ? CalibPhase.Aligning : CalibPhase.Review;
    }

    private void Capture()
    {
        if (_queue.Count == 0 || Latest is not { Usable: true } s) return;
        _captured[_queue[0]] = s;
        _queue.RemoveAt(0);
        Armed = false;
        _stable.Reset(); // the next target needs its own still run
        if (_queue.Count == 0) RequestSolve();
    }

    private void RequestSolve()
    {
        Phase = CalibPhase.Solving;
        _pending = new CalibRequest(true, null, BuildSolveInput());
    }

    /// <summary>The solve input for the current captures (every slot is filled when this runs).</summary>
    public string BuildSolveInput()
    {
        var g = Geometry!;
        var pairs = CalibGeometry.Dots.Where(d => _captured[d.Index] is not null).Select(d =>
        {
            var (x, y) = g.Metres(d);
            return (x, y, _captured[d.Index]!);
        });
        string camera = Hello?.CameraKey is { Length: > 0 } k ? k : _captured.FirstOrDefault(c => c is not null)?.Camera?.Key ?? "";
        return CalibrationCommands.SolveInput(ScreenKey, camera, pairs);
    }

    /// <summary>The solve run finished: its exit code and stdout's JSON (exit 0 ok, 2 refused, 1 bad input).</summary>
    public void SolveFinished(bool started, int exitCode, string json, string summary)
    {
        if (Phase != CalibPhase.Solving) return;
        if (started && SolveResult.TryParse(json, out var r))
        {
            Solve = r;
            Error = r!.Ok ? null : r.Reason;
            NudgeXmm = NudgeYmm = 0;
            Pose = r.Pose;
            Phase = CalibPhase.Review;
            return;
        }
        Solve = null;
        Error = $"'displayxr-cli tracking calibrate solve' gave no result: {summary}";
        Phase = CalibPhase.Review;
    }

    private void StartLiveCheck()
    {
        if (StreamEnded) { Fail("The tracking stream ended; close and start the calibration again."); return; }
        Pose = Solve!.Pose!.Nudged(NudgeXmm / 1000.0, NudgeYmm / 1000.0);
        Phase = CalibPhase.LiveCheck;
    }

    private void RequestApply()
    {
        if (Pose is null || Solve is null) return;
        string camera = Solve.CameraKey is { Length: > 0 } c ? c : Hello?.CameraKey ?? "";
        Phase = CalibPhase.Applying;
        ApplyReport = null;
        _pending = new CalibRequest(false, CalibrationCommands.Apply(ScreenKey, camera, Pose, Hello?.Provider, Solve.IrisScale, Solve.FocalScale,
                                                                       Solve.RmsMm, Solve.UsedCount, Solve.Mode), null);
    }

    /// <summary>The apply run finished.</summary>
    public void ApplyFinished(bool ok, string report)
    {
        if (Phase != CalibPhase.Applying) return;
        ApplyReport = report;
        if (ok) { Phase = CalibPhase.Done; Error = null; }
        else { Error = "Could not save the calibration: " + report; Phase = CalibPhase.LiveCheck; }
    }

    private void Fail(string why)
    {
        Error = why;
        Phase = CalibPhase.Failed;
        _pending = null;
    }
}
