# Privacy and security when sharing reports

- Do **not** upload complete Windows environment dumps, saved API credentials, OpenXR application logs, GPU traces, memory dumps, or crash dumps without inspecting them.
- Review game installation paths and SDK paths for account names. Replace these with placeholders in GitHub issues.
- The source is experimental and hooks both Vulkan and OpenXR; test with local process-only layer activation. Avoid system-wide registration.
- The owner selected MIT for public distribution. Validate the Windows build and BeamNG/headset behavior before publishing a binary beta.
- Binary releases are built from the recorded source commit and distributed as GitHub Release assets. Generated DLLs, logs and local captures are kept out of the source tree.

Please report regressions with minimal, redacted log excerpts and version/driver information instead of full logs.
