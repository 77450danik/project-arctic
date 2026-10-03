# Sourced by the builds that use ccache, with the same settings in CI and
# locally. The trees are unpacked anew for every build but always at the same
# path, so the fresh timestamps of their headers must not keep a result from
# being reused; the compiler is told apart by its content, not its date.
export CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/ccache}
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-5G}
export CCACHE_SLOPPINESS=include_file_mtime,include_file_ctime,time_macros,locale
export CCACHE_COMPILERCHECK=content
mkdir -p "$CCACHE_DIR"
