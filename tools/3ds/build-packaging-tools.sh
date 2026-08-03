#!/usr/bin/env bash
#
# Builds makerom and bannertool into tools/3ds/bin.
#
# These two turn a built executable into an installable .cia, and they are not part of devkitPro, so they have to come
# from somewhere. Rather than ship binaries this builds them from source, once. If you already have them on PATH this
# is never run, and if you only want the .3dsx you do not need them at all - pass --skip-cia to the build script.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_DIR="${REPO_ROOT}/tools/3ds/bin"
WORK_DIR="${REPO_ROOT}/tools/3ds/.build"

MAKEROM_REPO="https://github.com/3DSGuy/Project_CTR.git"
BANNERTOOL_REPO="https://github.com/Steveice10/bannertool.git"

info() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

for tool in git make cc; do
    command -v "${tool}" >/dev/null 2>&1 || fail "'${tool}' is needed to build the packaging tools but is not installed."
done

mkdir -p "${BIN_DIR}" "${WORK_DIR}"

# ------------------------------------------------------------------------------------------------------------------
# makerom
# ------------------------------------------------------------------------------------------------------------------
if [[ ! -x "${BIN_DIR}/makerom" ]]; then
    info "Building makerom..."

    if [[ ! -d "${WORK_DIR}/Project_CTR" ]]; then
        git clone --depth 1 "${MAKEROM_REPO}" "${WORK_DIR}/Project_CTR"
    fi

    make -C "${WORK_DIR}/Project_CTR/makerom" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    cp "${WORK_DIR}/Project_CTR/makerom/bin/makerom" "${BIN_DIR}/makerom"
    chmod +x "${BIN_DIR}/makerom"
fi

# ------------------------------------------------------------------------------------------------------------------
# bannertool
# ------------------------------------------------------------------------------------------------------------------
if [[ ! -x "${BIN_DIR}/bannertool" ]]; then
    info "Building bannertool..."

    if [[ ! -d "${WORK_DIR}/bannertool" ]]; then
        git clone --depth 1 --recursive "${BANNERTOOL_REPO}" "${WORK_DIR}/bannertool"
    fi

    make -C "${WORK_DIR}/bannertool" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

    if [[ -f "${WORK_DIR}/bannertool/output/linux-x86_64/bannertool" ]]; then
        cp "${WORK_DIR}/bannertool/output/linux-x86_64/bannertool" "${BIN_DIR}/bannertool"
    elif [[ -f "${WORK_DIR}/bannertool/output/osx-x86_64/bannertool" ]]; then
        cp "${WORK_DIR}/bannertool/output/osx-x86_64/bannertool" "${BIN_DIR}/bannertool"
    else
        found="$(find "${WORK_DIR}/bannertool/output" -type f -name 'bannertool*' | head -n 1)"
        [[ -n "${found}" ]] || fail "bannertool built but its output could not be found."
        cp "${found}" "${BIN_DIR}/bannertool"
    fi

    chmod +x "${BIN_DIR}/bannertool"
fi

info "Packaging tools are in ${BIN_DIR}."
