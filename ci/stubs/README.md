# Host-side syntax-check stubs

These are **not** the PlayStation 4 SDK headers and must never be used to build
a real package. They declare just enough of the OpenOrbis SDK surface that the
project's own `.c` files can be parsed by a host clang, so CI can run
`-Wall -Wextra` over them without the toolchain installed.

They catch the things a parser can see on its own: wrong `printf` argument
types, uninitialised locals, unreachable labels, mismatched call signatures.
They cannot catch anything that depends on the real SDK's struct layouts or
constants.

If CI fails here but the real build is fine, the stub is out of date -- widen
the stub, don't weaken the check.
