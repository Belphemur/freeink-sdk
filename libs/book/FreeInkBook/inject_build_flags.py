# FreeInkBook build hook: with FREEINK_BOOK_EXTERNAL_EXPAT defined (e.g.
# CrossPoint firmware, which builds its own hardened lib/expat), the vendored
# expat is excluded from compilation (src/vendor/expat_*.c compile to
# nothing) — so the vendored third_party/expat include path must go too, or
# every <expat.h> resolves to THIS copy's headers (e.g. 2.8.2) while the
# XML_* definitions come from the host's expat (e.g. firmware 2.7.3). The
# host expat's include path is provided by the consuming project (lib_deps
# entry + our sources' #include <expat.h>, both already in place in
# CrossPoint); we only ever REMOVE ours.
# The vendored path is declared in two ways: library.json build "flags" (-I,
# which land in BUILD_FLAGS / *FLAGS) and any CPPPATH entries, so both are
# filtered.
Import("env")

_PATH_MARK = "third_party/expat"


def _is_external(e):
    for d in e.get("CPPDEFINES", []):
        name = d[0] if isinstance(d, (tuple, list)) else d
        if name == "FREEINK_BOOK_EXTERNAL_EXPAT":
            return True
    return False


def _want(drop_flag):
    return drop_flag is None or not drop_flag.startswith("-I")


def _drop_vendored_expat_path(e):
    for key in ("CPPPATH", "CPPFLAGS", "CFLAGS", "CXXFLAGS", "CCFLAGS", "BUILD_FLAGS"):
        entries = e.get(key, [])
        if not entries:
            continue
        filtered = [
            x for x in entries
            if _want(x) or _PATH_MARK not in str(x)
        ]
        if len(filtered) != len(entries):
            e.Replace(**{key: filtered})


for e in [env, DefaultEnvironment()] + list(env.GetLibBuilders()):
    if _is_external(e):
        _drop_vendored_expat_path(e)
