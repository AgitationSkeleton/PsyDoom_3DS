#!/usr/bin/env bash
#
# Builds PsyDoom for the Nintendo 3DS: Linux and macOS.
#
# You supply your own PlayStation Doom disc. Nothing copyrighted is in this repository, so the disc image, the game
# data extracted from it and the banner music taken from its audio track are all produced here from your own copy.
#
# Usage:
#   tools/3ds/build.sh --disc-dir <folder with your bin/cue files> [options]
#
# See tools/3ds/README.md for the full explanation.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${REPO_ROOT}/tools/3ds/common.sh"

DISC_DIR=""
EDITIONS="doom final_doom master_edition"
JOBS="$(detect_cpu_count)"
SKIP_CIA=0
FORCE_ROMFS=0

print_usage() {
    cat <<'USAGE'
Build PsyDoom for the Nintendo 3DS.

Required:
  --disc-dir <path>     Folder holding your ripped PlayStation Doom discs.
                        Each edition is found by looking for its .cue file; see tools/3ds/README.md for the names.

Options:
  --edition <name>      Build one edition only: doom, final_doom or master_edition.
                        May be given more than once. Defaults to all three.
  --jobs <n>            Parallel compile jobs. Defaults to the number of processors.
  --skip-cia            Build only the .3dsx, skipping the .cia. Faster, and needs no makerom.
  --force-romfs         Rebuild the compressed disc archive even if one is already there.
  -h, --help            Show this message.

Output goes to dist/<edition>/.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --disc-dir)     DISC_DIR="${2:-}"; shift 2 ;;
        --edition)      EDITIONS_OVERRIDE="${EDITIONS_OVERRIDE:-} ${2:-}"; shift 2 ;;
        --jobs)         JOBS="${2:-}"; shift 2 ;;
        --skip-cia)     SKIP_CIA=1; shift ;;
        --force-romfs)  FORCE_ROMFS=1; shift ;;
        -h|--help)      print_usage; exit 0 ;;
        *)              fail "Unknown option: $1 (try --help)" ;;
    esac
done

if [[ -n "${EDITIONS_OVERRIDE:-}" ]]; then
    EDITIONS="${EDITIONS_OVERRIDE}"
fi

[[ -n "${DISC_DIR}" ]] || fail "No disc folder given. Use --disc-dir <path>; see --help."
[[ -d "${DISC_DIR}" ]] || fail "Disc folder does not exist: ${DISC_DIR}"

require_toolchain
[[ ${SKIP_CIA} -eq 1 ]] || require_packaging_tools

for edition in ${EDITIONS}; do
    validate_edition "${edition}"
done

for edition in ${EDITIONS}; do
    info "==> ${edition}"
    prepare_romfs "${edition}" "${DISC_DIR}" "${FORCE_ROMFS}"
    prepare_metadata "${edition}" "${DISC_DIR}"
    build_edition "${edition}" "${JOBS}" "${SKIP_CIA}"
done

info ""
info "Done. Packages are in dist/:"
for edition in ${EDITIONS}; do
    ls -la "${REPO_ROOT}/dist/${edition}" 2>/dev/null | tail -n +2 || true
done
