#!/usr/bin/env bash
# Install redistributable runtime assets and an isolated minimal OpenCV SDK.
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
set -euo pipefail

YUNET_NAME="face_detection_yunet_2023mar.onnx"
YUNET_COMMIT="47534e27c9851bb1128ccc0102f1145e27f23f98"
YUNET_SHA256="8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"
YUNET_URL="https://media.githubusercontent.com/media/opencv/opencv_zoo/${YUNET_COMMIT}/models/face_detection_yunet/${YUNET_NAME}"

OPENCV_VERSION="4.12.0"
OPENCV_ARCHIVE="opencv-${OPENCV_VERSION}.tar.gz"
OPENCV_SHA256="44c106d5bb47efec04e531fd93008b3fcd1d27138985c5baf4eafac0e1ec9e9d"
OPENCV_URL="https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${ROOT_DIR}/data/models"
DEPS_DIR="${ROOT_DIR}/.deps"
OPENCV_PREFIX="${DEPS_DIR}/opencv"
CHECK_ONLY=0
FORCE=0

if [[ "$(uname)" == "Darwin" ]]; then
  INSTALL_OPENCV=1
else
  INSTALL_OPENCV=0
fi

usage() {
  cat <<EOF
Usage: $0 [options]

Installs and verifies third-party dependencies used by Smooth. On macOS this
builds a minimal private OpenCV SDK under .deps/opencv, avoiding Homebrew and
its Qt dependencies. Linux defaults to the system OpenCV development package.

Options:
  --check             Verify installed dependencies without downloading/building
  --force             Reinstall even when existing dependencies are valid
  --with-opencv       Build the private OpenCV SDK (default on macOS)
  --without-opencv    Only install/verify the YuNet model (default on Linux)
  --model-dir DIR     Override model destination (default: data/models)
  --deps-dir DIR      Override private dependency root (default: .deps)
  -h, --help          Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) CHECK_ONLY=1; shift ;;
    --force) FORCE=1; shift ;;
    --with-opencv) INSTALL_OPENCV=1; shift ;;
    --without-opencv) INSTALL_OPENCV=0; shift ;;
    --model-dir) MODEL_DIR="$2"; shift 2 ;;
    --deps-dir)
      DEPS_DIR="$2"
      OPENCV_PREFIX="${DEPS_DIR}/opencv"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ $CHECK_ONLY -eq 1 && $FORCE -eq 1 ]]; then
  echo "ERROR: --check and --force cannot be used together" >&2
  exit 2
fi

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    echo "ERROR: shasum or sha256sum is required" >&2
    exit 1
  fi
}

download_file() {
  local url="$1"
  local destination="$2"
  if command -v curl >/dev/null 2>&1; then
    curl --fail --location --retry 3 --output "$destination" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget --tries=3 --output-document="$destination" "$url"
  else
    echo "ERROR: curl or wget is required" >&2
    exit 1
  fi
}

fetch_verified() {
  local name="$1"
  local url="$2"
  local expected_sha="$3"
  local destination="$4"

  if [[ -f "$destination" ]]; then
    local actual_sha
    actual_sha="$(sha256_file "$destination")"
    if [[ "$actual_sha" == "$expected_sha" && $FORCE -eq 0 ]]; then
      echo "OK: ${name} (${actual_sha})"
      return
    fi
    if [[ $CHECK_ONLY -eq 1 ]]; then
      echo "ERROR: checksum mismatch for ${destination}" >&2
      echo "  expected: ${expected_sha}" >&2
      echo "  actual:   ${actual_sha}" >&2
      exit 1
    fi
  elif [[ $CHECK_ONLY -eq 1 ]]; then
    echo "ERROR: missing third-party file: ${destination}" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$destination")"
  local temporary="${destination}.download.$$"
  trap 'rm -f "${temporary:-}"' EXIT
  echo "Downloading ${name}..."
  download_file "$url" "$temporary"

  local downloaded_sha
  downloaded_sha="$(sha256_file "$temporary")"
  if [[ "$downloaded_sha" != "$expected_sha" ]]; then
    echo "ERROR: checksum mismatch for downloaded ${name}" >&2
    echo "  expected: ${expected_sha}" >&2
    echo "  actual:   ${downloaded_sha}" >&2
    exit 1
  fi

  mv "$temporary" "$destination"
  trap - EXIT
  echo "Installed: ${destination}"
}

install_yunet() {
  fetch_verified "$YUNET_NAME" "$YUNET_URL" "$YUNET_SHA256" "${MODEL_DIR}/${YUNET_NAME}"
}

opencv_config_path() {
  printf '%s/lib/cmake/opencv4/OpenCVConfig.cmake' "$OPENCV_PREFIX"
}

install_opencv() {
  local config
  config="$(opencv_config_path)"
  if [[ -f "$config" && $FORCE -eq 0 ]]; then
    echo "OK: private OpenCV SDK ${OPENCV_VERSION} (${OPENCV_PREFIX})"
    return
  fi
  if [[ $CHECK_ONLY -eq 1 ]]; then
    echo "ERROR: private OpenCV SDK is missing: ${config}" >&2
    exit 1
  fi

  command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake is required" >&2; exit 1; }
  command -v tar >/dev/null 2>&1 || { echo "ERROR: tar is required" >&2; exit 1; }

  local archive="${DEPS_DIR}/downloads/${OPENCV_ARCHIVE}"
  local source_dir="${DEPS_DIR}/src/opencv-${OPENCV_VERSION}"
  local build_dir="${DEPS_DIR}/build/opencv-${OPENCV_VERSION}"
  fetch_verified "OpenCV ${OPENCV_VERSION} sources" "$OPENCV_URL" "$OPENCV_SHA256" "$archive"

  cmake -E remove_directory "$source_dir"
  cmake -E remove_directory "$build_dir"
  if [[ $FORCE -eq 1 ]]; then
    cmake -E remove_directory "$OPENCV_PREFIX"
  fi
  mkdir -p "$(dirname "$source_dir")" "$build_dir" "$OPENCV_PREFIX"
  tar -xzf "$archive" -C "$(dirname "$source_dir")"

  local configure_args=(
    -S "$source_dir"
    -B "$build_dir"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$OPENCV_PREFIX"
    -DBUILD_LIST=core,imgproc,objdetect,dnn
    -DBUILD_SHARED_LIBS=ON
    -DBUILD_TESTS=OFF
    -DBUILD_PERF_TESTS=OFF
    -DBUILD_EXAMPLES=OFF
    -DBUILD_opencv_apps=OFF
    -DBUILD_JAVA=OFF
    -DBUILD_DOCS=OFF
    -DBUILD_opencv_python3=OFF
    -DBUILD_opencv_python_bindings_generator=OFF
    -DBUILD_PROTOBUF=ON
    -DWITH_PROTOBUF=ON
    -DWITH_FFMPEG=OFF
    -DWITH_GSTREAMER=OFF
    -DWITH_AVFOUNDATION=OFF
    -DWITH_ADE=OFF
    -DWITH_OPENCL=OFF
    -DWITH_OPENGL=OFF
    -DWITH_QT=OFF
    -DWITH_VTK=OFF
    -DWITH_IPP=OFF
    -DWITH_ITT=OFF
    -DWITH_TBB=OFF
    -DWITH_JPEG=OFF
    -DWITH_OPENJPEG=OFF
    -DWITH_PNG=OFF
    -DWITH_TIFF=OFF
    -DWITH_WEBP=OFF
    -DWITH_OPENEXR=OFF
  )
  if [[ "$(uname)" == "Darwin" ]]; then
    configure_args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
  fi

  echo "Configuring minimal OpenCV ${OPENCV_VERSION}..."
  cmake "${configure_args[@]}"
  local cores
  if [[ "$(uname)" == "Darwin" ]]; then
    cores="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  else
    cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)"
  fi
  echo "Building minimal OpenCV ${OPENCV_VERSION}..."
  cmake --build "$build_dir" --parallel "$cores"
  cmake --install "$build_dir"

  if [[ ! -f "$config" ]]; then
    echo "ERROR: OpenCV installation did not produce ${config}" >&2
    exit 1
  fi
  echo "Installed private OpenCV SDK: ${OPENCV_PREFIX}"

  # Keep the verified source archive for repeatability, but discard expanded
  # sources and objects once the installed SDK is complete.
  cmake -E remove_directory "$source_dir"
  cmake -E remove_directory "$build_dir"
}

install_yunet
if [[ $INSTALL_OPENCV -eq 1 ]]; then
  install_opencv
fi

if [[ $CHECK_ONLY -eq 1 ]]; then
  echo "All requested third-party dependencies are present and verified."
else
  echo "Third-party installation complete."
fi
