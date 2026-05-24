# Contributing to DisplayXR

See the full [contributing guide](docs/guides/contributing.md) for workflow, code style, CI expectations, and audience-specific guidance (app developers, runtime contributors, display vendors).

## Quick Reference

1. Fork the repository (external) or create a feature branch off `main` (collaborators)
2. Make your changes and commit; format with `git clang-format` before committing
3. Submit a pull request targeting `main`
4. The Windows CI build must pass (macOS CI is currently disabled — macOS work is verified locally)
5. A maintainer will review your PR

### Working on a PR

CI runs are PR-driven and cost-aware. To keep contributor friction low while keeping the runner bill predictable:

- **Open the PR as a draft while iterating** — CI doesn't run on drafts. Push as many commits as you want for free.
- **Click "Ready for review" when you're ready** — that's the trigger for CI to run.
- **Push fixes after review** — each push runs CI on the latest commit. If you force-push or push rapidly, in-progress runs are cancelled so only the latest commit's CI completes.
- **Doc-only PRs skip CI entirely** (anything matching `**.md`, `docs/**`, `LICENSE`, or `.github/ISSUE_TEMPLATE/**`).
- **Push directly to a feature branch?** No CI fires — branch pushes are free. CI only fires on PRs and `v*` tag pushes.

### Issues

Open issues at [github.com/DisplayXR/displayxr-runtime/issues](https://github.com/DisplayXR/displayxr-runtime/issues). For the **DisplayXR Shell** (the spatial workspace controller), file at [displayxr-shell-releases](https://github.com/DisplayXR/displayxr-shell-releases/issues) — those are forwarded to the private shell repo.

### For Display Vendors

See the [vendor plug-in onboarding guide](docs/guides/vendor-plugin-onboarding.md). Vendors ship a plug-in DLL from their own repo — the runtime discovers it at `xrCreateInstance` and never carries vendor code or SDK identifiers in its link line (ADR-019). Zero compositor changes required. The [Leia SR plug-in](https://github.com/DisplayXR/displayxr-leia-plugin) is the canonical example; new vendors follow the same pattern.

### License

Boost Software License 1.0. By contributing, you agree your code is licensed under BSL-1.0.
