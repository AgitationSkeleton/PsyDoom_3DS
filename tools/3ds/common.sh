#!/usr/bin/env bash
#
# Shared machinery for the 3DS build scripts. Not meant to be run on its own.
#
# Everything here works from your own copy of the game: the disc image is read from a folder you point at, the game
# data is compressed out of it into a RomFS archive, and the banner music is cut from its audio track. None of that
# is in this repository and none of it is redistributed.

# ----------------------------------------------------------------------------------------------------------------
# Output helpers
# ----------------------------------------------------------------------------------------------------------------
info() { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

detect_cpu_count() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

# ----------------------------------------------------------------------------------------------------------------
# What each edition is called, and how to recognise its disc
#
# The cue file names are the ones the usual redump rips use. If yours are named differently, either rename them or
# pass the folder containing just the one disc you want to build.
# ----------------------------------------------------------------------------------------------------------------
edition_file_stem() {
    case "$1" in
        doom)           echo "PsyDoom-Doom" ;;
        final_doom)     echo "PsyDoom-Final-Doom" ;;
        master_edition) echo "PsyDoom-Master-Edition" ;;
    esac
}

edition_title() {
    case "$1" in
        doom)           echo "PsyDoom" ;;
        final_doom)     echo "PsyDoom Final Doom" ;;
        master_edition) echo "PsyDoom Master Edition" ;;
    esac
}

edition_product_code() {
    case "$1" in
        doom)           echo "CTR-H-PDM1" ;;
        final_doom)     echo "CTR-H-PDM2" ;;
        master_edition) echo "CTR-H-PDM3" ;;
    esac
}

edition_unique_id() {
    case "$1" in
        doom)           echo "0xD0010" ;;
        final_doom)     echo "0xD0011" ;;
        master_edition) echo "0xD0012" ;;
    esac
}

# The name the game looks for inside the archive. A rip called anything else is fine: it is packed under this.
edition_cue_name() {
    case "$1" in
        doom)           echo "Doom.cue" ;;
        final_doom)     echo "FinalDoom.cue" ;;
        master_edition) echo "PSXDOOM_BETA_4.cue" ;;
    esac
}

# Patterns matched case insensitively against cue file names to work out which disc is which
edition_cue_pattern() {
    case "$1" in
        doom)           echo "doom" ;;
        final_doom)     echo "final" ;;
        master_edition) echo "master" ;;
    esac
}

validate_edition() {
    case "$1" in
        doom|final_doom|master_edition) ;;
        *) fail "Unknown edition '$1'. Valid values: doom, final_doom, master_edition." ;;
    esac
}

# ----------------------------------------------------------------------------------------------------------------
# Prerequisites
# ----------------------------------------------------------------------------------------------------------------
require_toolchain() {
    [[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set. Install devkitPro with the 3ds-dev package, then open a devkitPro shell or export DEVKITPRO=/opt/devkitpro."
    [[ -d "${DEVKITPRO}/devkitARM" ]] || fail "No devkitARM under ${DEVKITPRO}. Install the 3ds-dev package."
    [[ -f "${DEVKITPRO}/cmake/3DS.cmake" ]] || fail "Missing ${DEVKITPRO}/cmake/3DS.cmake. Update devkitPro."

    command -v cmake >/dev/null 2>&1 || fail "cmake is not installed."
    command -v ffmpeg >/dev/null 2>&1 || fail "ffmpeg is not installed. It is used to cut the banner music from your disc's audio track."
    command -v python3 >/dev/null 2>&1 || fail "python3 is not installed. It is used to pack the game data."
}

# makerom and bannertool are not part of devkitPro. Build them once into tools/3ds/bin if they are not already about.
require_packaging_tools() {
    local bin_dir="${REPO_ROOT}/tools/3ds/bin"
    mkdir -p "${bin_dir}"

    MAKEROM="$(command -v makerom || true)"
    BANNERTOOL="$(command -v bannertool || true)"

    [[ -n "${MAKEROM}" ]]    || { [[ -x "${bin_dir}/makerom" ]]    && MAKEROM="${bin_dir}/makerom"; }
    [[ -n "${BANNERTOOL}" ]] || { [[ -x "${bin_dir}/bannertool" ]] && BANNERTOOL="${bin_dir}/bannertool"; }

    if [[ -z "${MAKEROM}" || -z "${BANNERTOOL}" ]]; then
        info "makerom and/or bannertool were not found: building them from source into tools/3ds/bin."
        info "This happens once. Pass --skip-cia to build only the .3dsx and skip this entirely."
        "${REPO_ROOT}/tools/3ds/build-packaging-tools.sh" || fail "Could not build the packaging tools. Build them yourself and put them on PATH, or use --skip-cia."

        MAKEROM="${bin_dir}/makerom"
        BANNERTOOL="${bin_dir}/bannertool"
    fi

    [[ -x "${MAKEROM}" ]] || fail "makerom is still missing."
    [[ -x "${BANNERTOOL}" ]] || fail "bannertool is still missing."
    export MAKEROM BANNERTOOL
}

# ----------------------------------------------------------------------------------------------------------------
# Finding the disc
# ----------------------------------------------------------------------------------------------------------------
find_edition_cue() {
    local edition="$1"
    local disc_dir="$2"
    local pattern
    pattern="$(edition_cue_pattern "${edition}")"

    # Prefer a cue whose name mentions the edition; for plain 'doom' take care not to match 'final doom'
    local found=""
    while IFS= read -r cue; do
        local base
        base="$(basename "${cue}" | tr '[:upper:]' '[:lower:]')"

        case "${edition}" in
            doom)
                [[ "${base}" == *final* || "${base}" == *master* ]] && continue
                ;;
        esac

        if [[ "${base}" == *"${pattern}"* ]]; then
            found="${cue}"
            break
        fi
    done < <(find "${disc_dir}" -type f -iname '*.cue' | sort)

    printf '%s' "${found}"
}

# ----------------------------------------------------------------------------------------------------------------
# Packing the disc into a RomFS archive the game extracts on first run
# ----------------------------------------------------------------------------------------------------------------
prepare_romfs() {
    local edition="$1"
    local disc_dir="$2"
    local force="$3"

    local romfs_dir="${REPO_ROOT}/romfs/${edition}/psydoom"
    local archive="${romfs_dir}/disc.zip"

    if [[ -f "${archive}" && "${force}" -eq 0 ]]; then
        info "  disc archive already built (use --force-romfs to redo it)"
        return
    fi

    local cue
    cue="$(find_edition_cue "${edition}" "${disc_dir}")"
    [[ -n "${cue}" ]] || fail "No .cue file for '${edition}' under ${disc_dir}. See tools/3ds/README.md for what is expected."

    info "  disc: ${cue}"
    mkdir -p "${romfs_dir}"
    python3 "${REPO_ROOT}/tools/3ds/pack_disc.py" \
        --cue "${cue}" \
        --cue-name "$(edition_cue_name "${edition}")" \
        --output "${archive}"
}

# ----------------------------------------------------------------------------------------------------------------
# Banner music, cut from the disc's own audio track
# ----------------------------------------------------------------------------------------------------------------
prepare_metadata() {
    local edition="$1"
    local disc_dir="$2"
    local pkg_dir="${REPO_ROOT}/packaging/${edition}"
    local banner_wav="${pkg_dir}/banner.wav"

    [[ -f "${pkg_dir}/banner.png" ]] || fail "Missing ${pkg_dir}/banner.png"
    [[ -f "${pkg_dir}/icon.png" ]] || fail "Missing ${pkg_dir}/icon.png"

    if [[ -f "${banner_wav}" ]]; then
        info "  banner audio already built"
        return
    fi

    # Track two is the first music track, taken raw because it is CD audio: 44.1 kHz, 16 bit, stereo.
    #
    # Two rip layouts turn up and the track has to be found differently in each. One file per track puts it in a file
    # of its own; one file for the whole disc leaves it inside that file, with the cue giving its start as MM:SS:FF.
    # The Master Edition is the second kind, and only looking for a second file meant nothing was found there at all -
    # so its banner came out silent.
    local cue cue_dir track_bin skip_seconds file_list
    cue="$(find_edition_cue "${edition}" "${disc_dir}")"
    cue_dir="$(dirname "${cue}")"
    file_list="$(tr -d '\r' < "${cue}" | sed -n 's/^[[:space:]]*FILE[[:space:]]*"\(.*\)".*/\1/p')"

    if [ "$(printf '%s\n' "${file_list}" | grep -c .)" -ge 2 ]; then
        track_bin="${cue_dir}/$(printf '%s\n' "${file_list}" | sed -n '2p')"
        skip_seconds=5
    else
        track_bin="${cue_dir}/$(printf '%s\n' "${file_list}" | sed -n '1p')"

        # Where the cue says track two starts, as MM:SS:FF at 75 frames a second, plus a few seconds to get past the
        # quiet lead-in.
        local stamp
        stamp="$(tr -d '\r' < "${cue}" | awk '
            toupper($1) == "TRACK" && $2 + 0 == 2 { in_track = 1; next }
            toupper($1) == "TRACK" { in_track = 0 }
            in_track && toupper($1) == "INDEX" && $2 + 0 == 1 { print $3; exit }
        ')"

        if [[ -z "${stamp}" ]]; then
            warn "  the cue does not say where the music starts, so the banner will be silent"
            ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=16364:cl=stereo -t 3 -c:a pcm_s16le "${banner_wav}"
            return
        fi

        skip_seconds="$(awk -v s="${stamp}" 'BEGIN { split(s, p, ":"); printf "%.2f", p[1] * 60 + p[2] + p[3] / 75 + 3 }')"
    fi

    if [[ ! -f "${track_bin}" ]]; then
        warn "  could not find the disc's music track, so the banner will be silent"
        ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=16364:cl=stereo -t 3 -c:a pcm_s16le "${banner_wav}"
        return
    fi

    info "  banner audio: from $(basename "${track_bin}") at ${skip_seconds}s"

    # 16364 Hz stereo, exactly three seconds. These are not preferences: banner audio outside them plays as noise.
    # See packaging/BANNER_ASSETS.md.
    ffmpeg -hide_banner -loglevel error -y \
        -f s16le -ar 44100 -ac 2 -ss "${skip_seconds}" -i "${track_bin}" \
        -t 3 -af "volume=1.65,afade=t=in:st=0:d=0.10,afade=t=out:st=2.75:d=0.25" \
        -ar 16364 -ac 2 -c:a pcm_s16le "${banner_wav}"

    python3 "${REPO_ROOT}/tools/3ds/check_banner_audio.py" "${banner_wav}"
}

# ----------------------------------------------------------------------------------------------------------------
# Compiling and packaging
# ----------------------------------------------------------------------------------------------------------------
build_edition() {
    local edition="$1"
    local jobs="$2"
    local skip_cia="$3"

    local build_dir="${REPO_ROOT}/build-3ds-${edition}"
    local dist_dir="${REPO_ROOT}/dist/${edition}"
    local pkg_dir="${REPO_ROOT}/packaging/${edition}"
    local stem
    stem="$(edition_file_stem "${edition}")"

    mkdir -p "${dist_dir}"

    # The Vulkan renderer and the desktop launcher cannot be built for this target, and the compiler based dependency
    # scanner does not work with the devkitARM toolchain file.
    cmake -S "${REPO_ROOT}" -B "${build_dir}" \
        -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_DEPENDS_USE_COMPILER=FALSE \
        -DPSYDOOM_INCLUDE_VULKAN_RENDERER=OFF \
        -DPSYDOOM_INCLUDE_LAUNCHER=OFF \
        -DPSYDOOM_3DS_VARIANT="${edition}" \
        -DPSYDOOM_3DS_ROMFS_DIR="${REPO_ROOT}/romfs/${edition}" \
        -DPSYDOOM_3DS_ICON="${pkg_dir}/icon.png" \
        > /dev/null

    cmake --build "${build_dir}" --parallel "${jobs}"

    cp "${build_dir}/game/PsyDoom.3dsx" "${dist_dir}/${stem}.3dsx"
    cp "${build_dir}/game/PsyDoom.smdh" "${dist_dir}/${stem}.smdh"

    if [[ "${skip_cia}" -eq 1 ]]; then
        info "  built ${stem}.3dsx (skipping the .cia)"
        return
    fi

    local banner_bnr="${build_dir}/banner.bnr"
    "${BANNERTOOL}" makebanner -i "${pkg_dir}/banner.png" -a "${pkg_dir}/banner.wav" -o "${banner_bnr}" > /dev/null

    "${MAKEROM}" -f cia -o "${dist_dir}/${stem}.cia" -target t -desc app:2.50 \
        -rsf "${REPO_ROOT}/packaging/PsyDoom.rsf" \
        -elf "${build_dir}/game/PsyDoom.elf" \
        -icon "${build_dir}/game/PsyDoom.smdh" \
        -banner "${banner_bnr}" \
        "-DAPP_TITLE=$(edition_title "${edition}")" \
        "-DAPP_PRODUCT_CODE=$(edition_product_code "${edition}")" \
        "-DAPP_UNIQUE_ID=$(edition_unique_id "${edition}")" \
        "-DDIR_ROMFS=${REPO_ROOT}/romfs/${edition}"

    info "  built ${stem}.3dsx and ${stem}.cia"
}
