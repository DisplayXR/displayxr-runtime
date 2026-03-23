# Contributing to DisplayXR

See the full [contributing guide](docs/guides/contributing.md) for workflow, code style, CI expectations, and audience-specific guidance (app developers, runtime contributors, display vendors).

## Quick Reference

1. Fork the repository (external) or create a feature branch off `main` (collaborators)
2. Make your changes and commit
3. Format with `git clang-format` before committing
4. Submit a pull request targeting `main`
5. Both CI jobs (Windows + macOS) must pass
6. A maintainer will review your PR

### For Display Vendors

See the [vendor integration guide](docs/guides/vendor-integration.md). All vendor-specific code lives under `src/xrt/drivers/<vendor>/` — zero compositor changes required.
