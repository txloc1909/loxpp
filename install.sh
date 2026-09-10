#!/bin/sh
# PLACEHOLDER installer.
#
# The real install and upgrade script is delivered by mission node D5
# (feat/standalone-install). Node D4 only attaches this file to every
# release so the asset name is stable from the first release on.
#
# Until D5 lands, install by hand:
#   1. Download loxpp-<version>-x86_64-linux.tar.gz and SHA256SUMS from
#      https://github.com/txloc1909/loxpp/releases
#   2. Run: sha256sum -c SHA256SUMS
#   3. Extract the tarball and copy loxpp onto your PATH.
echo "loxpp installer placeholder."
echo "The real installer lands in node D5."
echo "See https://github.com/txloc1909/loxpp for manual install steps."
exit 1
