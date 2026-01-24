# GET-STARTED.md

A quick-start guide for using the SRMonado OpenXR runtime on Leia SR devices.

## Installation

Run the installer (`SRMonadoSetup-X.X.X.exe`). It will:

1. Install files to `C:\Program Files\LeiaSR\SRMonado\`
2. Set registry keys to register SRMonado as the active OpenXR runtime:
   ```
   HKEY_LOCAL_MACHINE\Software\Khronos\OpenXR\1\ActiveRuntime
     = "C:\Program Files\LeiaSR\SRMonado\SRMonado_win64.json"
   ```

## Common Questions

### Do I need to run the service (monado-service.exe)?

**No.** SRMonado uses **in-process mode** by default. The runtime library (`SRMonadoClient.dll`) loads directly into your application's process. There's no separate service to start.

#### What is in-process mode?

```
┌─────────────────────────────────────────┐
│           Your Application.exe          │
│  ┌───────────────────────────────────┐  │
│  │  Your app code                    │  │
│  │         │                         │  │
│  │         ▼                         │  │
│  │  SRMonadoClient.dll               │  │
│  │  ├── OpenXR state tracker         │  │
│  │  ├── Compositor                   │  │
│  │  ├── SR Weaver                    │  │
│  │  └── Drivers                      │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
Everything runs in ONE process. No service needed.
```

#### What is monado-service for then?

The `monado-service.exe` exists because SRMonado is built from Monado's codebase, which was designed for **VR headsets**. Service mode solves VR-specific problems:

| VR Headset Need | Leia SR Reality |
|-----------------|-----------------|
| Exclusive HMD hardware access | It's just a monitor - no exclusive access needed |
| Persistent head tracking state | Eye tracking is handled by SR Tracker Service (Leia's service) |
| Single display output shared by apps | XR_EXT_session_target gives each app its own window |

For Leia SR, tracking state is already shared via **Leia's SR Tracker Service** (part of CNSDK), not Monado:

```
┌──────────────┐  ┌──────────────┐
│ App 1        │  │ App 2        │
│ └─ SR Weaver │  │ └─ SR Weaver │
└──────┬───────┘  └──────┬───────┘
       │                 │
       │ SR SDK API      │ SR SDK API
       ▼                 ▼
┌─────────────────────────────────┐
│     SR Tracker Service          │  ← Leia's service (not Monado)
│     (part of CNSDK/SR SDK)      │
│     - Eye tracking              │
│     - Face detection            │
│     - Shared state              │
└─────────────────────────────────┘
```

**Bottom line:** You don't need `monado-service.exe` for Leia SR workflows.

### Do I need to add SRMonado to PATH?

**No.** The OpenXR loader finds the runtime through the registry, not PATH. As long as:
- The installer ran successfully
- `ActiveRuntime` registry key points to `SRMonado_win64.json`

...any OpenXR application will automatically use SRMonado.

### How do I verify the runtime is registered?

Check the registry:
```powershell
reg query "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime
```

Should output:
```
ActiveRuntime    REG_SZ    C:\Program Files\LeiaSR\SRMonado\SRMonado_win64.json
```

## Compositor: Vulkan vs D3D11

SRMonado has two compositor implementations:

| Compositor | Default? | Requirements | SR Weaver |
|------------|----------|--------------|-----------|
| Vulkan | Yes | None | `leia_interlacer_vulkan` |
| D3D11 Native | No | Opt-in + XR_EXT_session_target | `leiasr_d3d11` |

### Default: Vulkan Compositor

By default, even D3D11 applications use the Vulkan compositor with D3D11-to-Vulkan texture interop:

```
App (D3D11) → D3D11↔Vulkan interop → Vulkan Compositor → leia_interlacer_vulkan → Display
```

### Optional: D3D11 Native Compositor

For a pure D3D11 pipeline (better Intel GPU compatibility), opt in with:

```powershell
$env:OXR_ENABLE_D3D11_NATIVE_COMPOSITOR = "1"
./your_app.exe
```

This gives you:
```
App (D3D11) → D3D11 Compositor → leiasr_d3d11 weaver → Display
```

**Requirements for D3D11 native compositor:**
- Must use `XR_EXT_session_target` (provide your own HWND)
- Must be in-process mode (default)
- Must set the environment variable

## Using Existing OpenXR Apps (Blender, etc.)

Existing OpenXR applications that don't know about `XR_EXT_session_target` will still work:

```
Blender.exe starts
    │
    ▼
Blender calls xrCreateInstance()
    │
    ▼
OpenXR Loader reads registry → finds SRMonado
    │
    ▼
SRMonadoClient.dll loads into Blender's process (in-process mode)
    │
    ▼
Runtime creates its own window for 3D output
```

However, without `XR_EXT_session_target`:

| Aspect | With Extension | Without (Blender, etc.) |
|--------|----------------|------------------------|
| Window | App provides HWND | Runtime creates its own |
| Input | App receives keyboard/mouse | Runtime window captures input |
| Multi-app | Multiple apps can run | Typically one app at a time |
| D3D11 native compositor | Available | Not available |

## Using the XR_EXT_session_target Extension

The `XR_EXT_session_target` extension allows your app to provide its own window (HWND) to the runtime. This enables:

- **Direct input handling** - Your app receives keyboard/mouse/gamepad events
- **Windowed 3D output** - Interlaced 3D rendered to your window
- **Multiple XR apps** - Run several XR applications simultaneously

### Does the extension register automatically?

**Yes.** The extension is built into SRMonado. When you create an OpenXR instance, you can query for it:

```c
// Check if extension is available
uint32_t extensionCount;
xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);

std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());

for (const auto& ext : extensions) {
    if (strcmp(ext.extensionName, "XR_EXT_session_target") == 0) {
        // Extension is available!
    }
}
```

### How to use the extension

1. **Create your own window** (Win32):
   ```c
   HWND myWindow = CreateWindowEx(0, "MyClass", "My XR App",
       WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
       1280, 720, nullptr, nullptr, hInstance, nullptr);
   ShowWindow(myWindow, SW_SHOW);
   ```

2. **Request the extension** when creating the instance:
   ```c
   const char* extensions[] = { "XR_EXT_session_target", ... };
   XrInstanceCreateInfo createInfo = {
       .enabledExtensionCount = 1,
       .enabledExtensionNames = extensions,
       ...
   };
   xrCreateInstance(&createInfo, &instance);
   ```

3. **Pass your HWND** when creating the session:
   ```c
   #include <openxr/XR_EXT_session_target.h>

   XrSessionTargetCreateInfoEXT targetInfo = {
       .type = XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT,
       .next = nullptr,
       .windowHandle = myWindow,  // Your HWND
   };

   XrSessionCreateInfo sessionInfo = {
       .type = XR_TYPE_SESSION_CREATE_INFO,
       .next = &targetInfo,  // Chain the extension struct
       .systemId = systemId,
   };

   xrCreateSession(instance, &sessionInfo, &session);
   ```

4. **Handle input in your message loop**:
   ```c
   MSG msg;
   while (PeekMessage(&msg, myWindow, 0, 0, PM_REMOVE)) {
       TranslateMessage(&msg);
       DispatchMessage(&msg);
       // Your app gets all keyboard/mouse/gamepad input!
   }
   ```

## Running SessionTargetTest

The `session_target_test` app demonstrates the complete extension workflow.

### Build it

```bash
cd build
cmake --build . --target session_target_test
```

### Run it

```bash
# Default (Vulkan compositor)
./bin/session_target_test.exe

# With D3D11 native compositor
$env:OXR_ENABLE_D3D11_NATIVE_COMPOSITOR = "1"
./bin/session_target_test.exe
```

Or from the install directory after running the installer:
```bash
"C:\Program Files\LeiaSR\SRMonado\session_target_test.exe"
```

### What you'll see

- A window with a rotating 3D cube (stereoscopic interlacing on Leia display)
- Text overlay showing:
  - Session state (Running/Focused)
  - FPS and frame time
  - Input state (keyboard, mouse position)
  - Eye tracking position (if available)

### Controls

| Input | Action |
|-------|--------|
| WASD | Move camera |
| Mouse drag | Look around |
| ESC | Quit |

## Troubleshooting

### App doesn't find OpenXR runtime

1. Check registry is set correctly (see above)
2. Try setting `XR_RUNTIME_JSON` environment variable explicitly:
   ```powershell
   $env:XR_RUNTIME_JSON = "C:\Program Files\LeiaSR\SRMonado\SRMonado_win64.json"
   ./your_app.exe
   ```

### Extension not found

Ensure you're requesting `"XR_EXT_session_target"` in `enabledExtensionNames` when calling `xrCreateInstance()`.

### No 3D effect visible

- Ensure you're on a Leia SR display
- The SR weaver requires the display to be in 3D mode
- Check that eye tracking is working (visible in SessionTargetTest overlay)

### Window appears but is blank

- Verify D3D11/Vulkan is initialized correctly
- Check that swapchains were created successfully
- Look for errors in the console output

## Architecture Overview

```
┌─────────────────────────────────────────────┐
│              Your Application               │
│  ┌─────────────────────────────────────┐   │
│  │  Creates HWND                        │   │
│  │  Handles input (keyboard/mouse)      │   │
│  │  Renders via OpenXR                  │   │
│  └─────────────────────────────────────┘   │
└──────────────────┬──────────────────────────┘
                   │ xrCreateSession(HWND)
                   ▼
┌─────────────────────────────────────────────┐
│           SRMonadoClient.dll                │
│  ┌─────────────────────────────────────┐   │
│  │  OpenXR state tracker               │   │
│  │  Compositor (Vulkan or D3D11)       │   │
│  │  SR Weaver (3D interlacing)         │   │
│  └─────────────────────────────────────┘   │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│         SR Tracker Service (Leia)           │
│         Eye tracking / face detection       │
└─────────────────────────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│           Your Window (HWND)                │
│           3D stereoscopic display           │
└─────────────────────────────────────────────┘
```

## Further Reading

- `doc/XR_EXT_session_target/` - Detailed extension documentation
- `test_apps/session_target_test/` - Example source code
- `src/xrt/state_trackers/oxr/oxr_session.c` - Extension implementation
