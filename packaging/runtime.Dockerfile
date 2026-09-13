# The ghcr runtime image: the statically linked release binary on a scratch
# base. The musl build has no dynamic dependencies and no TLS stack, so a
# scratch image needs no libc, no shell, and no CA certificates.
#
# The build context is dist/loxpp/ (see release.yml and ci.yml), which
# build_release.sh lays out with the stripped binary plus licences. Only the
# binary is copied: the image stays as small as the tarball's runtime payload.
FROM scratch
COPY loxpp /loxpp
ENTRYPOINT ["/loxpp"]