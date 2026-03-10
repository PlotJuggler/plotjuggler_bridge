# Session Context

## User Prompts

### Prompt 1

Implement the following plan:

# Fix CI release/upload failures for tag 0.5.0

## Context

Tag 0.5.0 fails on AppImage and Debian release workflows. Conda worked.
The working tree already has partial fixes from the previous planning session — this plan reviews them, identifies a remaining race condition, and adds the final fix.

## Current State of Working Tree

**`appimage.yaml`** (staged) — Major restructure already done:
- Removed per-matrix-job `softprops/action-gh-release` upload
- Adde...

### Prompt 2

continue retagging

### Prompt 3

onitor CI and don't stop working or ask me what to do until all the CI workflows aren't succesful

