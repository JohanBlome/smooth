#!/usr/bin/env bash
# Build and install using the same source/build-tree workflow as spectra/scopes.
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
set -euo pipefail

OBS_STUDIO_DIR=""
OBS_BUILD_DIR=""
OPENCV_DIR=""
PREFIX=""
BUILD_DIR="build"
SKIP_THIRD_PARTY=0

if [[ "$(uname)" == "Darwin" ]]; then
  IS_MACOS=1
else
  IS_MACOS=0
fi

has_obsconfig() {
  local dir="${1:-}"
  [[ -n "$dir" ]] && { [[ -f "${dir}/config/obsconfig.h" ]] || [[ -f "${dir}/obsconfig.h" ]]; }
}

has_libobs() {
  local dir="${1:-}"
  [[ -n "$dir" ]] && {
    [[ -f "${dir}/libobs/RelWithDebInfo/libobs.framework/Versions/A/libobs" ]] ||
    [[ -f "${dir}/libobs/Release/libobs.framework/Versions/A/libobs" ]] ||
    [[ -f "${dir}/libobs/Debug/libobs.framework/Versions/A/libobs" ]] ||
    [[ -f "${dir}/libobs/RelWithDebInfo/libobs.so" ]] ||
    [[ -f "${dir}/libobs/Release/libobs.so" ]] ||
    [[ -f "${dir}/libobs/Debug/libobs.so" ]] ||
    [[ -f "${dir}/libobs/libobs.dylib" ]] ||
    [[ -f "${dir}/libobs.dylib" ]] ||
    [[ -f "${dir}/libobs/libobs.so" ]] ||
    [[ -f "${dir}/libobs.so" ]]
  }
}

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  -s, --source DIR       obs-studio source checkout
  -b, --build DIR        configured obs-studio build directory
  --opencv-dir DIR       directory containing OpenCVConfig.cmake
  --prefix PREFIX        install prefix
  --build-dir DIR        plugin build directory (default: build)
  --skip-third-party     require blobs to exist instead of installing them
  -h, --help             show this help

Defaults:
  macOS prefix: ~/Library/Application Support/obs-studio
  Linux prefix: /opt/obs-studio
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -s|--source) OBS_STUDIO_DIR="$2"; shift 2 ;;
    -b|--build) OBS_BUILD_DIR="$2"; shift 2 ;;
    --opencv-dir) OPENCV_DIR="$2"; shift 2 ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --skip-third-party) SKIP_THIRD_PARTY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ -z "$OBS_STUDIO_DIR" ]]; then
  for candidate in "${ROOT_DIR}/../obs-studio" "${HOME}/code/obs-studio" "${HOME}/obs-studio"; do
    if [[ -f "${candidate}/libobs/obs-module.h" ]]; then
      OBS_STUDIO_DIR="$(cd "$candidate" && pwd)"
      echo "Auto-detected OBS source: ${OBS_STUDIO_DIR}"
      break
    fi
  done
fi

if [[ -z "$OBS_BUILD_DIR" && -n "$OBS_STUDIO_DIR" ]]; then
  for candidate in "${OBS_STUDIO_DIR}/build_macos" "${OBS_STUDIO_DIR}/build" "${OBS_STUDIO_DIR}"/build*/; do
    if has_obsconfig "$candidate"; then
      OBS_BUILD_DIR="${candidate%/}"
      echo "Auto-detected OBS build: ${OBS_BUILD_DIR}"
      break
    fi
  done
fi

if [[ -z "$OBS_STUDIO_DIR" || ! -f "${OBS_STUDIO_DIR}/libobs/obs-module.h" ]]; then
  echo "ERROR: could not find obs-studio sources; pass --source" >&2
  exit 1
fi

if [[ $IS_MACOS -eq 1 ]]; then
  if ! has_obsconfig "$OBS_BUILD_DIR"; then
    if [[ -f "${OBS_STUDIO_DIR}/CMakePresets.json" ]]; then
      OBS_BUILD_DIR="${OBS_STUDIO_DIR}/build_macos"
      echo "Configuring OBS build tree: ${OBS_BUILD_DIR}"
      cmake --preset macos -DENABLE_BROWSER=OFF -S "$OBS_STUDIO_DIR"
    fi
  fi
fi

if ! has_obsconfig "$OBS_BUILD_DIR"; then
  echo "ERROR: OBS build is not configured; pass --build" >&2
  exit 1
fi

if ! has_libobs "$OBS_BUILD_DIR"; then
  echo "Building OBS libobs target..."
  if [[ $IS_MACOS -eq 1 ]]; then
    cmake --build "$OBS_BUILD_DIR" --target libobs --config RelWithDebInfo
  else
    cmake --build "$OBS_BUILD_DIR" --target libobs
  fi
fi
if ! has_libobs "$OBS_BUILD_DIR"; then
  echo "ERROR: OBS build did not produce a linkable libobs" >&2
  exit 1
fi

LOCAL_OPENCV_DIR="${ROOT_DIR}/.deps/opencv/lib/cmake/opencv4"
if [[ $SKIP_THIRD_PARTY -eq 0 ]]; then
  if [[ $IS_MACOS -eq 1 && -z "$OPENCV_DIR" ]]; then
    "${SCRIPT_DIR}/install_third_party.sh" --with-opencv
  else
    "${SCRIPT_DIR}/install_third_party.sh" --without-opencv
  fi
else
  "${SCRIPT_DIR}/install_third_party.sh" --check --without-opencv
fi

if [[ -z "$OPENCV_DIR" && $IS_MACOS -eq 1 ]]; then
  for candidate in "$LOCAL_OPENCV_DIR" /opt/homebrew/opt/opencv/lib/cmake/opencv4 /usr/local/opt/opencv/lib/cmake/opencv4; do
    if [[ -f "${candidate}/OpenCVConfig.cmake" ]]; then
      OPENCV_DIR="$candidate"
      echo "Auto-detected OpenCV: ${OPENCV_DIR}"
      break
    fi
  done
fi
if [[ $IS_MACOS -eq 1 && -z "$OPENCV_DIR" ]]; then
  echo "ERROR: OpenCV C++ SDK not found." >&2
  echo "       Run scripts/install_third_party.sh --with-opencv or pass --opencv-dir." >&2
  exit 1
fi

if [[ -z "$BUILD_DIR" || "$BUILD_DIR" == "/" ]]; then
  echo "ERROR: --build-dir must not be empty or /" >&2
  exit 1
fi
if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi
if [[ "$BUILD_DIR" == "$ROOT_DIR" ]]; then
  echo "ERROR: build directory must not be the project root" >&2
  exit 1
fi

cmake -E remove_directory "$BUILD_DIR"

CMAKE_ARGS=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DOBS_STUDIO_DIR="$OBS_STUDIO_DIR"
  -DOBS_BUILD_DIR="$OBS_BUILD_DIR"
)
if [[ -n "$OPENCV_DIR" ]]; then
  CMAKE_ARGS+=(-DOpenCV_DIR="$OPENCV_DIR")
fi
cmake "${CMAKE_ARGS[@]}"

if [[ $IS_MACOS -eq 1 ]]; then
  CORES="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
else
  CORES="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)"
fi
cmake --build "$BUILD_DIR" --parallel "$CORES"

MODULE="${BUILD_DIR}/smooth.so"
if [[ ! -f "$MODULE" ]]; then
  echo "ERROR: built module not found: ${MODULE}" >&2
  exit 1
fi

copy_data_files() {
  local source="$1"
  local destination="$2"
  shift 2
  "$@" mkdir -p "$destination"
  if command -v rsync >/dev/null 2>&1 && [[ $# -eq 0 ]]; then
    rsync -a --exclude='__pycache__/' --exclude='*.pyc' "${source}/" "${destination}/"
  else
    "$@" cp -a "${source}/." "$destination/"
  fi
}

if [[ $IS_MACOS -eq 1 ]]; then
  [[ -n "$PREFIX" ]] || PREFIX="${HOME}/Library/Application Support/obs-studio"
  BUNDLE="${PREFIX}/plugins/smooth.plugin"
  cmake -E remove_directory "$BUNDLE"
  mkdir -p "${BUNDLE}/Contents/MacOS" "${BUNDLE}/Contents/Resources"
  cp "$MODULE" "${BUNDLE}/Contents/MacOS/smooth"
  chmod +x "${BUNDLE}/Contents/MacOS/smooth"
  mkdir -p "${BUNDLE}/Contents/Frameworks"
  cmake \
    -DMODULE="${BUNDLE}/Contents/MacOS/smooth" \
    -DOUTPUT_DIR="${BUNDLE}/Contents/Frameworks" \
    -P "${ROOT_DIR}/cmake/BundleOpenCV.cmake"
  if ! otool -l "${BUNDLE}/Contents/MacOS/smooth" | grep -Fq '@loader_path/../Frameworks'; then
    install_name_tool -add_rpath '@loader_path/../Frameworks' "${BUNDLE}/Contents/MacOS/smooth"
  fi
  cp "${ROOT_DIR}/packaging/macos/Info.plist" "${BUNDLE}/Contents/Info.plist"
  copy_data_files "${ROOT_DIR}/data" "${BUNDLE}/Contents/Resources"
  if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep --sign - "$BUNDLE"
  fi
  echo "Installed: ${BUNDLE}"
else
  [[ -n "$PREFIX" ]] || PREFIX="/opt/obs-studio"
  INSTALL_CMD=()
  probe="$PREFIX"
  while [[ ! -e "$probe" && "$probe" != "/" ]]; do probe="$(dirname "$probe")"; done
  if [[ ! -w "$probe" && ${EUID:-$(id -u)} -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || { echo "ERROR: sudo is required for ${PREFIX}" >&2; exit 1; }
    INSTALL_CMD=(sudo)
  fi
  "${INSTALL_CMD[@]}" install -D -m 0755 "$MODULE" "${PREFIX}/lib/obs-plugins/smooth.so"
  copy_data_files "${ROOT_DIR}/data" "${PREFIX}/share/obs/obs-plugins/smooth" "${INSTALL_CMD[@]}"
  echo "Installed module: ${PREFIX}/lib/obs-plugins/smooth.so"
fi

echo "Restart OBS and add the Smooth filter."
