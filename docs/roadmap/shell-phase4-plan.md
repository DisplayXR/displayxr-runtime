# Shell Phase 4: Spatial Companion

## Prerequisites

Phase 3 is complete on branch `feature/shell-phase3-ci` (merged to main). All features working:
- Z-depth positioning with per-eye parallax (Shift+Scroll, [/] keys)
- Perspective-correct rotated windows (RMB drag on title bar)
- 3D layout presets: Theater (Ctrl+3), Stack (Ctrl+4), Carousel (Ctrl+5)
- D3D11 multithread protection (stable with 4+ apps)
- Inter-app launch delay reduced to 100ms

Phase 3B (cross-API shell support) is in progress on `feature/shell-phase3b-ci` — GL/VK/D3D12 apps in shell. Phase 4 can begin in parallel since it's orthogonal (new texture source, not new API import).

See `shell-phase3-status.md` and `shell-phase3b-status.md` for full details.

### Known Issues Carried Forward

- **Apps don't survive shell exit** (Phase 1A) — ESC dismisses shell, apps become invisible. Addressed in Phase 4D.

## Vision

The shell becomes a **spatial companion** — a hotkey-summoned overlay that instantly captures all open windows into a 3D workspace. Regular Windows apps appear as 2D panels with spatial parallax. DisplayXR apps render in full 3D. The user works in spatial mode, then exits back to the normal Windows desktop seamlessly.

The key UX flow:

```
User working normally on Windows desktop
  │
  ├─ Presses Ctrl+Space (or configured hotkey)
  │
  ├─ Shell activates, captures all visible windows
  │   ├─ DisplayXR/OpenXR apps → 3D windows (stereo, full depth)
  │   └─ Regular apps (Notepad, Chrome, etc.) → 2D panels (flat texture, spatial parallax)
  │
  ├─ User interacts in spatial mode
  │   ├─ Click a window → snaps to Z=0 (display plane), grows to working size
  │   ├─ Focus on 2D app → render mode switches to 2D (no interlacing)
  │   ├─ Focus on 3D app → render mode switches to 3D (stereo + weave)
  │   ├─ No focus (overview/carousel) → default 3D mode
  │   └─ Can launch new apps from built-in launcher
  │
  ├─ Presses hotkey or ESC to exit
  │   ├─ All captures stop
  │   ├─ All windows restored to original positions
  │   ├─ OpenXR apps hot-switch back to standalone compositing
  │   └─ Normal Windows desktop resumes
  │
  └─ Shell stays resident in system tray, waiting for next summon
```

## Phase 4A: 2D Window Capture Compositor

**Goal:** Capture any Windows HWND as a D3D11 texture and feed it into the multi-compositor as a "virtual client" — a flat textured quad with no OpenXR session.

| Task | Size | Description |
|------|------|-------------|
| 4A.1 | L | **Windows.Graphics.Capture integration** — Use `GraphicsCaptureItem` + `Direct3D11CaptureFramePool` (WinRT, Win10 1903+) to capture individual HWNDs. Each captured window gets its own capture session delivering `ID3D11Texture2D` frames. |
| 4A.2 | M | **Virtual client slot** — Extend multi-compositor slot model to support "capture clients" alongside IPC clients. A capture client has: a shared texture (from capture API), window dimensions (from HWND rect), an app name (from HWND title), but no OpenXR session, no swapchain, no IPC connection. |
| 4A.3 | M | **Mono texture handling** — Capture produces a single texture (not L/R stereo). Multi-compositor renders the same texture for both left and right eye views. Level 2 Kooima still applies (spatial parallax from window position), but the content itself is flat. |
| 4A.4 | S | **Aspect ratio preservation** — Captured windows have arbitrary aspect ratios. The quad dimensions must match the source HWND's client rect, converted to meters using the display's pixels-per-meter ratio. Handle DPI scaling (GetDpiForWindow). |
| 4A.5 | S | **Frame rate adaptation** — Capture frames arrive at the captured app's render rate (variable). Multi-compositor renders at display refresh rate (60Hz). Use the latest available capture frame; don't block if no new frame. Ring buffer or single-slot with atomic swap. |

**Architecture:**

```
Notepad (renders to HWND normally)
    │
    ├─ Windows.Graphics.Capture grabs backbuffer
    │
    ├─ ID3D11Texture2D (captured frame)
    │
    ├─ DXGI shared handle (or direct texture ref if same device)
    │
    └─ Multi-compositor virtual client slot
        ├─ Renders as textured quad (same pipeline as IPC clients)
        ├─ Mono: same texture for L and R eye passes
        └─ Level 2 Kooima provides spatial parallax from 3D pose
```

**Key API: `Windows.Graphics.Capture`** (preferred over DXGI Desktop Duplication):
- Per-window capture (not per-monitor) — captures exactly the HWND content, not overlapping windows
- Works with DWM-composited windows (no exclusive fullscreen limitation for captured apps)
- Delivers `ID3D11Texture2D` directly — no format conversion needed
- Handles window resize automatically (frame pool recreated on size change)
- Requires user consent on first use (system picker dialog) — but `GraphicsCaptureItem.CreateFromVisual()` with a known HWND bypasses this in programmatic mode (requires `GraphicsCaptureProgrammatic` capability, Win10 2004+)

**Fallback:** DXGI Desktop Duplication (`IDXGIOutputDuplication`) captures the entire monitor output. More complex (need to crop per-window from the full-screen capture), but works on older Windows versions and doesn't require programmatic capture capability.

**Key design decisions:**
- Capture runs on the service side (same process as multi-compositor, same D3D11 device) to avoid cross-process texture sharing overhead
- One capture session per adopted window — max ~20 concurrent captures (OS limit)
- Capture frame pool size: 2 frames (double-buffered, minimal latency)

## Phase 4B: Window Adoption and Lifecycle

**Goal:** When the shell activates, enumerate all visible windows and adopt them into the spatial scene. When the shell deactivates, release all captures and restore windows.

| Task | Size | Description |
|------|------|-------------|
| 4B.1 | M | **Window enumeration** — `EnumWindows` + filter for visible, top-level, non-system windows. Skip: taskbar (`Shell_TrayWnd`), desktop (`Progman`), system tray, own shell window, tooltips, popups. Collect: HWND, title, class name, client rect, `WINDOWPLACEMENT`. |
| 4B.2 | M | **App type classification** — Cross-reference enumerated HWNDs with `system_get_clients` (IPC-connected apps). HWNDs matching IPC clients → tag "3D" (already have shared textures). All others → tag "2D" (need capture). |
| 4B.3 | M | **Capture startup** — For each "2D" window: create `GraphicsCaptureSession`, start frame delivery, register as virtual client in multi-compositor. Assign default spatial pose (Expose-style grid or saved layout). |
| 4B.4 | M | **State snapshot for restore** — Before adoption, save per-window: HWND, `WINDOWPLACEMENT` (position, size, show state, min/max positions), z-order (via `GetWindow(GW_HWNDPREV)`), and style flags. Store in memory for Phase 4D restore. |
| 4B.5 | S | **Dynamic window tracking** — While shell is active, poll for new windows (`EnumWindows` every 500ms, diff against known set). New windows → auto-adopt with capture. Closed windows → remove virtual client slot + stop capture. |
| 4B.6 | S | **System tray residence** — Shell stays running after deactivation. System tray icon with context menu: "Open Shell", "Settings", "Exit". Tray icon indicates shell state (active/standby). |

**Window filtering heuristics:**

```c
bool should_adopt(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return false;  // owned popup
    if (hwnd == shell_hwnd) return false;                  // own window
    
    // Skip known system windows
    char class_name[256];
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    if (strcmp(class_name, "Shell_TrayWnd") == 0) return false;   // taskbar
    if (strcmp(class_name, "Progman") == 0) return false;          // desktop
    if (strcmp(class_name, "Button") == 0) return false;           // Start button
    if (strcmp(class_name, "Windows.UI.Core.CoreWindow") == 0) {
        // UWP system windows (Action Center, Start Menu)
        return false;
    }
    
    // Skip tiny windows (likely tooltips or invisible helpers)
    RECT rect;
    GetClientRect(hwnd, &rect);
    if ((rect.right - rect.left) < 100 || (rect.bottom - rect.top) < 100)
        return false;
    
    return true;
}
```

**Key insight:** The 2D windows don't need to be hidden or moved. DWM keeps rendering them to the desktop normally. The shell's fullscreen window occludes them. When we capture them, `Windows.Graphics.Capture` gets their content regardless of occlusion. On shell exit, they're already in the right place — we never moved them.

For OpenXR IPC apps, their HWNDs were already hidden (borderless, behind shell window) by the existing Phase 0 mechanism. Phase 4D handles restoring those.

## Phase 4C: Focus-Adaptive Rendering

**Goal:** Render mode and window position adapt automatically based on which app is focused and its type (2D vs 3D).

| Task | Size | Description |
|------|------|-------------|
| 4C.1 | S | **App type tag in slot model** — Each multi-compositor slot carries a `client_type` field: `CLIENT_TYPE_OPENXR_3D` (IPC client with stereo swapchain) or `CLIENT_TYPE_CAPTURED_2D` (virtual client from capture). |
| 4C.2 | M | **Snap-to-focus** — When a window gains focus, animate it to Z=0 (display plane) over ~200ms (lerp). Scale to a comfortable working size (e.g., 60-80% of display width). Previous focus target returns to its layout position. |
| 4C.3 | M | **Auto render mode switch** — On focus change, evaluate the focused app's type: 2D → switch to `MODE_2D` (no interlacing, full resolution mono). 3D → switch to `MODE_3D` (stereo + weave). No focus (overview) → default `MODE_3D`. Reuse existing `rendering_mode` shared memory mechanism. |
| 4C.4 | S | **Transition smoothness** — When switching 2D→3D or 3D→2D, the weaver toggles interlacing. This can flash. Mitigate by fading to black over 1-2 frames during the switch (fill atlas with black, switch mode, resume rendering). |
| 4C.5 | S | **Fullscreen toggle** — Double-click title bar (existing maximize behavior) but for focused window: fills entire display, hides all other windows. In 2D mode for 2D apps, 3D mode for 3D apps. Second double-click restores layout. |
| 4C.6 | S | **Overview mode** — When no window has focus (e.g., after clicking empty space), all windows return to their layout positions. Render mode defaults to 3D. This is the "spatial overview" state. |

**Render mode decision logic:**

```c
void on_focus_change(int new_focused_slot) {
    if (new_focused_slot < 0) {
        // No focus — overview mode
        set_render_mode(MODE_3D);
        animate_all_to_layout_positions();
        return;
    }
    
    struct window_state *win = &slots[new_focused_slot];
    
    // Snap focused window to display plane
    animate_window_to_z0(win, SNAP_DURATION_MS);
    
    // Switch render mode based on app type
    if (win->client_type == CLIENT_TYPE_CAPTURED_2D) {
        set_render_mode(MODE_2D);
    } else {
        set_render_mode(MODE_3D);
    }
}
```

**Design rationale for 2D mode on 2D focus:** When you're reading a document or browsing the web, interlacing artifacts and reduced resolution from the lenticular are pure downside — the content has no depth information to display. Switching to 2D gives the user full display resolution and zero artifacts for flat content. The 3D spatial layout is still visible in the peripheral windows.

**Open question:** Should unfocused windows continue rendering in the background during 2D focus mode? Options:
- **A) Freeze unfocused windows** — don't render them at all in 2D mode (saves GPU, but looks dead)
- **B) Render all, present mono** — render all windows but present the combined output in 2D mode (spatial layout visible but flat)
- **C) Fade unfocused windows** — dim/blur non-focused windows in 2D mode to indicate they're spatial background

Recommend option **B** — render all windows, present in 2D. The spatial layout is still useful context even when working in a 2D app. Users can see other windows in their peripheral vision. The performance cost is minimal since the expensive part (interlacing) is disabled in 2D mode.

## Phase 4D: Graceful Exit (Phase 1A Resolution)

**Goal:** When the shell exits or deactivates, all windows return to their original Windows desktop positions. OpenXR apps switch back to standalone compositing. The desktop looks exactly as it did before the shell was summoned.

| Task | Size | Description |
|------|------|-------------|
| 4D.1 | M | **Capture teardown** — Stop all `GraphicsCaptureSession` instances. Release capture frame pools and shared textures. Remove virtual client slots from multi-compositor. |
| 4D.2 | S | **2D window restore** — For each captured 2D window: call `SetWindowPlacement` with the saved snapshot from 4B.4. Restores position, size, show state (normal/maximized/minimized). Restore z-order by calling `SetWindowPos` with saved `HWND_PREV`. |
| 4D.3 | L | **OpenXR app hot-switch (Phase 1A)** — When shell deactivates, connected IPC apps must transition from IPC compositor to standalone in-process compositor. This is the deferred Phase 1A work. See detailed design below. |
| 4D.4 | S | **Shell window hide** — Shell's fullscreen window hides (`ShowWindow(SW_HIDE)`). DWM resumes compositing the desktop normally. Shell process stays alive in system tray. |
| 4D.5 | S | **Re-summon** — Next hotkey press: re-enumerate windows, re-capture, re-adopt. Previously connected OpenXR apps re-enter IPC mode. Layout restored from persistence (Phase 2D JSON config). |

**Phase 1A hot-switch design (4D.3):**

This is the hardest piece. When the shell deactivates, OpenXR apps need to switch from "IPC client with hidden HWND" back to "standalone app with visible HWND". Options:

**Option A: Full compositor swap (correct but complex)**
- Runtime sends `XrEventDataSessionStateChanged` to transition session through STOPPING → IDLE → READY → RUNNING
- App recreates swapchains on the new (standalone) compositor
- Risk: many apps don't handle mid-session compositor swap gracefully

**Option B: HWND restore + keep IPC (pragmatic)**
- Don't swap the compositor. Keep the IPC pipe alive but dormant.
- Restore the HWND: `SetWindowLong(GWL_STYLE, WS_OVERLAPPEDWINDOW)`, `SetWindowPlacement(saved)`, `ShowWindow(SW_SHOW)`
- The app's next frame submit goes to... nowhere useful (IPC compositor but no multi-comp rendering)
- Problem: app renders but output isn't displayed correctly without the multi-compositor

**Option C: Kill and relaunch (simple but lossy)**
- Shell saves app exe paths + window state
- On exit, terminate app processes
- On next summon, relaunch apps
- Problem: loses app state (unsaved work, game progress, etc.)

**Option D: Hide shell, keep rendering standalone-style (recommended)**
- When shell deactivates:
  1. Restore all 2D window captures (stop capture, `SetWindowPlacement`)
  2. For each OpenXR app: restore HWND to visible + decorated (`WS_OVERLAPPEDWINDOW`)
  3. Switch each per-client compositor back to "direct mode" — it creates its own DP instance and presents to its own HWND (the restored one)
  4. Multi-compositor stops rendering, releases shared DP
  5. Each app is now effectively standalone again, rendering to its own visible window
- When shell re-summons:
  1. Each per-client compositor switches back to "export mode" — stops direct present, exports shared textures
  2. Multi-compositor resumes, imports textures, takes over DP
  3. HWNDs go back to hidden/borderless

This is essentially a mode toggle on the per-client compositor: `direct` (owns DP + window) vs `export` (shared textures to multi-comp). The app's OpenXR session, swapchains, and rendering code are completely unaffected — only the compositor's output path changes.

**Key implementation:** Add a `comp_mode` field to the per-client compositor:
```c
enum comp_output_mode {
    COMP_MODE_DIRECT,  // standalone: compositor → DP → own HWND
    COMP_MODE_EXPORT,  // shell: compositor → shared texture → multi-comp
};
```

The mode switch is triggered by an IPC message from the shell (`shell_deactivate` / `shell_activate`). The compositor drains its current frame, switches output path, and continues. No session teardown needed.

## Phase 4E: App Launcher and System Tray

**Goal:** Built-in UI to launch new apps from within the shell. File browser or registered app list.

| Task | Size | Description |
|------|------|-------------|
| 4E.1 | M | **Registered apps config** — JSON file listing known apps: `[{name, exe_path, icon_path, type: "3d"|"2d"}]`. Pre-populated with demo apps and common apps (Notepad, Calculator, Chrome). |
| 4E.2 | L | **Launcher panel** — Rendered as a special window in the spatial scene. Grid of app tiles (icon + name). Click to launch. For "3d" apps: launch with `DISPLAYXR_SHELL_SESSION=1`. For "2d" apps: launch normally, auto-capture via 4B.5 dynamic tracking. |
| 4E.3 | M | **Quick launch from file system** — "Browse..." button in launcher opens a file browser (rendered as a 2D captured window) to select an exe. Or: text field where user types a path/command. |
| 4E.4 | S | **Auto-detect app type** — If a launched app connects via IPC within 5 seconds, tag as 3D. Otherwise, tag as 2D (capture-only). This handles unknown apps without user configuration. |

## Hotkey Integration

**System-wide hotkey registration:**

```c
// In shell main(), after initialization:
RegisterHotKey(NULL, HOTKEY_ID_TOGGLE_SHELL, MOD_CONTROL, VK_SPACE);

// In message loop:
case WM_HOTKEY:
    if (wParam == HOTKEY_ID_TOGGLE_SHELL) {
        if (shell_is_active())
            shell_deactivate();  // Phase 4D
        else
            shell_activate();    // Phase 4B + 4C
    }
    break;
```

`RegisterHotKey` is global (works when shell window is not focused). `Ctrl+Space` is a reasonable default — doesn't conflict with common app shortcuts. Could be configurable via settings JSON.

Alternative: `Win+Space` (mirrors macOS Spotlight pattern) but conflicts with Windows input language switcher. `Ctrl+`\`` (backtick) is another option.

## IPC Additions

| IPC Call | Direction | Description |
|----------|-----------|-------------|
| `shell_deactivate` | shell → runtime | Exit shell mode: stop multi-comp rendering, switch per-client compositors to direct mode, release shared DP |
| `shell_activate` | shell → runtime | Enter shell mode (already exists): switch per-client compositors to export mode, start multi-comp |
| `shell_add_capture_client(hwnd, name)` | shell → runtime | Register a captured 2D window as a virtual client slot |
| `shell_remove_capture_client(client_id)` | shell → runtime | Remove a captured window from the scene |
| `shell_get_client_type(client_id)` | runtime → shell | Returns `CLIENT_TYPE_OPENXR_3D` or `CLIENT_TYPE_CAPTURED_2D` |

## Key Architecture Notes

### Capture runs server-side

The `Windows.Graphics.Capture` sessions run inside `displayxr-service`, on the same D3D11 device as the multi-compositor. This avoids cross-process texture sharing for captured frames — the captured `ID3D11Texture2D` is directly usable by the multi-comp's blit pipeline. The shell app tells the service which HWNDs to capture via IPC; the service handles the actual capture and rendering.

### No app modification required for 2D capture

Captured 2D apps (Notepad, Chrome, VS Code) are completely unaware they're being captured. `Windows.Graphics.Capture` reads their backbuffer through DWM — no hooks, no injection, no special API. The app runs exactly as it would on a normal desktop.

### Render mode contract with display processor

The existing `rendering_modes[]` shared memory array already supports 2D and 3D modes. The auto-switch in 4C.3 sets `active_rendering_mode_index` the same way the V key does today. The DP reads this and enables/disables interlacing accordingly. No DP changes needed.

### Performance budget

| Component | Per-frame cost | Notes |
|-----------|---------------|-------|
| `Windows.Graphics.Capture` (per window) | ~0.5ms | DWM-driven, async delivery |
| Multi-comp blit (per window) | ~0.1ms | Single textured quad, already profiled in Phase 3 |
| Capture frame copy (if needed) | ~0.3ms | If captured texture isn't directly shareable; most cases are zero-copy |
| Render mode switch overhead | ~2ms (one-time) | Weaver reconfiguration on 2D↔3D toggle |

With 10 windows (5 captured + 5 OpenXR), total compositor overhead is ~6ms — well within 16ms frame budget (60Hz).

## Dependencies

| Dependency | Status | Notes |
|------------|--------|-------|
| Phase 3 (3D positioning) | Complete | Required for spatial layout of captured windows |
| Phase 3B (cross-API) | In progress | Nice-to-have for GL/VK OpenXR apps in shell |
| Phase 1A (hot-switch) | Deferred → resolved in 4D | Proper exit path |
| Windows 10 2004+ | Required | For programmatic `GraphicsCaptureItem` creation |
| D3D11 multithread protection | Complete (#108) | Required for concurrent capture + IPC clients |

## Files (Key Reference)

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp: extend slot model for virtual clients, capture integration |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | Public API: add capture client management |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window WndProc: hotkey handler, system tray |
| `src/xrt/ipc/server/ipc_server_handler.c` | IPC handlers: shell_deactivate, add/remove capture client |
| `src/xrt/ipc/shared/proto.json` | IPC protocol: new capture client messages |
| `src/xrt/targets/shell/main.c` | Shell app: window enumeration, adoption, lifecycle, launcher |
| `src/xrt/targets/shell/capture.cpp` | **New:** Windows.Graphics.Capture wrapper (C++/WinRT) |
| `src/xrt/targets/shell/launcher.c` | **New:** App launcher panel, registered apps config |

## Test Procedure

### 4A: Capture Compositor
```bash
# Build
scripts\build_windows.bat build

# Manual test: capture Notepad
# 1. Open Notepad, type some text
# 2. Launch shell with capture flag (TBD CLI syntax):
_package\bin\displayxr-shell.exe --capture-hwnd <notepad_hwnd>
# 3. Verify: Notepad content appears as a 2D panel in 3D space
# 4. Type in Notepad → captured texture updates in real time
```

### 4B: Window Adoption
```bash
# 1. Open several windows: Notepad, Calculator, a DisplayXR cube app
# 2. Press Ctrl+Space
# 3. Verify: all windows appear in spatial layout
#    - Cube app renders in 3D (stereo)
#    - Notepad and Calculator render as 2D panels
# 4. Close a window externally → verify it disappears from shell
# 5. Open a new window → verify it appears in shell
```

### 4C: Focus-Adaptive Rendering
```bash
# 1. Activate shell with mixed 2D/3D windows
# 2. Click Notepad → verify:
#    - Notepad snaps to Z=0, grows to working size
#    - Render mode switches to 2D (no interlacing)
# 3. Click cube app → verify:
#    - Cube snaps to Z=0
#    - Render mode switches to 3D (stereo + weave)
# 4. Click empty space → verify:
#    - All windows return to layout positions
#    - Render mode switches to 3D
```

### 4D: Graceful Exit
```bash
# 1. Activate shell with several windows
# 2. Press Ctrl+Space (or ESC) to deactivate
# 3. Verify: all windows back in original positions on Windows desktop
# 4. DisplayXR app continues rendering in its own window (standalone mode)
# 5. Press Ctrl+Space again → shell re-activates, all windows re-adopted
```

## Implementation Order

```
4A (capture compositor) ─────────────────────────┐
4B (window adoption) ──────── depends on 4A ─────┤
4C (focus-adaptive rendering) ── depends on 4B ──┤──→ 4E (launcher)
4D (graceful exit / 1A) ─────── independent ─────┘
```

4A and 4D can be developed in parallel. 4B builds on 4A. 4C builds on 4B. 4E is additive polish and can come at any point after 4B.

**Suggested branch:** `feature/shell-phase4-ci`
