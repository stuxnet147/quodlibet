# Quodlibet compatibility shim

**Nothing here is vendored.** These files are written by Quodlibet and are not
part of any upstream project. They exist so that the pinned CaDiCaL and
`lrat-check` sources compile unmodified under the `windows-clang` preset, which
targets the MSVC ABI and therefore has no `unistd.h`, `sys/time.h`, or
`sys/resource.h`.

Patching a pinned dependency would make the checksum in `scripts/vendor.sh`
describe something other than what is built. Supplying the four headers the
platform is missing keeps the pin honest: the bytes that were checksummed are
the bytes the compiler reads.

The shim is on the include path of the two vendored executables only. It never
reaches `libquodlibet`, and no Quodlibet source includes it.
