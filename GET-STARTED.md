# GET-ME-STARTED.md

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

### Do I need to run the service?

**No.** SRMonado uses **in-process mode** by default. The runtime library (`SRMonadoClient.dll`) loads directly into your application. There's no separate service to start.

The `monado-service.exe` is included for potential future multi-client scenarios but is **not required** for normal use.

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
│  │  Compositor (layer compositing)     │   │
│  │  SR Weaver (3D interlacing)         │   │
│  │  Eye tracking integration           │   │
│  └─────────────────────────────────────┘   │
└──────────────────┬──────────────────────────┘
                   │ Interlaced output
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
