# Android: adopting an already-connected service socket (`DXR_IPC_FD`)

**Status:** implemented · **Issue:** [#1056](https://github.com/DisplayXR/displayxr-runtime/issues/1056) ·
**Epic:** [#1031](https://github.com/DisplayXR/displayxr-runtime/issues/1031) ·
**Related:** [`ipc-client-classes.md`](ipc-client-classes.md), [`XR_DXR_weave`](../extensions/XR_DXR_weave.md)

## The problem

On Android the client's half of `xrCreateInstance` connects to the runtime service
through **Java**. `ipc_client_connection.c` → `ipc_client_android.cpp` →
`loadClassFromRuntimeApk("org.freedesktop.monado.ipc.Client")` →
`Client.blockingConnect(Context, runtimePackage)`, which

1. asks the runtime's **slot broker** for a satellite compositor slot (#1053),
2. `bindService()`s `MonadoService` / `MonadoServiceSlotN` over AIDL,
3. makes a `ParcelFileDescriptor.createSocketPair()` and hands one end to the
   service with `IMonado.connect()`,
4. returns the other end's fd number to native code.

That needs a `Context`, cross-apk class loading, `<queries>` package visibility
and the right to `bindService` — a poor fit for an **embedder whose rendering
process is not the process that owns its Java world**. The motivating case is
`displayxr-browser` on Android: the OpenXR session that owns the weave has to
live in Chromium's GPU process (`:privileged_process0`), while the Java world,
the package-visibility declarations and the service bindings naturally belong to
the browser process.

## The mechanism

A process that **already holds a connected socket end** can skip the whole Java
path. The connecting process does the ordinary connect once and ships the fd
down its own transport; the adopting process publishes it before
`xrCreateInstance` with either of:

| Route | Call | For |
|---|---|---|
| Explicit | `ipc_client_connection_adopt_fd(int fd)` | an embedder handed the fd at runtime (Mojo `PlatformHandle`, `ParcelFileDescriptor`, `SCM_RIGHTS`) |
| Environment | `DXR_IPC_FD=<n>` | a process handed the fd in its **descriptor table at launch** (Chromium's `FileDescriptorInfo` / `base::GlobalDescriptors` shape), and test harnesses |

Both are consumed **once**, by the next `ipc_client_connection_init()`. When one
fires, `ipc_client_android_create()` is never called: no `Context` use for IPC, no
`bindService`, no AIDL, no `org.freedesktop.monado.ipc.*` in the adopting process.

The fd is validated (open · socket · `AF_UNIX` · `SOCK_STREAM`/`SOCK_SEQPACKET` ·
connected) and **duped** — the caller keeps ownership of what it passed — then
marked `FD_CLOEXEC`.

`ipc_client_connection_adopt_fd` is exported from the runtime library
(`libopenxr.version`), so an embedder may `dlsym(RTLD_DEFAULT, …)` it. Note the
ordering: the symbol only resolves **after the loader has loaded the runtime
`.so`**, which any OpenXR call does — e.g. a throwaway
`xrEnumerateInstanceExtensionProperties()`. `DXR_IPC_FD` has no such ordering
constraint. On Android `setenv()` is visible to the runtime because a process has
exactly one bionic libc; the Windows static-CRT caveat around `XRT_FORCE_MODE`
does not apply.

## What adoption does *not* remove

- **The Khronos loader still runs in the adopting process.** Runtime discovery is
  a `ContentProvider` query against `org.khronos.openxr.runtime_broker` plus
  `createPackageContext(runtimePkg, CONTEXT_INCLUDE_CODE)` and a `System.load` of
  the runtime `.so` out of the runtime apk. That process therefore still needs a
  `Context`, the `<queries>` entries, and the ability to load cross-apk code. The
  runtime has to *be* code in the process that calls OpenXR; only the **service
  connection** is what adoption moves.
- **`android_instance_base_init()` still wants a VM + Context** (it takes a global
  ref and installs lifecycle callbacks). Any `Context` will do — a `Service`
  works; it is never cast to `Activity`. It used to *fail* for one, though:
  `android_lifecycle_callbacks_create()` returns NULL for a non-Activity Context
  and `android_instance_base_init()` turned that into `XRT_ERROR_ALLOCATION`, so
  `xrCreateInstance` returned `XR_ERROR_RUNTIME_FAILURE` in any Service-hosted
  client. Fixed with #1056: a NULL container is expected, and everything
  downstream already handled it.

## Identity: who does the server think the client is?

This is the sharp edge, and it is **not** symmetric.

| Check | Where | Sees |
|---|---|---|
| `SO_PEERCRED` peer pid (#954) | `ipc_server_peer_creds.c` | the process that called **`socketpair()`** — i.e. the *connector*, not the adopter. `SO_PEERCRED` is fixed at `socketpair()`/`connect()` time and does not follow the fd. |
| `describe_client` claimed pid | `ipc_server_handler.c` | the **adopter**'s `getpid()` → logs a "claims pid X but OS-derived peer pid is Y" WARN. Diagnostic only; the claim was already non-authoritative since #954. |
| `verify_client_class` (#960) | `ipc_server_handler.c` | `PRESENT_OWNER` and `RELAY` are *verified by use*, so no pid/exe check runs for a weave client. `CONTROLLER`/`DIAG`/`PROVIDER_HOST` **are** pid-keyed and would be attributed to the connector. |
| `Binder.getCallingUid()` | `MonadoImpl.packageDeclaresOverlay`, `SlotBroker.resolveCallerPackage` | the **app uid** — identical for every process of one app, so unaffected. |
| Slot ownership + death link | `Client.java` `slotToken`, `SlotBroker.Owner.pid` | the **connector**. Its death frees the slot; the adopter's does not. |

Consequences to design around:

- A `PRESENT_OWNER` (the browser case) is unaffected — nothing it needs is
  pid-keyed. **Do not** adopt a connection for a `CONTROLLER` or `DIAG` client
  without first making those checks follow the adopter.
- Slot ownership living with the connector is **correct** for a browser: the
  browser process outlives GPU-process crashes, so the satellite slot survives a
  GPU restart. Reconnect = a new socketpair from the browser process and a fresh
  fd handoff; the slot never moves.
- The connector must **retain** its `Client` instance for as long as the adopter
  holds the session. Dropping it unbinds the service and releases the slot.
- Use the **application** `Context` for `blockingConnect`, not an `Activity`:
  `Client.java` only attaches a `MonadoView` / `SystemUiController` when given an
  `Activity`, and a present-owner must keep its own surface.

## Sandbox notes (Chromium)

- Chromium's Android **GPU** process is `:privileged_process0` — non-isolated,
  the app's uid, with a `Context` and binder. The Linux seccomp-bpf hook in
  `content/gpu/gpu_main.cc` is behind `BUILDFLAG(USE_ZYGOTE)`, which is false on
  Android, so no bpf filter is installed there.
- Even under Chromium's strictest Android policy (`BaselinePolicyAndroid`, the
  *renderer* one), what adoption needs is permitted: `sendmsg`/`recvmsg`,
  `mmap`, `dup`, `fcntl`, `close`, `getsockopt`. Passing an `AHardwareBuffer`
  with `AHardwareBuffer_{send,recv}HandleToUnixSocket` is `sendmsg`/`recvmsg`
  with an `SCM_RIGHTS` control message — **seccomp-bpf cannot inspect cmsg
  payloads**, so it is allowed exactly when `sendmsg` is.
- What such a policy *does* block is `socket()`/`connect()`/`socketpair()`.
  That is precisely why adoption is the right shape: the adopting process never
  creates a socket, it only reads and writes one it was given.
- Nothing on the client path `execve`s, `fork`s or opens a file outside the app
  dir. The shared memory is not `shm_open`ed by the client — it arrives as an fd
  over the socket and is only `mmap`ed.

## Testing

`test_apps/weave/weave_client_vk_android` simulates the split when
`debug.dxr.fdhandoff` is set:

```bash
adb shell setprop debug.dxr.fdhandoff 1
adb shell am start -n com.displayxr.weave_client_vk_android/.MainActivity
adb logcat -s dxr-weave:W dxr-fd-connector:I dxr-weave-gpu:I
```

The `MainActivity` process connects (Java, `Context`, `bindService`, cross-apk
class load) and hands the socket **and its `Surface`** to `WeaveGpuService` in
`android:process=":gpu"`, which owns the OpenXR instance, session, Vulkan device
and the `xrWeaveSubmitDXR` loop — while never calling `bindService` and never
loading `org.freedesktop.monado.ipc.*`. `debug.dxr.fdhandoff.env 1` forces the
environment route instead of the exported entry; `debug.dxr.fdhandoff 0` restores
the ordinary in-process path.

Measured on an NP02J (out-of-process runtime, CNSDK services 0.10.61, 2560×1412,
two rects), both routes:

| | |
|---|---|
| Connect in the Activity process (broker + `bindService` + socketpair) | 283–372 ms |
| Adopt → first woven frame in `:gpu` (incl. Vulkan device, swapchain, AHB) | 182–205 ms |
| Steady-state `xrWeaveSubmitDXR` | 5.5–6.4 ms avg |

The in-process #1036 baseline is 5.8–6.7 ms, so the handoff costs nothing per
frame — as it should: after adoption the two shapes run identical code.
