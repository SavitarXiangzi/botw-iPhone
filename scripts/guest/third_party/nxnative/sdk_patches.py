# Trimmed for botw-iPhone.  The full upstream file also carries patches for
# many other titles; this copy keeps only the Nintendo SDK build used by
# The Legend of Zelda: Breath of the Wild 1.6.0 (v786432), plus an empty
# SDK-wide MATCH_CUTS table.  Upstream: nxnative, MIT (see LICENSE).
#
# The three cuts below are the upstream "AddressRegionManager" fixes for a
# 32-bit-ish host address space.  They are asserted byte-for-byte by
# patch_nso.py; a different SDK build fails the rewrite instead of silently
# applying the wrong offsets.
CUTS = {
  # The Legend of Zelda: Breath of the Wild 1.6.0 (v786432) sdk.
  # The node area is 0x3fe00000 (about 1 GB) at the start of the alias region;
  # on a host with a ~4.4 GB address space it leaves too little room.  Cut it
  # to 0x3e00000 (62 MB) and align regions of 1 GB or more to 32 MB, as the
  # SDK already does for regions from 32 MB up.
  'fcf93a41b08c5193a1410d262602dc62': [
    (0x32c460, 'e9230b32', '097ca052', 'AddressRegionManager constructor: node area limit orr w9,wzr,#0x3fe00000 -> mov w9,#0x3e00000'),
    (0x32c500, 'e9230b32', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region orr w9,wzr,#0x3fe00000 -> mov w9,#0x3e00000'),
    (0x32c66c, 'ea030232', '0a40a052', 'AllocateAddressRegion: alignment of regions of 1 GB and more orr w10,wzr,#0x40000000 -> mov w10,#0x2000000 (32 MB)'),
  ],
}

# No SDK-wide function-match rules are enabled in this trimmed copy.  Exact
# build cuts above are the only code patches applied to the 1.6.0 SDK.
MATCH_CUTS = {'sdk': []}
