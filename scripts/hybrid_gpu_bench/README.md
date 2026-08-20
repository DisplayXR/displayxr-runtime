# hybrid_gpu_bench — #918 hybrid-GPU weave-placement measurement tools

Purpose-built for `docs/investigations/hybrid-igpu-weave.md`. Windows-only.

- `gpu_loadgen.cpp` — closed-loop synthetic GPU load on a chosen adapter
  (`--adapter=igpu|dgpu|<idx> --duty=<pct> --seconds=<n> --res=WxH`); servoes a
  full-screen ALU pass to the duty target via timestamp-disjoint queries.
- `xbridge_bench.cpp` — D3D11 cross-adapter share capability matrix (six texture
  flavors + fences, both directions, exact HRESULTs) + shared-texture loop bench.
- `xbridge12_bench.cpp` — D3D12 cross-adapter heap benchmark: placed row-major
  `ALLOW_CROSS_ADAPTER` texture, shared fence, pixel-verified;
  `--mode=full|copyonly|sample --paced=0|1`. `copyonly --paced=0` prints bandwidth.
- `gpusample.ps1` — per-process/per-adapter/per-engine GPU busy from
  `\GPU Engine(*)\Running Time` deltas (never `Utilization Percentage`).
  Adapter-LUID names are hardcoded for the reference box — edit `$names`.

`build.bat` locates VS 2022 via vswhere and builds all three tools.
