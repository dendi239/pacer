#!/usr/bin/env bash
#
# Builds the track annotator as a web app and, with --serve, hosts it.
#
#   scripts/build-web.sh            # build into build-web/
#   scripts/build-web.sh --serve    # build, then serve on http://localhost:8000
#
# Needs the Emscripten SDK. Point EMSDK at your checkout if it is not in the
# default place:
#
#   git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
#   ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build-web"
emsdk_dir="${EMSDK:-${HOME}/emsdk}"

serve=0
for arg in "$@"; do
    case "$arg" in
        --serve) serve=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v emcmake >/dev/null 2>&1; then
    if [[ -f "${emsdk_dir}/emsdk_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "${emsdk_dir}/emsdk_env.sh" >/dev/null
    else
        echo "emcmake not found and no emsdk at ${emsdk_dir}." >&2
        echo "Install it, or set EMSDK to your checkout. See the header of this script." >&2
        exit 1
    fi
fi

emcmake cmake -S "${repo_root}" -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}" --target track_annotator

echo
echo "Built ${build_dir}/apps/track_annotator.html"

if [[ "${serve}" == "1" ]]; then
    echo "Serving http://localhost:8000/track_annotator.html (Ctrl-C to stop)"
    # A plain file:// open will not work: the browser refuses to fetch the
    # .wasm and .data alongside it.
    cd "${build_dir}/apps"
    exec python3 -m http.server 8000
fi
