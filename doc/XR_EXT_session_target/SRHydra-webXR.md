# SR Hydra WebXR Implementation Analysis

This document describes how SR Hydra implements WebXR support for Chrome browser, based on analysis of the SRHydra codebase. This serves as a reference for implementing WebXR support in CNSDK-OpenXR.

## Architecture Overview

SR Hydra uses a **client-server IPC architecture** with the following key components:

```
Chrome WebXR
    ↓
Native Messaging Host (openxr-bridge.cpp)
    ↓  JSON stdin/stdout
Trampoline DLL (in-process client)
    ↓  Named Pipes IPC
Runtime Service (out-of-process OpenXR runtime)
    ↓
LeiaSR Display (light field weaving)
```

## Key Architectural Difference: Swapchain Ownership

**SR Hydra (Working):** Service creates textures, shares handles TO client
```
Runtime Service                          Client (Chrome)
┌──────────────────────┐                ┌─────────────────────┐
│ Creates D3D11        │ ─DuplicateHandle→ │ Receives handle    │
│ textures with        │                │ Opens shared        │
│ SHARED flag          │                │ resource            │
│                      │                │ Renders to texture  │
└──────────────────────┘                └─────────────────────┘
```

**Monado (Current):** Client creates textures, shares handles TO service
```
Client (Chrome)                          Runtime Service
┌─────────────────────┐                ┌──────────────────────┐
│ Creates D3D11       │ ─share handle→ │ Imports via         │
│ textures            │                │ OpenSharedResource  │
│ Renders to texture  │                │                      │
└─────────────────────┘                └──────────────────────┘
```

**Why This Matters:** Chrome's GPU process is sandboxed. It's more reliable for the privileged runtime service to allocate shared resources and pass handles TO the browser, rather than expecting the sandboxed browser to export handles.

## IPC Protocol Details

### Named Pipe Architecture

SR Hydra uses 4 named pipes per connection:
- `{PipeName}_Inbound`: Server → Client messages
- `{PipeName}_InboundReturn`: Client responses
- `{PipeName}_Outbound`: Client → Server messages
- `{PipeName}_OutboundReturn`: Server responses

### Session Creation Flow

```
Client: xrCreateSession()
    ↓
Trampoline: FXRSession::CreateSession()
    ↓
IPC: IPC_CreateSession(messaging_handle, instance_ptr, system_id,
                       session_handle, graphics_type, out_session_ptr, out_result)
    ↓
Runtime: FIPCInboundHandler receives → dispatches to handler
    ↓
Runtime: Creates FXRSession, stores in thread-safe FArray
    ↓
Returns: session_ptr via IPC response
```

### Swapchain Creation Flow

```cpp
// Client-side call:
IPC_CreateSwapchain(
    messaging_handle,
    session_runtime_ptr,
    resolution (width, height),
    array_size,
    create_flags,
    usage_flags,
    format,
    client_process_id,  // ← KEY: Client PID for handle duplication
    &swapchain_ptr,
    &result
)

// Server-side:
1. Creates FXRSwapchainD3D11 with FrameBufferCount=1
2. Allocates FrameTargets[] (render targets for client)
3. Allocates FrameResources[] (compositor reference copy)
4. Creates ShareHandles[] via DuplicateHandle:
   - GetHeap()->GetShareHandle() on FrameTarget
   - DuplicateHandle(ProcessHandle, Handle, ClientProcessHandle, ...)
5. Returns duplicated handles valid in client's process space

// Client-side import:
OpenSharedTextureHandle(handle, resolution, array_size, format, usage_flags)
  → D3D11 device.OpenSharedResource() to import texture
```

## Dual-Buffer Pattern

SR Hydra uses a dual-buffer strategy for cross-process synchronization:

```
FrameTargets[]   ← Client renders here (shared via D3D11 handle)
       ↓ (synchronized copy)
FrameResources[] ← Compositor uses this copy for compositing
```

### Synchronization Protocol

```cpp
// Before texture copy (in ComposeLayers):
Target->GetHeap()->GetShareSync();    // Claim exclusive access

// Safe copy while holding sync
CommandList->CopyTexture(Resource, ImageArrayIndex, Target, ImageArrayIndex);

// Release to allow client access
Target->GetHeap()->ReleaseShareSync();
```

## Frame Submission Flow

```
Client (Chrome)
┌──────────────────────────────────────────────┐
│ 1. xrWaitFrame()                              │
│    IPC: IPC_WaitFrame() → get DisplayTime    │
│                                               │
│ 2. xrBeginFrame()                            │
│    IPC: IPC_BeginFrame()                     │
│                                               │
│ 3. Render to swapchain image                 │
│    (direct D3D11 rendering to shared texture)│
│                                               │
│ 4. xrEndFrame(layers)                        │
│    IPC chain:                                │
│    - IPC_AcquireImage(swapchain)             │
│    - IPC_WaitImage(swapchain)                │
│    - IPC_AddCompositionLayer(...)            │
│    - IPC_AddCompositionView(...) per layer   │
│    - IPC_ReleaseImage(swapchain)             │
│    - IPC_ComposeLayers()                     │
└──────────────────────────────────────────────┘
```

## Compositor Architecture

```cpp
struct FSessionCompositor {
    RHI::IRHIRenderTarget* FrameBuffers[2];  // Left eye, right eye
    RHI::IRHICommandList* CommandList;
    RHI::IRHIPipelineState* CompositionPipeline[2]; // Blit + Blend
    RHI::FRHIConstantBuffer* CompositionData;
}
```

### Composition Steps

1. **Texture Copy Phase** (in ComposeLayers):
   - Synchronize shared texture access using `GetShareSync()`
   - Copy FrameTarget → FrameResource for each swapchain image
   - Release sync with `ReleaseShareSync()`

2. **Composition Phase** (in Compose):
   - Set viewport for left/right eye halves
   - For each layer/view: configure UV mapping, set texture, apply blend
   - Output to FrameBuffers[0] (left), FrameBuffers[1] (right)

3. **LeiaSR Integration**:
   - Composed stereo frames sent to SRWindowCompositor
   - SRWindowCompositor handles light field weaving

## Native Messaging Host (WebXR Bridge)

The `openxr-bridge.cpp` acts as JSON-based IPC translator:

```
Chrome WebXR Extension
    ↓ (stdin: 4-byte length + JSON payload)
Native Messaging Host
    ↓ (stdout: JSON response)

Example message: {"setting": "IPDScale", "value": 1.5, "tabId": 42}
    ↓
FindWindow("SR | Hydra")
    ↓
SendMessage(hwnd, OPENXR_MESSAGE_SET_IPD_SCALE, Data[0], Data[2])
    ↓
Runtime updates FSessionSettings
```

## Key Design Patterns

### 1. Thread-Safe Singleton Collections
```cpp
FThreadSafe<FArray<FXRSession*>> Sessions;
auto Lock = Sessions.Lock();
Lock->Add(session);
```

### 2. Dual-Buffer for Cross-Process Safety
```cpp
FrameTargets[]   ← Client writes
FrameResources[] ← Compositor reads (copied with sync)
```

### 3. Handle Duplication for Process Isolation
```cpp
HANDLE GetSharableTextureHandle(RHI::FRHITexture* Texture, DWORD ClientProcessId) {
    HANDLE Handle = Texture->GetHeap()->GetShareHandle();
    HANDLE ClientProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, ClientProcessId);
    HANDLE DuplicatedHandle;
    DuplicateHandle(GetCurrentProcess(), Handle, ClientProcess, &DuplicatedHandle,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    return DuplicatedHandle;
}
```

## Implications for CNSDK-OpenXR

### What Needs to Change

1. **Implement `compositor_create_swapchain`** in `comp_d3d11_service.cpp`:
   - Currently returns `XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED`
   - Must allocate D3D11 textures with `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
   - Return handles to IPC layer for duplication to client

2. **Use `DuplicateHandle` in IPC layer**:
   - `ipc_message_channel_windows.cpp` has `ipc_send_handles` with DuplicateHandle
   - Ensure it targets client's process ID when sending swapchain handles

3. **Add Share Synchronization** (optional, may already be sufficient):
   - Monado uses `IDXGIKeyedMutex` (`AcquireSync`/`ReleaseSync`)
   - This is functionally equivalent to SR Hydra's `GetShareSync`/`ReleaseShareSync`

### Why Client→Service Fails for WebXR

Chrome's GPU process is sandboxed. The sandbox restricts:
- Creating globally sharable D3D11 resources
- Exporting handles to external processes

The privileged runtime service must:
1. Create the shared textures
2. Use `DuplicateHandle` to inject handles into Chrome's process
3. Chrome opens the duplicated handle via `OpenSharedResource`

This is the opposite of Monado's current model where the client creates and exports.

## References

- SR Hydra source: `/Users/david.fattal/Documents/GitHub/SRHydra`
- Key files:
  - `Runtime/Source/Endpoints/Runtime/` - Service implementation
  - `Runtime/Source/Endpoints/Trampoline/` - Client library
  - `native-messaging-host/openxr-bridge.cpp` - Chrome bridge
  - `Runtime/Source/XR/Swapchain.cpp` - Swapchain handle sharing
  - `Runtime/Source/XR/Compositor.cpp` - Frame composition
