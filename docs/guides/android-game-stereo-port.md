# Porting a native Android game to the weave extension

How a game that was never written for 3D gets real two-eye rendering on an Android 3D display,
in-process, with look-around. Derived from one shipped port (a GLES deferred renderer, two
devices) and from the stereo path for desktop D3D11 titles. Companion to the
[`XR_DXR_weave`](../specs/extensions/XR_DXR_weave.md) spec and the
[Android build guide](../getting-started/android-build-guide.md).

A native game gives you no engine source and no stable symbol names. What it always gives you is
the graphics API (GLES or Vulkan), the window swap, and a camera that ends up as numbers in
uniforms or uniform buffers. That is enough.

## The pipeline

| Layer | Job | Generic mechanism | Per-game part |
|---|---|---|---|
| 1. Entry | get code into the process | a mod loader that preloads native libraries; otherwise a GLES/Vulkan layer or a launcher that preloads | loader glue |
| 2. Frame | find the frame and the window | hook `eglSwapBuffers` / `eglSwapBuffersWithDamage` (`vkQueuePresentKHR`); present from whichever thread swaps | none |
| 3. Two eyes | render the second eye | multiview (`GL_OVR_multiview2` / Vulkan multiview) first; per-pass replay second; per-draw doubling last | which passes stay mono |
| 4. Eye offset | move each eye's camera | clip-space injection in every vertex shader, or patch the camera uniforms | camera uniform names, if any |
| 5. Camera-dependent inputs | keep everything that depends on the camera consistent with the eye | numeric classification of uploaded matrices against the frame's camera (below) | which passes ignore the matrix |
| 6. Look-around | follow the viewer's head | the weave returns tracked eye positions every frame; they set the eye offset | gain |
| 7. Weave | interlace for the panel | the weave transport: side-by-side in, woven image and eye positions out | none |
| 8. Policy | resolution, 2D/3D, convergence | shared defaults (view-scale resolution, auto-2D, convergence, separation gain) | defaults |

## The twin rule

Every non-square, screen-sized render target gets a **right-eye twin**, and so does every
framebuffer that draws into one, including the window. Each draw is replayed into the twin with
the right eye's camera. Everything else (shadow maps, lookup tables, mip pyramids, square
targets) stays shared.

**A twin must inherit everything the game puts into the original outside the per-frame
draws.** Measured failures, each of which showed as a one-eye artifact:

- a depth texture the game clears **once at creation** and never per frame (sampled later by a
  reflection pass): the twin started uninitialized, so the right eye treated the sky as geometry;
- CPU uploads (`glTexSubImage2D`) and GPU copies (`glCopyImageSubData`,
  `glCopyTexSubImage2D`, blits) into an original: mirror them onto the twin;
- a camera-aligned 3D volume written by compute (clustered lights, volumetric fog): twin it and
  run the dispatch twice, or seed it from the left eye's result per frame.

So: **seed every new twin from its original**, with a framebuffer blit at the frame boundary
(a copy in the middle of a pass stalls the GPU behind the game's own work), and mirror every
upload and copy afterwards.

**Mod-issued copies run under the game's raster state.** A blit is clipped by the scissor box,
a clear by the write masks. The ported game leaves a 1024-pixel UI scissor enabled at the
frame boundary; with it on, only the first 1024 columns of each twin were seeded and the right
eye showed a faint vertical line with a different sky right of it. Save, neutralize and restore
the scissor (and the masks, for clears) around every copy the mod makes.

## Layers 4 and 5: one transform, applied everywhere the camera appears

Moving an eye by `e` (in camera units) with the convergence plane held fixed is, in clip space,

```
E = Shift(e) · P · T(-e) · P⁻¹
```

where `P` is the frame's projection, `T(-e)` translates the camera, and `Shift` re-centers the
convergence plane (`clip.xy += s · clip.w`, `s = e · P00 / conv`, `e · P11 / conv`). Keep the
convergence as a camera parameter: shifting the finished eye images instead makes the picture
edges move with the head under look-around and takes the HUD off the screen plane.

Every input that depends on the camera has to see the same eye:

| Input | What breaks if left alone | Fix |
|---|---|---|
| forward camera (`VP`, `P`) | no stereo | `E · VP`, or the vertex-shader injection |
| inverse camera / inverse projection (deferred lighting, shadows, screen-space reflections rebuild positions from depth) | shadows and lighting computed for the unmoved camera: they sit at the wrong depth | `M · E⁻¹`; closed form keeps the z and w rows bit-exact |
| previous-frame camera (TAA, motion vectors) | ghosting | `E · VP_prev`, `M_prev · E⁻¹` |
| camera world position (view vectors for specular, fog) | highlights fixed to the center view | `pos += e.x · right + e.y · up`, axes from the view matrix |
| world-space matrices (model, bones, shadow-map projections) | nothing | leave alone |

**Classifying without names.** Once the frame's camera is known (the perspective test on
uploaded 4x4s: `|m[11]| ≈ 1`, `m[15] ≈ 0`), any uploaded matrix `M` can be tested:
`M · VP ≈ I` is an inverse camera, `M · P ≈ I` an inverse projection, `M ≈ VP` within a few
percent a previous-frame or jittered camera, otherwise unrelated. The same test applies to
16-float aligned runs in uniform-buffer uploads.

**Read how each shader uses the matrix.** The ported game's screen-space passes rebuilt
view-space position with a hand-written shortcut that read only four elements of the inverse
projection, so the per-eye matrix had no effect on shadows, fog and sky. Rewriting the shortcut
into the full multiply at `glShaderSource` fixed all three. Dump every shader that mentions an
inverse camera matrix and check how it reads it: a selective read is a hole the eye cannot
pass through.

**Some passes must keep the center camera.** The reflection pass in the ported game reads a
depth buffer and an environment record that are both center-view; giving it per-eye matrices
re-broke the sky edge. Decide per pass by measurement (block-match the pass's output between
the eyes against the geometry's disparity), not by assumption.

## The frame boundary and the game's lifecycle

- **Games present from more than one thread.** The ported game swaps from a loading thread
  and a render thread. Anything the hooks share must be thread-safe, or a stall on one thread
  freezes the other.
- **The game replaces its GL context when backgrounded.** Every object name is reissued,
  programs relinked. Compare the thread's current EGL context with the one last presented on;
  on a change, forget every GL object the mod and the transport own, keep the records that the
  game is about to refresh (framebuffer attachment lists, program marks), rewrap the runtime's
  woven buffer on the new context (the runtime hands it over once), and retry twin builds that
  fail in the first frames after the restart instead of marking them mono for good.
- **Match the loader's SDK exactly.** A preloader built against a different tag than the
  launcher ships crashed the game at mod registration (struct layout).
- **Head tracking needs a foreground camera grant.** The runtime inherits capabilities from
  its client's binding. When the client is a helper app, the helper must hold a camera-type
  foreground service and the game must bind to it; some power managers freeze a background
  helper, so start it right before the game.
- **Without a face the vendor goes flat** in MANAGED eye-tracking mode. Ask for MANUAL for a
  weave-only client (#2532) and feed the last good eye positions.
- **Tracking costs frame rate on phone-class devices**: a CPU face tracker shares the cores
  with the game (measured: about half the frame rate with a face tracked). A thermal-aware
  tracking cap is a platform item, not a per-app one.

## Diagnosis method that worked

Fake the head position through a debug property to reproduce a head-dependent artifact
without a face. Take the buffer the suspect pass writes, divide by albedo where needed, and
block-match it against the albedo between the eyes: the feature is correct where its disparity
equals the geometry's. Switch patches off one at a time and re-measure the same buffer to learn
which uniforms a pass actually reads. A step at exactly zero eye offset in one eye is a
sign-sensitive path (a division or a comparison on the offset), not a scale error.

What did not work: row-wise edge detectors (lock onto silhouettes), global block statistics
(dominated by wall shading), and reasoning from the shader source alone (the engine's
`u_invProj` story above).

## What is reusable, what is per game

The hooks, the per-thread binding shadow, the twins and their seeding, the replay, the camera
classification and patch, the shader rewrite hook, the frame boundary with context-change
reset, and the weave transport are game-independent: about 85 % of the first port by line
count. Per game: which passes stay mono or keep the center camera, the names of any camera
uniforms the engine exposes, the shader patterns to rewrite, and the loader glue.
