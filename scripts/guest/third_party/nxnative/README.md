# Vendored nxnative rewriter

Source: https://github.com/seedds/nxnative (MIT, see LICENSE).
Snapshot taken 2026-10-31 for the botw-iPhone guest-core toolchain.

Files: nso.py, nso_asm.py, nso_code.py, nso_match.py, nso_syms.py,
patch_nso.py.

Local changes:
- sdk_patches.py is a trimmed replacement containing only the 1.6.0 SDK
  build's three AddressRegionManager cuts; SDK-wide MATCH_CUTS is empty.
- No game data, keys or title-derived binaries are included.

Do not add game-specific patches to patch_nso.py itself.  They belong in
scripts/guest/profiles/<title id>/patches.py, keyed by the full NSO build ID,
with the original bytes asserted.
