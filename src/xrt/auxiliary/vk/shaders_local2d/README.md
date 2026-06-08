# #439 Phase 3 — VK Local2D composite shaders (foundation)

GLSL source + pre-compiled SPIR-V headers for the Vulkan masked-2D-over-3D
composite (mirrors the D3D11 `masked_composite_ps` / `local2d_flatten_ps` in
`src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp`).

- `fst.vert` → `fst_vert_spv` — fullscreen triangle, uv [0,1] top-left.
- `masked_composite.frag` → `masked_composite_frag_spv` — `M*weave + (1-M)*twod`
  (or rect-discard path when `use_rect_mask`). bindings: 0=twod, 1=mask, 2=weave.
- `local2d_flatten.frag` → `local2d_flatten_frag_spv` — sample one layer image
  through a `src_rect` push constant.

Regenerate (glslangValidator from the Vulkan SDK):
    glslangValidator -V --target-env spirv1.0 <in> --vn <name>_spv -o <name>.h

WIP: the aux_vk pipeline helper (vk_local2d_composite.{h,c}) that consumes these
+ the vk_native zone-mask API + layer_commit wiring are the remaining steps
(see the D3D11 leg on main as the reference, PR #487).
