wld
===
wld is a drawing library that targets Wayland. The [swc Wayland compositor
library](https://github.com/michaelforney/swc) uses wld.

Installing
==========
To build and install wld, simply use:

```Bash
make
make install
```

Various flags may be set in config.mk. You will likely want to compile
using only intel or noveau, not both.
