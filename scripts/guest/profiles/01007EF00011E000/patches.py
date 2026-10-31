# BOTW 1.6.0 (v786432) profile for the vendored nxnative rewriter.
#
# No game-specific cuts yet: the first goal is to run the generic rewrite
# rules (x18, TLS registers, SVC gates, spin loops) unchanged, so the output
# is a faithful transformation of the retail module.  Game-specific patches
# (60 fps interval controller, render scale, ...) require 1.6.0 addresses and
# will be added only after they are located and byte-asserted.
CUTS = {}
CAPABILITIES = {}
