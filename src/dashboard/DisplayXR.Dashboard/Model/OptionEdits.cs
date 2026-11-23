// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;

namespace DisplayXR.Dashboard.Model;

/// <summary>
/// The text boxes of the Options block: what is typed and not yet applied, and which box is being
/// edited. While a box is being edited the page defers its rebuilds (a rebuild would take the text from
/// under the user's fingers); a SUBMITTED edit (Enter, Apply) ends the edit at once, so the write's
/// result and the CLI's reply show without waiting for the focus to leave. Keys are
/// <c>&lt;driver id&gt;/&lt;setting key&gt;</c>. Pure, so the transitions are testable.
/// </summary>
public sealed class OptionEdits
{
    private readonly Dictionary<string, string> _drafts = new();

    /// <summary>The box being edited (it has the focus and nothing has been submitted from it since).</summary>
    public string? Editing { get; private set; }

    public bool IsEditing => Editing is not null;

    /// <summary>The typed text not yet applied, or null.</summary>
    public string? Draft(string key) => _drafts.TryGetValue(key, out var d) ? d : null;

    /// <summary>The box got the focus: an edit starts.</summary>
    public void Focus(string key) => Editing = key;

    /// <summary>The text changed: kept across rebuilds until it is applied.</summary>
    public void Type(string key, string text)
    {
        _drafts[key] = text;
        Editing = key;
    }

    /// <summary>The box lost the focus: the edit ends; the draft stays (it was not applied).</summary>
    public void Blur(string key)
    {
        if (Editing == key) Editing = null;
    }

    /// <summary>Enter or Apply with a value that will be written: the edit ends and the draft goes (the re-read shows the value).</summary>
    public void Submit(string key)
    {
        _drafts.Remove(key);
        if (Editing == key) Editing = null;
    }

    /// <summary>Enter or Apply with a value that was refused: the edit ends (the message must show), the draft stays to be corrected.</summary>
    public void Refuse(string key)
    {
        if (Editing == key) Editing = null;
    }

    /// <summary>The page rebuilt: every box was replaced, and a replaced box may never say it lost the focus.</summary>
    public void Rebuilt() => Editing = null;
}
