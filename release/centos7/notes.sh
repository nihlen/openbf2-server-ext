#!/usr/bin/env bash
# Writes the notes of a CentOS 7 release to stdout, as Markdown, from its zip:
#
#   release/centos7/notes.sh <zip>
#
# The notes say whom the zip is for, point everyone else at the regular release of the same commits, and list what
# the zip's source.txt records. The changes between releases are in the regular release's notes, not here.
#
# Needs unzip and awk. With the GitHub CLI (gh) the regular release is linked if it exists.
#
# Environment: MODULE_SLUG (default: GITHUB_REPOSITORY, then nihlen/openbf2-server-ext), OPENBF2_SLUG
# (nihlen/OpenBF2), BF2HUB_SLUG (B2BF/OpenBF2-BF2Hub).
set -euo pipefail

zip=${1:?usage: notes.sh <zip>}
name=$(basename "$zip" .zip)
regular=${name%-centos7}
MODULE_SLUG=${MODULE_SLUG:-${GITHUB_REPOSITORY:-nihlen/openbf2-server-ext}}
OPENBF2_SLUG=${OPENBF2_SLUG:-nihlen/OpenBF2}
BF2HUB_SLUG=${BF2HUB_SLUG:-B2BF/OpenBF2-BF2Hub}

src=$(unzip -p "$zip" source.txt)

# The second word of the source.txt line that starts with $1 (a commit, the domain), and the rest of that line
commit_of() { printf '%s\n' "$src" | awk -v k="$1" '$1 == k { print $2; exit }'; }
rest_of() { printf '%s\n' "$src" | awk -v k="$1" '$1 == k { sub(/^[^ ]+ /, ""); print; exit }'; }

openbf2=$(commit_of openbf2)
module=$(commit_of module)
bf2hub=$(commit_of bf2hub)

if gh release view "$regular" -R "$MODULE_SLUG" >/dev/null 2>&1; then
    regular_link="[\`$regular\`](https://github.com/$MODULE_SLUG/releases/tag/$regular)"
else
    regular_link="the [regular releases](https://github.com/$MODULE_SLUG/releases/latest)"
fi

echo '**Only for servers on CentOS 7 or another distribution of that age.** Everyone else uses'
echo "$regular_link, built from the same commits. This zip does not start on Debian 12 or Ubuntu 22.04 and newer."
echo
echo 'The binaries of an OpenBF2 dedicated server with the server extension, for Linux x86-64 with glibc 2.17 or'
echo 'newer and the ncurses 5 libraries (`libncursesw.so.5`, `libtinfo.so.5`, `libformw.so.5`): `bf2`,'
echo '`libdice_py.so`, `libserverext.so` and `libbf2hub.so`. Unpack the zip into the server'"'"'s `bin/amd-64`; the'
echo 'README has the steps, which are the same as for the regular release. Replace all four files together: the'
echo 'module does not load into another OpenBF2 build.'
echo
echo "\`libserverext.so\` carries its own OpenSSL ($(commit_of openssl)) because CentOS 7's has no TLS 1.3. It gets"
echo 'OpenSSL fixes only through a new release. This build is tested less than the regular one.'
echo

echo '## Built from'
echo
echo '| Source | Commit |'
echo '|---|---|'
echo "| openbf2-server-ext | [\`${module:0:7}\`](https://github.com/$MODULE_SLUG/commit/$module) |"
echo "| OpenBF2 | [\`${openbf2:0:7}\`](https://github.com/$OPENBF2_SLUG/commit/$openbf2) |"
echo "| OpenBF2-BF2Hub | [\`${bf2hub:0:7}\`](https://github.com/$BF2HUB_SLUG/commit/$bf2hub) |"
echo
echo "- Builder: $(rest_of builder)"
echo "- OpenSSL: $(rest_of openssl)"
echo "- GameSpy backend: \`$(commit_of gamespy-domain)\`"
echo

echo '## Files in the zip'
echo
echo '| File | SHA-256 | Newest glibc symbol | Shared libraries |'
echo '|---|---|---|---|'
# file <name> sha256 <hash> max <GLIBC_x> <GLIBCXX_x or no-GLIBCXX> needs <libraries>
printf '%s\n' "$src" | awk '$1 == "file" {
    libs = ""
    for (i = 9; i <= NF; i++) libs = libs (i > 9 ? ", " : "") $i
    printf "| `%s` | `%s` | %s | %s |\n", $2, $4, substr($6, 7), libs
}'
echo
echo "\`$name-debug.tar.gz\` holds the same binaries with their debug info, for gdb. It is not deployed."
echo

echo '<details><summary>source.txt, as packed in the zip</summary>'
echo
echo '```'
printf '%s\n' "$src"
echo '```'
echo
echo '</details>'
