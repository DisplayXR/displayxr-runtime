# SR SDK Race Condition Bug in WndProcDispatcher

## Summary

There is a use-after-free race condition in `WeaverBaseImpl.ipp` that causes crashes when window messages (especially mouse movement) arrive during weaver destruction.

**Affected APIs:** All graphics APIs on Windows (DX9, DX10, DX11, DX12, Vulkan, OpenGL) - the bug is in the shared `WeaverBaseImpl` base class.

This was discovered while testing `XR_EXT_session_target` where the window handle is passed from the application, but it can affect any application that destroys a weaver while the window is still receiving messages.

## Symptoms

- Application crashes when moving the mouse over the window during shutdown
- Crash occurs after `xrDestroySession` is called
- All initialization and rendering works correctly; crash only happens during cleanup
- Higher mouse movement frequency increases crash likelihood

## Root Cause

In `modules/DimencoWeaving/sr/weaver/WeaverBaseImpl.ipp`, the `WndProcDispatcher` function releases the lock **before** dereferencing the instance pointer:

```cpp
LRESULT CALLBACK WeaverBaseImpl::WndProcDispatcher(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WeaverBaseImpl* instance = nullptr;

    // Acquire the lock to access the global map
    AcquireSRWLockShared(&g_srwLock);
    auto it = g_windowObjectMap.find(hWnd);
    if (it != g_windowObjectMap.end())
    {
        instance = it->second;
    }
    ReleaseSRWLockShared(&g_srwLock);  // <-- Lock released HERE

    if (instance)
    {
        return instance->weaverWndProc(hWnd, message, wParam, lParam);  // <-- CRASH: instance may be destroyed
    }
    SR::Log::error("WeaverBaseImpl: no handler for this window.");
    return DefWindowProc(hWnd, message, wParam, lParam);
}
```

## Race Condition Sequence

1. **Thread A (Windows message pump)**: Calls `WndProcDispatcher` with a mouse message
2. **Thread A**: Acquires shared lock, retrieves `instance` pointer from map
3. **Thread A**: Releases shared lock
4. **Thread B (Application/OpenXR)**: Calls weaver destructor
5. **Thread B**: `restoreOriginalWindowProc()` restores original WndProc via `SetWindowLongPtr`
6. **Thread B**: Acquires exclusive lock, erases from `g_windowObjectMap`, releases lock
7. **Thread B**: Destructor completes, object memory is freed
8. **Thread A**: Dereferences `instance` pointer → **Use-after-free crash**

The race window between steps 3 and 8 is very small, but mouse messages are high-frequency (often 100+ Hz), making this crash reproducible.

## Suggested Fix

Keep the shared lock held while calling `weaverWndProc`:

```cpp
LRESULT CALLBACK WeaverBaseImpl::WndProcDispatcher(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    AcquireSRWLockShared(&g_srwLock);
    auto it = g_windowObjectMap.find(hWnd);
    if (it != g_windowObjectMap.end())
    {
        WeaverBaseImpl* instance = it->second;
        LRESULT result = instance->weaverWndProc(hWnd, message, wParam, lParam);
        ReleaseSRWLockShared(&g_srwLock);  // Lock held until after the call
        return result;
    }
    ReleaseSRWLockShared(&g_srwLock);
    SR::Log::error("WeaverBaseImpl: no handler for this window.");
    return DefWindowProc(hWnd, message, wParam, lParam);
}
```

This ensures:
- The instance cannot be removed from the map while we're using it
- The destructor's `restoreOriginalWindowProc()` will block on the exclusive lock until all in-flight `weaverWndProc` calls complete

### Alternative Fix (Reference Counting)

If holding the lock during `weaverWndProc` is a concern for performance, consider adding reference counting:

```cpp
LRESULT CALLBACK WeaverBaseImpl::WndProcDispatcher(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    std::shared_ptr<WeaverBaseImpl> instance;

    AcquireSRWLockShared(&g_srwLock);
    auto it = g_windowObjectMap.find(hWnd);
    if (it != g_windowObjectMap.end())
    {
        instance = it->second->shared_from_this();  // Prevent destruction
    }
    ReleaseSRWLockShared(&g_srwLock);

    if (instance)
    {
        return instance->weaverWndProc(hWnd, message, wParam, lParam);
    }
    // ...
}
```

## Workaround in Monado

Until this is fixed in the SR SDK, we've implemented a workaround in `leiasr_destroy()`:

1. Pump all pending window messages before destroying the weaver
2. Explicitly call `weaver->destroy()` to trigger `restoreOriginalWindowProc()`
3. Add small delays (50ms) to let in-flight handlers complete
4. Pump messages again after destroy

This reduces but does not completely eliminate the race window.

## Affected Code Locations

**Bug location:**
- `modules/DimencoWeaving/sr/weaver/WeaverBaseImpl.ipp` - Lines 106-125 (`WndProcDispatcher`)
- `modules/DimencoWeaving/sr/weaver/WeaverBaseImpl.ipp` - Lines 207-228 (`restoreOriginalWindowProc`)

**Libraries using this shared code:**
- `modules/srDirectX/dxweaver/sr/weaver/dxweaver_base_impl.cpp` → DX9, DX10, DX11, DX12
- `modules/srVulkan/vkweaver/sr/weaver/vulkanweaver_base_impl.cpp` → Vulkan
- `modules/srOpenGL/glweaver/sr/weaver/glweaver_base_impl.cpp` → OpenGL

## Test Case

1. Create an OpenXR application using `XR_EXT_session_target` to pass a window handle
2. Run the application on an SR display
3. Verify weaving works correctly
4. While the application is running, move the mouse rapidly over the window
5. Close the application (trigger `xrDestroySession`)
6. Observe crash during shutdown

The crash is intermittent but highly reproducible with rapid mouse movement during shutdown.

## Contact

For questions about this issue or the Monado integration, please contact the OpenXR runtime team.
