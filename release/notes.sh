#!/usr/bin/env bash
# Writes the notes of a release to stdout, as Markdown, from a release zip:
#
#   release/notes.sh <zip> [<previous release>]
#
# <previous release> is the name of the release before this one (openbf2-serverext-<openbf2>-<module>-<bf2hub>-<backend>).
# With it the notes start with the commits added since then, per repository. The rest comes from the zip's source.txt:
# the commits it was built from, the builder, the files with their checksums, and the lines bf2-docker needs to pin
# this zip.
#
# Needs unzip, sha512sum and awk. The commit lists need the GitHub CLI (gh) with read access to the three repositories;
# without it each list is replaced by its compare link.
#
# Environment: MODULE_SLUG (default: GITHUB_REPOSITORY, then nihlen/openbf2-server-ext), OPENBF2_SLUG
# (nihlen/OpenBF2), BF2HUB_SLUG (B2BF/OpenBF2-BF2Hub).
set -euo pipefail

zip=${1:?usage: notes.sh <zip> [<previous release>]}
prev=${2:-}
name=$(basename "$zip" .zip)
MODULE_SLUG=${MODULE_SLUG:-${GITHUB_REPOSITORY:-nihlen/openbf2-server-ext}}
OPENBF2_SLUG=${OPENBF2_SLUG:-nihlen/OpenBF2}
BF2HUB_SLUG=${BF2HUB_SLUG:-B2BF/OpenBF2-BF2Hub}

src=$(unzip -p "$zip" source.txt)
sha512=$(sha512sum "$zip" | cut -d' ' -f1)

# The second word of the source.txt line that starts with $1 (a commit, the domain), and the rest of that line
commit_of() { printf '%s\n' "$src" | awk -v k="$1" '$1 == k { print $2; exit }'; }
rest_of() { printf '%s\n' "$src" | awk -v k="$1" '$1 == k { sub(/^[^ ]+ /, ""); print; exit }'; }

openbf2=$(commit_of openbf2)
module=$(commit_of module)
bf2hub=$(commit_of bf2hub)

# changes <title> <owner/repo> <previous short commit> <commit>
changes() {
    local title=$1 slug=$2 old=$3 new=$4 list
    if [ "${new#"$old"}" != "$new" ]; then
        printf '**%s**: unchanged (`%s`)\n\n' "$title" "${new:0:7}"
        return
    fi
    printf '**%s**, [`%s` to `%s`](https://github.com/%s/compare/%s...%s)\n\n' \
        "$title" "$old" "${new:0:7}" "$slug" "$old" "${new:0:7}"
    if list=$(gh api "repos/$slug/compare/$old...$new" \
            --jq '.commits | reverse | .[] | "- \(.commit.message | split("\n")[0]) (`\(.sha[0:7])`)"' 2>/dev/null) \
            && [ -n "$list" ]; then
        printf '%s\n\n' "$list"
    else
        printf -- '- The commit list is not available here; see the link.\n\n'
    fi
}

echo 'The binaries of an OpenBF2 dedicated server with the server extension, for Linux x86-64 (glibc 2.35 or newer,'
echo 'OpenSSL 3): `bf2`, `libdice_py.so`, `libserverext.so` and `libbf2hub.so`. Unpack the zip into the server'"'"'s'
echo '`bin/amd-64`; the README has the steps.'
echo

if [ -n "$prev" ]; then
    # openbf2-serverext-<openbf2>-<module>-<bf2hub>-<backend>
    IFS=- read -r _ _ prev_openbf2 prev_module prev_bf2hub _ <<< "$prev"
    echo "## Changes since \`$prev\`"
    echo
    changes 'openbf2-server-ext (the module)' "$MODULE_SLUG" "$prev_module" "$module"
    changes 'OpenBF2 (the engine)' "$OPENBF2_SLUG" "$prev_openbf2" "$openbf2"
    changes 'OpenBF2-BF2Hub' "$BF2HUB_SLUG" "$prev_bf2hub" "$bf2hub"
fi

echo '## Built from'
echo
echo '| Source | Commit |'
echo '|---|---|'
echo "| openbf2-server-ext | [\`${module:0:7}\`](https://github.com/$MODULE_SLUG/commit/$module) |"
echo "| OpenBF2 | [\`${openbf2:0:7}\`](https://github.com/$OPENBF2_SLUG/commit/$openbf2) |"
echo "| OpenBF2-BF2Hub | [\`${bf2hub:0:7}\`](https://github.com/$BF2HUB_SLUG/commit/$bf2hub) |"
echo
echo "- Builder: $(rest_of builder)"
echo "- GameSpy backend: \`$(commit_of gamespy-domain)\`"
echo '- The same three commits give the same zip, byte for byte (`release/build.sh`).'
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

echo '## Using it in bf2-docker'
echo
echo 'A server image runs this release only once [bf2-docker](https://github.com/nihlen/bf2-docker) pins it. In each'
echo '`images/netsky-openbf2-*/assets/build/`, replace the previous zip in three files:'
echo
echo '`assets.txt`'
echo '```'
echo "https://github.com/$MODULE_SLUG/releases/download/$name/$name.zip"
echo '```'
echo '`assets.sha512`'
echo '```'
echo "$sha512  $name.zip"
echo '```'
echo '`build.sh`'
echo '```'
echo "OPENBF2_ZIP=\"\$TMP/$name.zip\""
echo '```'
echo
echo '<details><summary>source.txt, as packed in the zip</summary>'
echo
echo '```'
printf '%s\n' "$src"
echo '```'
echo
echo '</details>'
