#!/bin/bash
# Build ioc and install it.
#
#   ./install.sh            into ~/bin
#   ./install.sh DIR        into DIR
#
# Builds from clean and replaces any previous install. Only the binary is
# installed: the helper scripts and builtin.ini are compiled into it, so there
# is nothing else to copy. Run it again after pulling or editing builtin.ini.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
dest=${1:-$HOME/bin}

# From scratch every time: a stale object or generated header from an earlier
# build (another machine's, on a shared checkout) cannot end up in the binary.
# The old install goes first, so a failed build cannot leave it looking current.
rm -f "$dest/ioc"
make -C "$here" clean
make -C "$here"
mkdir -p "$dest"
install -m 755 "$here/ioc" "$dest/ioc"
echo "installed $("$dest/ioc" --version) to $dest/ioc"

# Compare against PATH as given; a ~/bin that is on it under another spelling
# (a symlinked home, say) gets a needless warning, which is harmless.
case ":$PATH:" in
    *":$dest:"*) ;;
    *) echo "warning: $dest is not on your PATH" >&2 ;;
esac
