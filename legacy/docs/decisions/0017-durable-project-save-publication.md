# 0017 — Durable project-file publication

**Status:** accepted
**Date:** 2026-07-17
**Scope:** canonical `.ntpacker_project` Save / Save As / create-only Save

## Context

Writing the destination with `fopen("wb")` can expose a truncated project after
a short write, process failure, or storage error. Atomic rename alone is also
insufficient: the temporary file must be durable before publication, and a
directory-sync failure occurs after the visible namespace change. Returning a
generic failure in that last case would invite a blind retry even though the new
file is already authoritative.

## Decision

Project Save uses one UTF-8 filesystem backend and this fixed order:

1. serialize and fingerprint canonical v5 bytes;
2. create a unique sibling temporary file exclusively;
3. write all bytes, sync the file, and close it successfully;
4. for optimistic Save, recheck the expected destination fingerprint;
5. atomically replace the destination, or atomically create it without clobber;
6. sync the containing directory where the platform exposes that primitive.

Every failure before step 5 leaves the destination unchanged and leaves live
project path state unadopted. A failure at step 6 returns
`TP_STATUS_FILE_DURABILITY_UNCERTAIN`: publication already happened, the new
fingerprint and saved path are authoritative, and session dirty/identity/event
state advances exactly as on ordinary success. CLI and GUI surface a structured
warning; they do not retry as though no write occurred.

On Windows, `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` is the publication
durability primitive. On POSIX, the implementation syncs the sibling file and
then the opened parent directory. Create-only publication must use an atomic
no-replace primitive; link/unlink emulation is not allowed.

## Executable evidence

- file-sync failure keeps the existing destination byte-identical;
- parent-sync failure reports the published file and fingerprint;
- the session advances identity, saved baseline, lease, fingerprint and Saved
  event for the post-publication warning;
- a safe subsequent Save succeeds without a false external-change conflict;
- UTF-8 and long-path filesystem tests cover the shared backend.

## Consequences

The save status is not a simple success/failure boolean. Frontends must inspect
the save receipt's notices. The project file is protected from partial
publication; storage and power-loss behavior is explicit instead of being
silently overclaimed.
