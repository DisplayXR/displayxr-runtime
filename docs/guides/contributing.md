# Contributing to DisplayXR

We welcome contributions from app developers, runtime contributors, and display vendors.

## General Workflow

1. Fork the repository (external) or create a feature branch off `main` (collaborators)
2. Make your changes and commit
3. Submit a pull request targeting `main`
4. Both CI jobs (Windows + macOS) must pass
5. A maintainer will review your PR

### Code Style

We use **clang-format** (version 11 or newer):

```bash
git clang-format    # Format only your changes (preferred)
```

### Testing

```bash
cd build && ctest
```

Ensure your changes build cleanly on both Windows and macOS. CI runs automatically on PRs.

---

## For App Developers

If you've found a bug or want to improve test coverage:

- **Bug reports**: Include your build environment (OS, compiler, CMake flags) and a minimal reproducer if possible.
- **Test app contributions**: Follow the naming convention `cube_{class}_{api}_{platform}`. See [App Classes](../getting-started/app-classes.md).

## For Runtime Contributors

If you're contributing to the OpenXR state tracker, compositors, or auxiliary code:

- Read [Separation of Concerns](../architecture/separation-of-concerns.md) to understand layer boundaries
- Read the relevant [Architecture Decision Records](../adr/) for context on past decisions
- See [Project Structure](../architecture/project-structure.md) for source tree orientation
- See [Implementing an Extension](implementing-extension.md) for adding OpenXR extensions

Key principles:
- Zero vendor `#ifdef` blocks in compositor or state tracker code ([ADR-003](../adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md))
- Compositor never weaves ([ADR-007](../adr/ADR-007-compositor-never-weaves.md))
- Native compositor per graphics API ([ADR-001](../adr/ADR-001-native-compositors-per-graphics-api.md))

## For Display Vendors

If you're integrating a new display into DisplayXR:

1. Start with the [Vendor Integration Guide](vendor-integration.md) — comprehensive walkthrough
2. Read [Writing a Driver](writing-driver.md) for the basics of the driver framework
3. Review [ADR-003](../adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) and [Separation of Concerns](../architecture/separation-of-concerns.md) for the vendor isolation rules
4. Use sim_display (`src/xrt/drivers/sim_display/`) as a reference implementation

All vendor-specific code lives under `src/xrt/drivers/<vendor>/`. No changes to compositors or state tracker should be needed.

---

## Upstream Monado

This project is a fork of [Monado](https://monado.freedesktop.org/). We selectively cherry-pick upstream fixes using a tiered strategy — see [ADR-009](../adr/ADR-009-upstream-cherry-pick-strategy.md).
