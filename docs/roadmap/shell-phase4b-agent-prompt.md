# Shell Phase 4B: Agent Prompt — Input Forwarding & Window Adoption

Use this prompt to start a new Claude Code session for implementing Phase 4B on branch `feature/shell-phase4-ci`.

---

## Prompt

```
I'm working on the DisplayXR shell — a spatial window manager for 3D displays. We're implementing Phase 4B: Input Forwarding & Window Adoption. This follows Phase 4A which added 2D window capture.

## Context

Read these docs first (in order):
1. `CLAUDE.md` — project overview, build commands, architecture
2. `docs/roadmap/shell-phase4-plan.md` — full Phase 4 design
3. `docs/roadmap/shell-phase4a-status.md` — what Phase 4A built (capture pipeline, 2D mode switch, resize sync)
4. `docs/roadmap/shell-runtime-contract.md` — IPC protocol boundary

## Branch

You are on branch `feature/shell-phase4-ci`. All work goes here. Commits must reference #119 (Phase 4 issue) or #43 (shell tracking issue).

## What Phase 4B Needs

### Priority 1: Input Forwarding to Captured 2D Windows

The critical unsolved problem from Phase 4A. PostMessage(WM_KEYDOWN/WM_CHAR) works for classic Win32 apps and DisplayXR IPC apps, but NOT for modern WinUI/XAML apps (Notepad, Paint, Windows Terminal, Chrome, etc.). These apps use a different input pipeline that requires OS-level keyboard focus.

**The constraint:** The captured HWND must stay hidden/background. The shell's compositor window on the 3D display is the only visible surface. Users interact by clicking/typing on the 3D display.

**Approaches to investigate (in order of preference):**

1. **UI Automation (IUIAutomation)** — The Windows accessibility API can programmatically interact with any app:
   - `IUIAutomationElement::FindFirst` to locate text controls
   - `IUIAutomationValuePattern::SetValue` for setting text
   - `IUIAutomationInvokePattern::Invoke` for clicking buttons
   - `IUIAutomationTextPattern` for cursor positioning and text editing
   - Pro: works with all apps (WinUI, Win32, UWP). Con: complex, may not support all input types (e.g., mouse drag in canvas apps like Paint)

2. **Off-screen foreground** — Move captured HWND off-screen (-32000,-32000), then SetForegroundWindow + SendInput:
   - Window is invisible on primary monitor but receives foreground input
   - Capture API still captures content regardless of position
   - Pro: works for all input types. Con: taskbar shows active window, edge cases with multi-monitor

3. **Input injection via helper process** — A small helper process that runs with the captured window's input context:
   - Helper attaches to target thread and injects input
   - Pro: clean separation. Con: extra process, IPC overhead

4. **Kernel-level input driver** — Custom input device driver (like RDP uses):
   - Pro: most robust. Con: requires driver signing, complex

**Key code to understand:**

- `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` — WndProc keyboard/mouse handling (lines ~363-510). Current forwarding uses PostMessage. The `input_forward_is_capture` flag distinguishes capture clients.
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — Click synthesis (lines ~4964-4996). Converts spatial hit coordinates to HWND client coordinates and PostMessages WM_LBUTTONDOWN.
- `src/xrt/compositor/d3d11/comp_d3d11_window.h` — `comp_d3d11_window_set_input_forward()` signature includes `is_capture` flag.

**What already works:**
- Mouse coordinate scaling from shell virtual rect to actual HWND client area
- Click-to-focus changes the focused_slot and calls update_input_forward
- WM_CHAR forwarding (works for Win32 apps, not WinUI)
- Shell-reserved keys (TAB, DELETE, ESC) are intercepted before forwarding

**Test procedure:**
```bash
# Open Notepad, get HWND:
powershell -Command "(Get-Process notepad).MainWindowHandle"
# Launch:
_package\bin\displayxr-shell.exe --capture-hwnd <hwnd> test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
# Test: click on Notepad text area in shell → type → text should appear
```

### Priority 2: Window Enumeration & Auto-Adoption (from Phase 4 plan)

Once input forwarding works, implement automatic window capture:

**4B.1: Window enumeration**
- `EnumWindows` + filter for visible, top-level, non-system windows
- Skip: taskbar, desktop, system tray, own shell window, tooltips, popups
- See filtering heuristics in `docs/roadmap/shell-phase4-plan.md` line ~112

**4B.2: App type classification**
- Cross-reference enumerated HWNDs with `system_get_clients` (IPC-connected apps)
- HWNDs matching IPC clients → already have compositor slots ("3D")
- Others → need capture ("2D")

**4B.3: Capture startup**
- For each "2D" window: call `ipc_call_shell_add_capture_client(hwnd)`
- Assign spatial poses (grid/expose layout)

**4B.4: Dynamic window tracking**
- Poll `EnumWindows` every 500ms, diff against known set
- New windows → auto-adopt
- Closed windows → remove capture client

### IPC additions needed:

Add to `src/xrt/ipc/shared/proto.json`:
- `shell_enumerate_windows` — returns list of adoptable HWNDs (server-side enumeration)
- Or do enumeration shell-side and use existing `shell_add_capture_client` per window

### What NOT to change:

- Don't modify the existing IPC client compositor path
- Don't change the display processor / weaver
- Don't break single-app standalone mode
- Don't break the working capture pipeline from Phase 4A

### Build and test:

```bash
# Build on Windows
scripts\build_windows.bat build

# Test: auto-capture all windows
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
# All visible desktop windows should appear as 2D panels alongside the cube app

# Test: input forwarding
# Click on a captured window → type → text should appear in captured content
```

### Commit style:

Each meaningful piece of work gets its own commit with issue ref:
- `Shell 4B: UI Automation input injection for captured windows (#119)`
- `Shell 4B: auto-enumerate and adopt desktop windows (#119)`
- etc.

Use `/ci-monitor` after each significant commit to verify the build passes.
```

---

## Notes for the developer

- The input forwarding problem is fundamentally about Windows not allowing cross-process input injection without foreground focus. Research the `IUIAutomation` COM API (documented on Microsoft Learn) as the primary approach.
- For UI Automation: `CoCreateInstance(CLSID_CUIAutomation, IUIAutomation)` → `ElementFromHandle(hwnd)` → find text control → use `IUIAutomationValuePattern` or `IUIAutomationTextPattern`.
- For the off-screen foreground approach: test whether `Windows.Graphics.Capture` still captures content when the window is at position (-32000, -32000).
- The window enumeration is straightforward Win32 (`EnumWindows`). The interesting part is classifying windows as 3D (IPC client) vs 2D (needs capture) and handling dynamic window creation/destruction.
- The multi-compositor already supports up to 8 clients (`D3D11_MULTI_MAX_CLIENTS`). This limits the number of captured windows. Consider increasing if needed.
- Phase 4A's `is_capture` flag on `comp_d3d11_window_set_input_forward` is the integration point — extend it with the chosen input injection method.
