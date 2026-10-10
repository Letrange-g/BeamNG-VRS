# Privacy and security when sharing reports

- Do **not** upload complete Windows environment dumps, saved API credentials, OpenXR application logs, GPU traces, memory dumps, or crash dumps without inspecting them.
- Review game installation paths and SDK paths for account names. Replace these with placeholders in GitHub issues.
- The source is experimental and hooks both Vulkan and OpenXR; test with local process-only layer activation. Avoid system-wide registration.
- Keep the repository private until its owner explicitly decides to publish it and selects a license.
- Before sharing a binary release, reproduce the build from this curated tree and scan the release archive; this repository intentionally contains source only.

Please report regressions with minimal, redacted log excerpts and version/driver information instead of full logs.
