#!/usr/bin/env bash
#
# Puts the GitHub Pages site together and checks its links.
#
#   scripts/assemble-site.sh                      # into build-web/site
#   scripts/assemble-site.sh site --require-annotator
#   scripts/assemble-site.sh --serve              # then http://localhost:8000
#
# Layout:
#   index.html              landing page (landing/)
#   timeline.html           about the desktop timeline app (landing/)
#   laps/, tracks/          recorded laps and track files the landing page loads
#   track_annotator.html    the web annotator, from an emscripten build
#                           (scripts/build-web.sh); skipped with a warning if
#                           it has not been built, unless --require-annotator

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/build-web/site"
require_annotator=0
serve=0
for arg in "$@"; do
    case "$arg" in
        --require-annotator) require_annotator=1 ;;
        --serve) serve=1 ;;
        -*) echo "unknown argument: $arg" >&2; exit 2 ;;
        *) out="$arg" ;;
    esac
done

rm -rf "${out}"
mkdir -p "${out}/tracks"
cp -R "${repo_root}/landing/." "${out}/"
cp "${repo_root}"/tracks/*.json "${out}/tracks/"

check_args=()
annotator="${repo_root}/build-web/apps"
if [[ -f "${annotator}/track_annotator.html" ]]; then
    # The shell page references its .js/.wasm/.data by relative name, so it
    # works from any directory as long as they sit next to it.
    cp "${annotator}/track_annotator.html" \
       "${annotator}/track_annotator.js" \
       "${annotator}/track_annotator.wasm" \
       "${annotator}/track_annotator.data" \
       "${annotator}/track_annotator_favicon.png" \
       "${out}/"
elif [[ "${require_annotator}" == "1" ]]; then
    echo "track_annotator has not been built in ${annotator}" >&2
    exit 1
else
    echo "warning: no web build of track_annotator; its links will 404 (run scripts/build-web.sh)" >&2
    check_args=(--allow-missing track_annotator.html)
fi

# Pages runs the upload through Jekyll otherwise, which drops files it does
# not recognise.
touch "${out}/.nojekyll"

python3 "${repo_root}/scripts/check-site-links.py" "${out}" ${check_args[@]+"${check_args[@]}"}
echo "Assembled ${out}"

if [[ "${serve}" == "1" ]]; then
    echo "Serving http://localhost:8000 (Ctrl-C to stop)"
    cd "${out}"
    exec python3 -m http.server 8000
fi
