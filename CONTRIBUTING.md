# Contributing

## Repo quality (automated)

The `repo-quality` workflow step scans for common AI-telltale patterns in C++ source and headers.
If it fires, treat it as a real cleanup task, not a warning to ignore.

Current checks:
- Comment lines that look like generic scaffolding headers (`// This ...`, `// The ...`, `// Simple ...`, `// Implementation ...`, etc.).
- In the future: unused includes, overly generic names (`helper`/`util`/`wrapper`), and dead scaffolding files.

## Local pre-commit (optional)

You can run the same check locally before pushing:

```bash
grep -RInE "^\s*// (This |The |Simple |Implementation |Forward |Main |Get |Discover |Build |Send |Connect |Parse |Skip|For )" \
  --include='*.cpp' --include='*.h' --include='*.hpp' .
```

## Build / CI notes

- Desktop builds currently require Qt6.
- On Ubuntu, install `qt6-base-dev qt6-tools-dev-tools` plus Vulkan and media deps.
