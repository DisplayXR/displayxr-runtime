# SR Hydra WebXR Implementation Reference

This document provides a comprehensive reference for the SR Hydra WebXR implementation, focusing on the core D3D11 cross-process swapchain sharing that enables Chrome WebXR support. This serves as a guide for implementing similar functionality in CNSDK-OpenXR.

**SR Hydra Source Location:** `/Users/david.fattal/Documents/GitHub/SRHydra`

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Why Service-Owned Textures](#why-service-owned-textures)
3. [D3D11 Shared Texture Creation](#d3d11-shared-texture-creation)
4. [DuplicateHandle for Chrome Sandbox](#duplicatehandle-for-chrome-sandbox)
5. [Client-Side Texture Import](#client-side-texture-import)
6. [KeyedMutex Synchronization](#keyedmutex-synchronization)
7. [IPC Protocol](#ipc-protocol)
8. [Session Management](#session-management)
9. [Swapchain Implementation](#swapchain-implementation)
10. [Frame Submission Flow](#frame-submission-flow)
11. [Compositor Architecture](#compositor-architecture)
12. [Key Source Files](#key-source-files)

---

## Architecture Overview

SR Hydra uses a **client-server IPC architecture**:

```
Chrome WebXR (GPU Process)
    ↓
Trampoline DLL (SRHydraClient.dll - in-process client)
    ↓  Named Pipes IPC
Runtime Service (SRHydra.exe - out-of-process OpenXR runtime)
    ↓
LeiaSR Display (light field weaving)
```

### Key Architectural Principle: Service Creates Textures

**SR Hydra (Working):** Service creates textures, duplicates handles TO client
```
Runtime Service                          Client (Chrome)
┌──────────────────────┐                ┌─────────────────────┐
│ Creates D3D11        │                │ Receives duplicated │
│ textures with        │ ─DuplicateHandle→ │ handle              │
│ SHARED flags         │                │ Opens via           │
│                      │                │ OpenSharedResource1 │
│ Owns texture         │                │ Renders to texture  │
└──────────────────────┘                └─────────────────────┘
```

**Monado (Current):** Client creates textures, shares handles TO service
```
Client (Chrome)                          Runtime Service
┌─────────────────────┐                ┌──────────────────────┐
│ Creates D3D11       │                │ Imports via         │
│ textures            │ ─share handle→ │ OpenSharedResource  │
│ Renders to texture  │                │ ❌ FAILS (sandbox)   │
└─────────────────────┘                └──────────────────────┘
```

---

## Why Service-Owned Textures

Chrome's GPU process runs in a **sandboxed AppContainer**. The sandbox restricts:

1. **Creating globally sharable D3D11 resources** - Limited permissions
2. **Exporting handles to external processes** - Blocked by AppContainer

The solution: The **privileged runtime service** must:
1. Create the shared textures with appropriate flags
2. Use `DuplicateHandle` to inject handles INTO Chrome's process handle table
3. Chrome opens the already-duplicated handle via `OpenSharedResource1`

This is the **opposite** of Monado's current model where the client creates and exports.

---

## D3D11 Shared Texture Creation

### Source: `Runtime/Dependencies/WyvernEngine/Source/WyvernEngine/Source/RHI/RHID3D11/Source/RHIHeapD3D11.cpp`

```cpp
void FRHIHeapD3D11::CreateBuffer()
{
    if (ResourceDesc.Type == HR_Texture)
    {
        D3D11_TEXTURE2D_DESC Desc;
        Desc.Width = ResourceDesc.Texture.Resolution.X;
        Desc.Height = ResourceDesc.Texture.Resolution.Y;
        Desc.MipLevels = 1;
        Desc.ArraySize = ResourceDesc.Texture.ArraySize;
        Desc.Format = ToDXGIFormat(ResourceDesc.Texture.Format);
        Desc.SampleDesc.Count = 1;
        Desc.SampleDesc.Quality = 0;
        Desc.Usage = D3D11_USAGE_DEFAULT;
        Desc.BindFlags = 0;
        Desc.CPUAccessFlags = 0;
        Desc.MiscFlags = 0;

        // Add bind flags based on usage
        if (ResourceDesc.Texture.IsShaderResource)
            Desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        if (ResourceDesc.Texture.IsColorTarget)
            Desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (ResourceDesc.Texture.IsDepthTarget)
            Desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        if (ResourceDesc.Texture.IsUnordered)
            Desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

        // CRITICAL: These flags enable cross-process texture sharing
        if (ResourceDesc.Texture.Sharable)
        {
            Desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;   // NT Handle support
            Desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX; // KeyedMutex for sync
        }

        // Create the texture
        ID3D11Texture2D* Texture = nullptr;
        HRESULT hr = GetTypedAdapter<FRHIAdapterD3D11>()->GetDevice()->CreateTexture2D(
            &Desc, nullptr, &Texture);
        Resource = Texture;

        // Create shared handle for cross-process access
        if (ResourceDesc.Texture.Sharable && Resource != nullptr)
        {
            IDXGIResource1* DXGIResource = nullptr;
            Resource->QueryInterface(__uuidof(IDXGIResource1), (void**)&DXGIResource);

            // Create NT handle that can be duplicated across processes
            DXGIResource->CreateSharedHandle(
                nullptr,  // No security attributes
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr,  // No name
                &ShareHandle);

            DXGIResource->Release();

            // Get KeyedMutex for synchronization
            Resource->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&ShareMutex);
        }
    }
}
```

### Required D3D11 Flags Summary

| Flag | Purpose |
|------|---------|
| `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` | Enables NT handle creation for `DuplicateHandle` |
| `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` | Enables `IDXGIKeyedMutex` for cross-process sync |

### Creating the Shared Handle

```cpp
IDXGIResource1* DXGIResource = nullptr;
texture->QueryInterface(__uuidof(IDXGIResource1), (void**)&DXGIResource);

HANDLE ShareHandle = nullptr;
DXGIResource->CreateSharedHandle(
    nullptr,  // SECURITY_ATTRIBUTES* - nullptr for default
    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
    nullptr,  // Name - nullptr for anonymous
    &ShareHandle);
```

---

## DuplicateHandle for Chrome Sandbox

### Source: `Runtime/Source/Endpoints/Runtime/Source/OpenXR/Swapchain.cpp`

```cpp
HANDLE FXRSwapchainD3D11::GetSharableTextureHandle(RHI::FRHITexture* Texture, DWORD ClientProcessId)
{
    // Get the original share handle from the texture heap
    HANDLE Handle = reinterpret_cast<HANDLE>(Texture->GetHeap()->GetShareHandle());

    HANDLE ProcessHandle = GetCurrentProcess();
    HANDLE DuplicationHandle = NULL;

    // Open the client process with PROCESS_DUP_HANDLE permission
    // This allows duplicating handles INTO the client's handle table
    HANDLE ClientProcessHandle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, ClientProcessId);

    if (ClientProcessHandle == NULL)
    {
        Log(FLogOpenXR, Error, "Failed to open client process for handle duplication");
        return NULL;
    }

    // Duplicate the handle from runtime's table to client's table
    // DUPLICATE_SAME_ACCESS preserves the original access rights
    BOOL success = DuplicateHandle(
        ProcessHandle,          // Source process (runtime)
        Handle,                 // Source handle
        ClientProcessHandle,    // Target process (Chrome)
        &DuplicationHandle,     // Output: handle valid in target process
        0,                      // Desired access (ignored with DUPLICATE_SAME_ACCESS)
        FALSE,                  // Not inheritable
        DUPLICATE_SAME_ACCESS); // Copy access rights from source

    CloseHandle(ProcessHandle);
    CloseHandle(ClientProcessHandle);

    if (!success)
    {
        Log(FLogOpenXR, Error, "DuplicateHandle failed");
        return NULL;
    }

    // Return the handle value that's valid in the CLIENT's address space
    // Note: This handle value is meaningless in the runtime's process!
    return DuplicationHandle;
}
```

### Critical Points

1. **`DuplicateHandle` injects handles INTO target process** - The returned handle value is only valid in the client's process
2. **Requires `PROCESS_DUP_HANDLE` permission** - Runtime must be able to open Chrome's process
3. **`DUPLICATE_SAME_ACCESS`** - Preserves READ|WRITE access from original handle
4. **The client receives the handle via IPC** - Not a HANDLE value, but a `uint64` that represents the handle in the client's table

---

## Client-Side Texture Import

### Source: `Runtime/Source/Endpoints/Trampoline/Source/OpenXR/GraphicsExtension.cpp`

```cpp
FXRSharedTexture FXRD3D11Extension::OpenSharedTextureHandle_Internal(
    HANDLE Handle,
    FUInt2 Resolution,
    uint16 ArraySize,
    RHI::RHIFormat::FRHIFormat Format,
    XrSwapchainUsageFlags UsageFlags)
{
    FXRSharedTexture Result;

    // CRITICAL: Use ID3D11Device1 for NT handle support
    // ID3D11Device::OpenSharedResource only works with legacy handles
    // ID3D11Device1::OpenSharedResource1 is required for NT handles
    ID3D11Device1* Device = nullptr;
    HRESULT hr = D3DDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&Device);

    if (FAILED(hr))
    {
        Log(FLogOpenXRInterface, Error, "Failed to get ID3D11Device1 interface");
        return {};
    }

    // OpenSharedResource1 is required for NT handles (D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
    hr = Device->OpenSharedResource1(
        Handle,
        __uuidof(ID3D11Texture2D),
        (void**)&Result.RenderTarget);

    if (FAILED(hr))
    {
        Log(FLogOpenXRInterface, Error, "Failed to open shared texture, hr=0x%08X", hr);
        Device->Release();
        return {};
    }

    // Get KeyedMutex for synchronization with runtime
    hr = Result.RenderTarget->QueryInterface(
        __uuidof(IDXGIKeyedMutex),
        (void**)&Result.ShareMutex);

    if (FAILED(hr))
    {
        Log(FLogOpenXRInterface, Error, "Failed to get KeyedMutex for shared texture");
        Result.RenderTarget->Release();
        Result.RenderTarget = nullptr;
        Device->Release();
        return {};
    }

    Device->Release();
    return Result;
}
```

### Important: `OpenSharedResource` vs `OpenSharedResource1`

| Method | Handle Type | Required For |
|--------|-------------|--------------|
| `ID3D11Device::OpenSharedResource` | Legacy `HANDLE` (no NT) | `D3D11_RESOURCE_MISC_SHARED` only |
| `ID3D11Device1::OpenSharedResource1` | NT `HANDLE` | `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` |

**Chrome requires `OpenSharedResource1`** because we use NT handles for `DuplicateHandle` compatibility.

---

## KeyedMutex Synchronization

### Client Side (Trampoline) - Lock Before Rendering

```cpp
void FXRD3D11Extension::LockSharedTexture(FXRSharedTexture Texture)
{
    IDXGIKeyedMutex* Mutex = (IDXGIKeyedMutex*)Texture.ShareMutex;
    if (Mutex != nullptr)
    {
        // Key=0, Timeout=32ms
        HRESULT Result = Mutex->AcquireSync(0, 32);

        if (Result == WAIT_TIMEOUT)
        {
            Log(FLogOpenXRInterface, Warning, "KeyedMutex AcquireSync timeout");
        }
        if (Result == WAIT_ABANDONED)
        {
            Log(FLogOpenXRInterface, Warning, "KeyedMutex was abandoned");
        }
    }
}

void FXRD3D11Extension::UnlockSharedTexture(FXRSharedTexture Texture)
{
    IDXGIKeyedMutex* Mutex = (IDXGIKeyedMutex*)Texture.ShareMutex;
    if (Mutex != nullptr)
    {
        Mutex->ReleaseSync(0);  // Release with key=0
    }
}
```

### Server Side (Runtime) - Lock Before Copying

```cpp
// From RHIHeapD3D11.cpp
void FRHIHeapD3D11::GetShareSync()
{
    ASSERT(ShareMutex != nullptr);
    HRESULT Result = ShareMutex->AcquireSync(0, 32);

    if (Result == WAIT_TIMEOUT)
    {
        Log(FLogRHID3D11, Warning, "KeyedMutex AcquireSync timeout");
    }
    if (Result == WAIT_ABANDONED)
    {
        Log(FLogRHID3D11, Warning, "KeyedMutex was abandoned");
    }
}

void FRHIHeapD3D11::ReleaseShareSync()
{
    ShareMutex->ReleaseSync(0);
}
```

### Synchronization Protocol

```
Client                          Server
  │                               │
  ├─ AcquireSync(0) ────────────► │ (blocked)
  │                               │
  ├─ Render to texture            │
  │                               │
  ├─ ReleaseSync(0) ────────────► │
  │                               ├─ AcquireSync(0)
  │ (blocked)                     │
  │                               ├─ Copy texture
  │                               │
  │ ◄──────────────── ReleaseSync(0)
  │                               │
```

---

## IPC Protocol

### Named Pipe Architecture

SR Hydra uses **4 named pipes** per connection:

| Pipe Name | Direction | Purpose |
|-----------|-----------|---------|
| `{PipeName}_Inbound` | Server → Client | Server-initiated messages |
| `{PipeName}_InboundReturn` | Client → Server | Responses to inbound |
| `{PipeName}_Outbound` | Client → Server | Client-initiated messages |
| `{PipeName}_OutboundReturn` | Server → Client | Responses to outbound |

### Source: `Runtime/Dependencies/WyvernEngine/Source/WyvernEngine/Source/IO/IPC/Source/IPCMessaging.cpp`

```cpp
FIPCMessaging::FIPCMessaging(FString PipeName, FIPCMessageHandler InHandler, bool IsRuntime)
{
    Inbound = GIPCManager.CreateConnection(PipeName + "_Inbound");
    InboundReturn = GIPCManager.CreateConnection(PipeName + "_InboundReturn");
    Outbound = GIPCManager.CreateConnection(PipeName + "_Outbound");
    OutboundReturn = GIPCManager.CreateConnection(PipeName + "_OutboundReturn");

    if (IsRuntime)
    {
        // Runtime creates server-side pipes
        Inbound->CreateServer();
        InboundReturn->CreateClient();
        Outbound->CreateClient();
        OutboundReturn->CreateServer();
    }
    else
    {
        // Client creates client-side pipes
        Inbound->CreateClient();
        InboundReturn->CreateServer();
        Outbound->CreateServer();
        OutboundReturn->CreateClient();
    }
}
```

### IPC Function Definitions

### Source: `Runtime/Source/Shared/OpenXRIPC/Source/ClientRuntimeIPC.json`

```json
{
    "name": "IPC_CreateSwapchain",
    "in": [
        {"name": "Session", "type": "WyvernEngine::uint64"},
        {"name": "Resolution", "type": "WyvernEngine::FUInt2"},
        {"name": "ArraySize", "type": "WyvernEngine::uint8"},
        {"name": "CreateFlags", "type": "XrSwapchainCreateFlags"},
        {"name": "UsageFlags", "type": "XrSwapchainUsageFlags"},
        {"name": "Format", "type": "WyvernEngine::RHI::RHIFormat::FRHIFormat"},
        {"name": "ProcessHandle", "type": "DWORD"}
    ],
    "out": [
        {"name": "Swapchain", "type": "WyvernEngine::uint64"},
        {"name": "Result", "type": "XrResult"}
    ],
    "from": "client",
    "to": "runtime"
}
```

**Note:** `ProcessHandle` is the client's process ID, used for `DuplicateHandle`.

### IPC_GetSwapchainImages - Returns Duplicated Handles

```json
{
    "name": "IPC_GetSwapchainImages",
    "in": [
        {"name": "Swapchain", "type": "WyvernEngine::uint64"}
    ],
    "out": [
        {"name": "ShareHandles", "type": "WyvernEngine::FArray<WyvernEngine::uint64>"},
        {"name": "Result", "type": "XrResult"}
    ],
    "from": "client",
    "to": "runtime"
}
```

The `ShareHandles` array contains handle values valid in the **client's** process space.

---

## Session Management

### Trampoline Session Creation

### Source: `Runtime/Source/Endpoints/Trampoline/Source/OpenXR/Session.cpp`

```cpp
XrResult FXRSession::CreateSession(
    XrInstance InstancePtr,
    const XrSessionCreateInfo* CreateInfo,
    XrSession* OutSessionHandle)
{
    FXRSession* Session = GMemory::New<FXRSession>(InstancePtr);

    // Check for D3D11 graphics binding
    XrGraphicsBindingD3D11KHR* D3D11Bindings = FindInputInChain<XrGraphicsBindingD3D11KHR>(
        CreateInfo, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR);

    if (D3D11Bindings != nullptr && FClientSettings::ClientSettings.AllowD3D11)
    {
        // Create D3D11 graphics extension with the app's device
        Session->GraphicsExtension = GMemory::New<FXRD3D11Extension>(
            Session,
            D3D11Bindings->device);
    }
    else
    {
        // Check for D3D12 or other bindings...
    }

    // Generate session handle for the client
    XrSession SessionHandle = GenerateHandle<XrSession>();

    // Get graphics type string for IPC
    char GraphicsType[32];
    GMemory::Copy<char>(
        Session->GraphicsExtension->ExtensionName.Data(),
        GraphicsType,
        Session->GraphicsExtension->ExtensionName.Length());

    // Forward session creation to runtime via IPC
    XrResult Result;
    IPC_CreateSession(
        Session->GetInstance()->GetMessaging(),
        XRT_CAST_OXR_HANDLE_TO_PTR(uint64, Session->GetInstance()->RuntimePtr),
        XRT_CAST_OXR_HANDLE_TO_PTR(uint64, CreateInfo->systemId),
        SessionHandle,
        GraphicsType,
        Session->RuntimePtr,  // Output: runtime-side session pointer
        Result);

    if (Result == XR_SUCCESS)
    {
        Sessions.Add(Session);
        *OutSessionHandle = SessionHandle;
    }

    return Result;
}
```

### Runtime Session Creation

### Source: `Runtime/Source/Endpoints/Runtime/Source/OpenXR/Session.cpp`

```cpp
void IPC_CreateSession_Handler(
    FIPCMessaging* Messaging,
    uint64 InstancePtr,
    uint64 SystemId,
    XrSession SessionHandle,
    char* GraphicsType,
    uint64& OutSessionPtr,
    XrResult& OutResult)
{
    FXRInstance* Instance = reinterpret_cast<FXRInstance*>(InstancePtr);

    // Create session with appropriate graphics extension
    FXRSession* Session = GMemory::New<FXRSession>(Instance, SessionHandle);

    FString GraphicsTypeStr(GraphicsType);
    if (GraphicsTypeStr == "XR_KHR_D3D11_enable")
    {
        Session->GraphicsExtension = GMemory::New<FXRD3D11Extension>(Session);
    }

    // Create compositor for this session
    Session->Compositor = GMemory::New<FSessionCompositor>(Session);

    // Store session in thread-safe collection
    auto Lock = Sessions.Lock();
    Lock->Add(Session);

    OutSessionPtr = reinterpret_cast<uint64>(Session);
    OutResult = XR_SUCCESS;
}
```

---

## Swapchain Implementation

### Runtime Swapchain Creation

### Source: `Runtime/Source/Endpoints/Runtime/Source/OpenXR/Swapchain.cpp`

```cpp
XrResult FXRSwapchainD3D11::Init(
    FUInt2 Resolution,
    uint8 ArraySize,
    XrSwapchainCreateFlags CreateFlags,
    XrSwapchainUsageFlags UsageFlags,
    RHI::RHIFormat::FRHIFormat Format,
    DWORD ClientProcessId)
{
    this->Resolution = Resolution;
    this->ArraySize = ArraySize;
    this->Format = Format;

    // Determine texture properties from usage flags
    bool IsShaderResource = (UsageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0;
    bool IsColorTarget = (UsageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    bool IsDepthTarget = (UsageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    bool IsUnordered = (UsageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;

    RHI::IRHICommandList* CommandList = GRenderer.GetAdapter()->CreateCommandList();
    CommandList->Reset();

    // Single-buffered: FrameBufferCount = 1
    const uint32 FrameCount = FrameBufferCount;

    FrameTargets.SetLength(FrameCount);
    FrameResources.SetLength(FrameCount);
    ShareHandles.SetLength(FrameCount);

    for (uint32 i = 0; i < FrameCount; i++)
    {
        // Create SHAREABLE render target for client to render into
        // The 'true' parameter enables D3D11_RESOURCE_MISC_SHARED_NTHANDLE | KEYEDMUTEX
        FrameTargets[i] = GRenderer.GetAdapter()->CreateTexture(
            CommandList,
            Format,
            nullptr,  // No initial data
            Resolution.Cast<int32>(),
            IsShaderResource,
            false,    // Not staging
            IsColorTarget,
            true,     // *** SHARABLE = true ***
            IsDepthTarget,
            ArraySize,
            IsUnordered,
            false);   // Not cubemap

        // Duplicate handle for cross-process access
        ShareHandles[i] = GetSharableTextureHandle(FrameTargets[i], ClientProcessId);

        // Create NON-SHARED texture for compositor's local copy
        // Compositor copies FrameTarget -> FrameResource before compositing
        FrameResources[i] = GRenderer.GetAdapter()->CreateTexture(
            CommandList,
            Format,
            nullptr,
            Resolution.Cast<int32>(),
            true,     // Shader resource for compositor
            false,    // Not staging
            false,    // Not render target
            false,    // *** NOT SHARABLE ***
            false,    // Not depth
            ArraySize,
            false,    // Not unordered
            false);   // Not cubemap
    }

    CommandList->Close();
    CommandList->Execute();

    return XR_SUCCESS;
}
```

### Dual-Buffer Pattern Explained

```
┌─────────────────────────────────────────────────────────────────┐
│                        RUNTIME SERVICE                          │
│                                                                 │
│  FrameTargets[]                    FrameResources[]            │
│  ┌─────────────┐                   ┌─────────────┐             │
│  │ D3D11       │  ─── GPU Copy ──► │ D3D11       │             │
│  │ Texture     │  (with KeyedMutex)│ Texture     │             │
│  │ (SHARED)    │                   │ (LOCAL)     │             │
│  └─────────────┘                   └─────────────┘             │
│        ▲                                  │                     │
│        │ DuplicateHandle                  │                     │
│        │                                  ▼                     │
│        │                           Compositor reads             │
│        │                           for weaving                  │
└────────│────────────────────────────────────────────────────────┘
         │
    ─────│─── IPC (handle sent as uint64) ────────────────────────
         │
┌────────│────────────────────────────────────────────────────────┐
│        ▼                        CLIENT (CHROME)                 │
│  ┌─────────────┐                                                │
│  │ D3D11       │ ◄── Opened via OpenSharedResource1             │
│  │ Texture     │                                                │
│  │ (imported)  │ ◄── Chrome GPU process renders here            │
│  └─────────────┘                                                │
└─────────────────────────────────────────────────────────────────┘
```

**Why Dual-Buffer?**
- Client holds `FrameTarget` lock while rendering
- Compositor can't wait indefinitely for client to finish
- Solution: Compositor copies to `FrameResource` during brief sync window
- Compositor then works with local copy without blocking client

---

## Frame Submission Flow

### Complete Flow Diagram

```
Client (Chrome Trampoline)                    Server (Runtime)
         │                                           │
    ┌────┴────┐                                      │
    │xrWaitFrame                                     │
    └────┬────┘                                      │
         │──── IPC_WaitFrame ───────────────────────►│
         │◄─── DisplayTime, PredictedPose ───────────│
         │                                           │
    ┌────┴────┐                                      │
    │xrBeginFrame                                    │
    └────┬────┘                                      │
         │──── IPC_BeginFrame ──────────────────────►│
         │◄─── OK ───────────────────────────────────│
         │                                           │
    ┌────┴────┐                                      │
    │xrAcquireSwapchainImage                         │
    └────┬────┘                                      │
         │──── IPC_AcquireImage ────────────────────►│
         │◄─── ImageIndex ───────────────────────────│
         │                                           │
         ├── LockSharedTexture (KeyedMutex)          │
         │                                           │
    ┌────┴────┐                                      │
    │xrWaitSwapchainImage                            │
    └────┬────┘                                      │
         │──── IPC_WaitImage ───────────────────────►│
         │◄─── OK ───────────────────────────────────│
         │                                           │
    ┌────┴────┐                                      │
    │ RENDER TO TEXTURE                              │
    └────┬────┘                                      │
         │                                           │
    ┌────┴────┐                                      │
    │xrReleaseSwapchainImage                         │
    └────┬────┘                                      │
         ├── UnlockSharedTexture (KeyedMutex)        │
         │──── IPC_ReleaseImage ────────────────────►│
         │◄─── OK ───────────────────────────────────│
         │                                           │
    ┌────┴────┐                                      │
    │xrEndFrame                                      │
    └────┬────┘                                      │
         │──── IPC_AddCompositionLayer ─────────────►│
         │──── IPC_AddCompositionView (Left) ───────►│
         │──── IPC_AddCompositionView (Right) ──────►│
         │──── IPC_ComposeLayers ───────────────────►│
         │                                      ┌────┴────┐
         │                                      │Compositor│
         │                                      │ - Copy   │
         │                                      │ - Weave  │
         │                                      │ - Present│
         │                                      └────┬────┘
         │◄─── OK ───────────────────────────────────│
         │                                           │
```

### Trampoline xrAcquireSwapchainImage

```cpp
XrResult FXRSwapchain::AcquireImage(uint32* OutImageIndex)
{
    XrResult Result;
    uint32 ImageIndex;

    // Request image from runtime
    IPC_AcquireImage(
        GetInstance()->GetMessaging(),
        NativePtr,
        ImageIndex,
        Result);

    if (Result == XR_SUCCESS)
    {
        // Lock ALL framebuffers with KeyedMutex before client renders
        for (FXRSharedTexture& Texture : Framebuffers)
        {
            GetSession()->GraphicsExtension->LockSharedTexture(Texture);
        }

        *OutImageIndex = ImageIndex;
    }

    return Result;
}
```

### Trampoline xrReleaseSwapchainImage

```cpp
XrResult FXRSwapchain::ReleaseImage()
{
    // Unlock KeyedMutex so compositor can access
    for (FXRSharedTexture& Texture : Framebuffers)
    {
        GetSession()->GraphicsExtension->UnlockSharedTexture(Texture);
    }

    XrResult Result;
    IPC_ReleaseImage(
        GetInstance()->GetMessaging(),
        NativePtr,
        Result);

    return Result;
}
```

---

## Compositor Architecture

### Compositor Structure

### Source: `Runtime/Source/Endpoints/Runtime/Source/Compositor.h`

```cpp
struct FSessionCompositor
{
    FXRSession* Session;

    // Output framebuffers (left eye, right eye)
    RHI::IRHIRenderTarget* FrameBuffers[2];

    // Command list for copy and composite operations
    RHI::IRHICommandList* CommandList;

    // Pipeline states for composition
    RHI::IRHIPipelineState* CompositionPipeline[2];  // Blit + Blend modes

    // Constant buffer for composition parameters
    RHI::FRHIConstantBuffer* CompositionData;

    // Current layers to composite
    FArray<FXRCompositionLayer> Layers;
};
```

### Composition Flow

### Source: `Runtime/Source/Endpoints/Runtime/Source/Compositor.cpp`

```cpp
void FSessionCompositor::Compose(FArray<FXRCompositionLayer>& Layers)
{
    GRenderer.Lock.Lock();
    CommandList->Reset();

    // ═══════════════════════════════════════════════════════════════
    // STEP 1: Copy shared textures to local resources (with sync)
    // ═══════════════════════════════════════════════════════════════
    for (FXRCompositionLayer& Layer : Layers)
    {
        for (FXRCompositionView& View : Layer.Views)
        {
            RHI::FRHITexture* Target = View.Swapchain->GetFrameTarget(View.ImageIndex);
            RHI::FRHITexture* Resource = View.Swapchain->GetFrameResource(View.ImageIndex);

            // Acquire KeyedMutex - wait for client to release
            Target->GetHeap()->GetShareSync();

            // Copy client-rendered texture to compositor-local texture
            CommandList->CopyTexture(
                Resource,              // Destination
                View.ImageArrayIndex,  // Dest array slice
                Target,                // Source
                View.ImageArrayIndex); // Source array slice

            // Release KeyedMutex - allow client to acquire again
            Target->GetHeap()->ReleaseShareSync();
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // STEP 2: Render to left/right framebuffers
    // ═══════════════════════════════════════════════════════════════
    for (int ViewIndex = 0; ViewIndex < 2; ViewIndex++)
    {
        CommandList->SetRenderTarget(FrameBuffers[ViewIndex]);
        CommandList->ClearRenderTarget(FFloat4(0, 0, 0, 1));

        // Set viewport for full framebuffer
        CommandList->SetViewport(0, 0,
            FrameBuffers[ViewIndex]->GetResolution().X,
            FrameBuffers[ViewIndex]->GetResolution().Y);

        // Composite each layer
        for (const FXRCompositionLayer& Layer : Layers)
        {
            if (ViewIndex < Layer.Views.Length())
            {
                ComposeView(Layer, Layer.Views[ViewIndex]);
            }
        }
    }

    CommandList->Close();
    CommandList->Execute();

    GRenderer.Lock.Unlock();
}

void FSessionCompositor::ComposeView(
    const FXRCompositionLayer& Layer,
    const FXRCompositionView& View)
{
    // Get the local copy of the texture (NOT the shared one)
    RHI::FRHITexture* Texture = View.Swapchain->GetFrameResource(View.ImageIndex);

    // Set up composition constants (UV mapping, blend mode, etc.)
    FCompositionConstants Constants;
    Constants.UVOffset = View.SubImage.imageRect.offset;
    Constants.UVScale = View.SubImage.imageRect.extent;
    Constants.BlendMode = Layer.BlendMode;
    CompositionData->Upload(&Constants, sizeof(Constants));

    // Bind texture and draw fullscreen quad
    CommandList->SetPipelineState(CompositionPipeline[Layer.BlendMode]);
    CommandList->SetTexture(0, Texture, View.SubImage.imageArrayIndex);
    CommandList->SetConstantBuffer(0, CompositionData);
    CommandList->Draw(6, 0);  // Fullscreen triangle or quad
}
```

### Integration with LeiaSR Weaver

After composition, the stereo framebuffers are sent to the LeiaSR weaver:

```cpp
void FSessionCompositor::Present()
{
    // Left and right framebuffers contain composed stereo images
    ID3D11Texture2D* LeftEye = FrameBuffers[0]->GetTexture();
    ID3D11Texture2D* RightEye = FrameBuffers[1]->GetTexture();

    // LeiaSR weaver performs light field interlacing
    SRWeaver->SetInputViews(LeftEye, RightEye);
    SRWeaver->Weave();
    SRWeaver->Present();
}
```

---

## Key Source Files

### Runtime (Server Side)

| File | Purpose |
|------|---------|
| `Runtime/Source/Endpoints/Runtime/Source/OpenXR/Session.cpp` | Session management, IPC handlers |
| `Runtime/Source/Endpoints/Runtime/Source/OpenXR/Swapchain.cpp` | Swapchain creation, handle duplication |
| `Runtime/Source/Endpoints/Runtime/Source/Compositor.cpp` | Layer composition, texture copy |
| `Runtime/Dependencies/WyvernEngine/.../RHI/RHID3D11/Source/RHIHeapD3D11.cpp` | D3D11 texture creation with share flags |
| `Runtime/Source/Shared/OpenXRIPC/Source/ClientRuntimeIPC.json` | IPC function definitions |

### Trampoline (Client Side)

| File | Purpose |
|------|---------|
| `Runtime/Source/Endpoints/Trampoline/Source/OpenXR/Session.cpp` | Session creation, IPC calls |
| `Runtime/Source/Endpoints/Trampoline/Source/OpenXR/Swapchain.cpp` | Swapchain wrapper, image acquire/release |
| `Runtime/Source/Endpoints/Trampoline/Source/OpenXR/GraphicsExtension.cpp` | D3D11 handle import, KeyedMutex lock/unlock |

### IPC Infrastructure

| File | Purpose |
|------|---------|
| `Runtime/Dependencies/WyvernEngine/.../IO/IPC/Source/IPCMessaging.cpp` | Named pipe setup, message dispatch |
| `Runtime/Dependencies/WyvernEngine/.../IO/IPC/Source/IPCMessaging.h` | IPC messaging interface |

---

## Summary: Key Implementation Requirements

| Requirement | SR Hydra Implementation |
|-------------|------------------------|
| **Texture Creation Flags** | `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` + `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` |
| **Handle Export** | `IDXGIResource1::CreateSharedHandle()` with `DXGI_SHARED_RESOURCE_READ \| WRITE` |
| **Cross-Process Handle** | `DuplicateHandle()` from runtime process to client process |
| **Client Import** | `ID3D11Device1::OpenSharedResource1()` (NOT `OpenSharedResource`) |
| **Synchronization** | `IDXGIKeyedMutex::AcquireSync(0, timeout)` / `ReleaseSync(0)` |
| **Buffer Strategy** | Dual-buffer: FrameTargets (shared) → copy → FrameResources (local) |
| **Frame Count** | Single-buffered (`FrameBufferCount = 1`) |

### Chrome-Specific Considerations

1. **Sandbox compatibility** - Runtime must create textures, not Chrome
2. **Process isolation** - Use `DuplicateHandle` to inject handles into Chrome's process
3. **NT handles** - Required for `DuplicateHandle`, use `OpenSharedResource1` on client
4. **GPU process** - Chrome's D3D11 device is in the GPU process, not renderer
