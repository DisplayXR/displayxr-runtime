# Shell Multi-App Black Flash — Debug Notes

## Problem

When running 2+ apps in shell mode, intermittent black frames flash in individual windows. Pattern:
- One window at a time (not all windows simultaneously)
- Regular/periodic blinking (~10 blinks in window 1, then ~8 in window 3, etc.)
- Cycles between windows
- Pre-existing issue (mentioned in Phase 3 status), NOT a Phase 3B regression

## Architecture

```
Client A thread:                    Client B thread:
  blit A's swapchain → A's atlas      blit B's swapchain → B's atlas
  lock render_mutex                    lock render_mutex (BLOCKED)
  multi_compositor_render:             (waiting...)
    clear combined_atlas               
    draw A's atlas → combined          
    draw B's atlas → combined    ← READS B's atlas while B may be mid-blit
    draw C's atlas → combined          
    weave → present                    
  unlock render_mutex                  
                                       multi_compositor_render:
                                         clear combined_atlas
                                         ...
```

## Root Cause (Most Likely)

The per-client atlas blit (swapchain → atlas) is NOT under `render_mutex`. The render IS under `render_mutex`. So when client A renders, it reads ALL clients' atlases — but client B might be mid-blit to B's atlas on another thread. The D3D11 multithread protection makes individual API calls atomic, but the SEQUENCE of calls (left eye CopySubresourceRegion + right eye CopySubresourceRegion) is NOT atomic. Between the two copies, the render can read B's atlas and see only one eye updated (or a partially-written texture).

The "one window at a time" pattern is a beat frequency between each client's blit timing and the render timing.

## What Was Tried

### 1. Double-buffered atlas
- Added `atlas_back`/`atlas_back_srv`/`atlas_back_rtv` per client
- layer_commit writes to back, then swaps front↔back
- **Result: STROBOSCOPE** — worse than before. The swap timing interacts badly with multi_compositor_render being called from each client's commit.

### 2. Per-client `blit_in_progress` atomic flag  
- `std::atomic<bool>` set before blit, cleared after
- multi_compositor_render skips clients where flag is set
- **Result: NO EFFECT** — the timing window is too narrow; the flag is usually cleared before the render checks it, or the D3D11 multithread protection serializes operations differently.

### 3. render_mutex during blit
- Added `std::lock_guard<std::recursive_mutex>` around the blit block
- Serializes blit + render across all clients
- **Result: STROBOSCOPE** — serializing all 4 clients' blits causes frame starvation.

### 4. VSync off (Present(0,0))
- Changed swap chain present from VSync (1,0) to immediate (0,0)
- **Result: NO EFFECT** — flashing unchanged, confirming it's not a VSync issue.

### 5. Skip uninitialized clients
- Skip rendering clients with `content_view_w == 0` (never committed a frame)
- **Result: NO EFFECT on steady-state flashing** — only helps at startup.

## What Hasn't Been Tried

### A. Per-client mutex (non-recursive, try-lock in render)
- Each client gets its own mutex
- Blit holds the client's mutex
- Render uses `try_lock` — if locked (client mid-blit), render uses the stale atlas content already in the combined atlas from the previous render
- **Pro:** No contention between clients, only skips one window per render
- **Con:** Combined atlas already cleared to dark gray, so skipping a window shows dark gray background instead of stale content

### B. Don't clear combined atlas
- Remove the `ClearRenderTargetView` at line 4962
- Since all windows are redrawn every render, the only "stale" content is background between windows
- **Pro:** If a window's blit is skipped (approach A), the previous frame's content persists
- **Con:** When windows move/resize, old content ghosts remain. Need explicit background rectangles.

### C. Copy atlas to combined atlas atomically
- Instead of per-view `CopySubresourceRegion` (2 calls for SBS), use a single `CopyResource` of the entire per-client atlas
- **Pro:** Single D3D11 call = atomic with multithread protection
- **Con:** Copies more data than needed (full atlas vs just the tiles that changed)

### D. KeyedMutex timeout handling
- Check if `AcquireSync(0, 100)` is timing out (log when it does)
- If timeout, SKIP the blit (use previous atlas content)
- **Pro:** Directly addresses the case where the shared texture is unavailable
- **Con:** Only applies if the root cause is actually KeyedMutex contention

### E. Reduce render frequency
- Instead of rendering on EVERY client's layer_commit, render at most once per VSync (throttle)
- Skip render if <16ms since last render
- **Pro:** Reduces contention, each render sees more clients' atlases fully written
- **Con:** Adds latency for some clients

## Recommended Next Step

**Try approach C first** (single `CopyResource` instead of per-view `CopySubresourceRegion`). This is the key insight: the per-view blit loop does 2+ separate `CopySubresourceRegion` calls to write the per-client atlas. Between these calls, another thread's render can read the partially-written atlas. A single `CopyResource` is atomic from D3D11 multithread protection's perspective — the render thread either sees the old atlas or the new one, never a partial.

Implementation: instead of copying individual tile regions from the swapchain into the per-client atlas, copy the ENTIRE swapchain texture to the atlas in one call. If the swapchain layout already matches the atlas tiling (which it does for extension apps using SBS layout), `CopyResource` works directly. For cases where the swapchain sub-rects don't match the atlas layout, blit to a STAGING atlas first (per-view copies), then `CopyResource` the staging atlas to the real atlas atomically.

If that doesn't work, try **approach E** (throttle render to once per VSync). The compositor currently renders 4x per frame cycle (once per client commit). Throttling to 1x reduces contention.

## Key Source Locations

- `comp_d3d11_service.cpp:6197` — `if (!zero_copy)` blit loop (writes per-client atlas)
- `comp_d3d11_service.cpp:4962` — `ClearRenderTargetView` (clears combined atlas)
- `comp_d3d11_service.cpp:4990-5100` — render loop (reads per-client atlases → combined atlas)
- `comp_d3d11_service.cpp:5731` — `Present(1, 0)` (VSync present)
- `comp_d3d11_service.cpp:6418-6421` — shell mode: lock render_mutex → multi_compositor_render
- `comp_d3d11_service.cpp:6080-6090` — KeyedMutex acquire with 100ms timeout
