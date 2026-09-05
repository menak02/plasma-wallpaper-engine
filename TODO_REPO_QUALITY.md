# Repo-quality follow-ups

This is a working checklist for the repo-quality gate, not a long-term design doc.

Current status:
- Workflow: `.github/workflows/build-and-test.yml` now has a `repo-quality` job.
- Goal: fail on obvious AI scaffolding comments and on blatantly generic file/class names like `helper.cpp`, `util.h`, `wrapper.h`, `impl.cpp`, `common.h` when they are top-level symbols.
- Allowlist: genuine why-comments that name the relevant subsystem are allowed; add keywords there when a real comment gets flagged.

Open items:
- Re-run the failed run after this commit lands and confirm `repo-quality` passes.
- If the allowlist gets noisy, replace the inline grep with a small script in `scripts/check_repo_quality.sh` and call that from CI.
- If we add more languages later, extend the scan per language instead of making one giant regex.

Not in scope for this pass:
- Full naming audit across the whole repo.
- Removal of legitimate abstractions just because their name contains `util`, `helper`, `manager`, etc. The check targets obviously generic top-level file/class names, not substring matches inside domain names.
