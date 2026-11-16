# Incremental build cache

```sh
ninja -C build -t recompact
```

Use this supported Ninja repair when repeated builds report
`premature end of file; recovering` and recompile unchanged objects. Stop all
build processes first and copy `build/.ninja_deps` and `build/.ninja_log` into an
experiment folder before repair. Only one Ninja process may write a build tree.

The observed corrupt cache contained a duplicate path record for
`include/game/remote_jump.hpp`: expected path ID 3019, stored ID 3016, existing
path ID 3018. Ninja retained old dependency timestamps despite newer object
files. Recompaction rebuilt a valid cache from its readable records.
[Ninja's dependency-log reader](https://github.com/ninja-build/ninja/blob/master/src/deps_log.cc)
checks path IDs to detect conflicting concurrent writes; its generic recovery
warning also covers these record errors.

After repair, build requested targets once to refresh stale dependencies. Run
the same command again and inspect output: there should be no C++ compilation
or linking without source changes. This project has always-run version, opcode
and shader-copy targets, so an incremental invocation can still list these
commands. `ninja -C build -d explain -n wowee` identifies remaining dirty edges.
