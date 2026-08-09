# Automation

The scripts use one shared UE4SS runtime cache. Its default location is
`../.ue4ss-runtime` relative to this repository's parent directory. Set the
`UE4SS_RUNTIME_ROOT` environment variable to use a different cache location.

- `setup-ue4ss.ps1` checks out the pinned UE4SS source and builds the shared
  shipping runtime once.
- `build.ps1` builds NearbyCrafting against that shared runtime; it does not
  build UE4SS or the `dwmapi` proxy again.
- `release.ps1` generates the mod description and creates both a lean full archive
  containing the pinned UE4SS runtime, release settings, and only NearbyCrafting, plus
  the standalone mod-only archive.
- `generate-mod-description.ps1` converts the README.md content to
  BBCode at `dist/MOD_DESCRIPTION.md`.
