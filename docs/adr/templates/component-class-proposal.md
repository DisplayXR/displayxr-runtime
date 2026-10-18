# Component-class proposal — one-page analysis (ADR-054 Amendment 1)

A new component class is admitted only when **all three** hold. Fill every section; a
proposal that skips the ladder is returned.

## 1. The capability in one sentence

What the runtime would *invoke, schedule, select or arbitrate*. If the runtime never calls
it, stop: it is a consumer (rung 10) or an SDK feature (rung 2).

## 2. The ladder — why each lower rung does not fit

| rung | mechanism | why not (one line each) |
|---|---|---|
| 1 | OS / platform facility | |
| 2 | app or web-SDK feature, no runtime coordination | |
| 3 | runtime policy (trunk) | |
| 4 | internal runtime module (not replaceable) | |
| 5 | data / calibration / configuration profile | |
| 6 | model or asset pack in an existing engine | |
| 7 | new task or role in an existing class | |
| 8 | optional capability on an existing provider | |
| 9 | app-facing OpenXR extension | |
| 10 | external registered consumer (IPC / shared memory) | |
| 11 | independently hosted service, established protocol | |
| 12 | **new class** | |

## 3. The three-part test

1. **Distinct, runtime-managed responsibility** not expressible in an existing class: …
2. **Interchangeable implementations or independently managed domains** with real value,
   *not* already obtained by hosting an existing-class provider in a satellite: …
3. **Vendor-neutral contract specifiable now** — semantics, ownership, selection scope,
   timing, lifecycle, state vocabulary, security and consent, simulated reference: …

Evidence (not criteria): second implementer? separable failure domain?

## 4. Selection scope and hosting (decision 8, §3)

Scope: per screen / per task + stream / per role family + client / other: …
Hosting: in-process (frame-critical, contained) / supervised satellite: …

## 5. Admission deliverables (N4 + Amendment 1)

- [ ] simulated reference provider shipped with the runtime
- [ ] per-class conformance step in CI (ABI-major rejection + fall-through, `Enabled=0`
      fall-back, `Preferred` wins)
- [ ] provenance classification
- [ ] the four verbs in `displayxr-cli` and the Dashboard
- [ ] one cross-class integration test on the route the class touches
- [ ] the same capture trigger on every compositing route the class adds
