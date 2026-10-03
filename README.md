# Smooth for OBS

Smooth is an experimental, face-aware OBS video filter. It targets two things
that are awkward to fix with a conventional blur:

- medium-scale skin variation while retaining fine texture and broad facial structure;
- dark under-eye shadows, without globally raising exposure.

The beauty pass runs in an OBS GPU effect. OBS compiles the same effect for
Metal on macOS, OpenGL on Linux, and Direct3D on Windows. Face detection uses
OpenCV's YuNet detector on a worker thread. Only a 320-pixel-wide image is read
back from the GPU, at a configurable rate, so detection does not hold up the
render thread.

## Current status

This is an MVP scaffold. It detects and processes the largest face, exposes
four OBS filter controls, and packages the shader and model with the plugin.
The next important step is testing on representative webcams and tuning the
face/eye masks. Multi-face support and a zero-copy detector path are not yet
implemented.

## Dependencies

- CMake 3.22 or newer
- a C++17 compiler
- OBS Studio development files (OBS 30 or newer; developed against OBS 32)
- OpenCV 4.5.4 or newer, with `core`, `imgproc`, `objdetect`, and `dnn`

The build follows the local OBS source/build-tree convention used by the
`spectra` and `scopes` projects. The standard entry point is
`scripts/build_install.sh`; direct CMake builds use `OBS_STUDIO_DIR` and
`OBS_BUILD_DIR`.

## Third-party blobs

The YuNet model is redistributable but intentionally not committed. Install it
before configuring:

```sh
./scripts/install_third_party.sh
```

The script downloads from a pinned OpenCV Zoo commit, checks the SHA-256, and
places the model under `data/models/`. It is safe to run repeatedly. CI or an
offline build can verify an existing copy without network access:

```sh
./scripts/install_third_party.sh --check
```

The model's MIT license is installed from `data/licenses/yunet/LICENSE`.
On macOS the same script also builds a minimal, pinned OpenCV SDK beneath the
ignored `.deps/opencv/` directory. It disables Qt, VTK, video I/O, and other
features Smooth does not use, so it does not modify or conflict with Homebrew's
Qt installation. On Linux, OpenCV defaults to the workstation development
package; pass `--with-opencv` to request the private build there too.

## Linux build

Install OpenCV development packages, configure/build an OBS source checkout,
then use the combined builder and installer:

```sh
./scripts/build_install.sh \
  --source ~/code/obs-studio \
  --build ~/code/obs-studio/build \
  --prefix /opt/obs-studio
```

The script installs `smooth.so` under `lib/obs-plugins` and the effect, model,
locale, and license under `share/obs/obs-plugins/smooth`, matching the sibling
plugins. It uses `sudo` only when the selected prefix is not writable.

## macOS build

Run the combined builder and installer; no Homebrew OpenCV installation is
required:

```sh
./scripts/build_install.sh
```

The script auto-detects `~/code/obs-studio`, its `build_macos` tree, and
the private OpenCV SDK. An external SDK remains supported as an override:

```sh
./scripts/build_install.sh \
  --source /path/to/obs-studio \
  --build /path/to/obs-studio/build_macos \
  --opencv-dir /path/to/opencv/lib/cmake/opencv4
```

The finished bundle is installed at
`~/Library/Application Support/obs-studio/plugins/smooth.plugin`.
Its required OpenCV dylibs are collected into `Contents/Frameworks`, so OBS
does not depend on the original build-tree path after installation.

For a direct CMake build on either platform:

```sh
./scripts/install_third_party.sh
cmake -S . -B build \
  -DOBS_STUDIO_DIR=/path/to/obs-studio \
  -DOBS_BUILD_DIR=/path/to/obs-studio/build \
  -DOpenCV_DIR=/path/to/opencv/lib/cmake/opencv4
cmake --build build
```

## Using the filter

In OBS, open a camera source's **Filters**, add **Smooth**, then start with:

- Smoothing amount: 0.45–0.60
- Detail center: 5–10 px
- Band width: 8–14 px
- Face-size scaling: 1.0 to keep the selected detail scale stable as faces move in depth
- Chroma smoothing: 0.0 for luma-only, 1.0 for full RGB smoothing
- Dark-eye correction: 0.30–0.50
- Face detection rate: 6–10 fps

The source itself remains full frame rate; the detection rate only controls how
often the face position is refreshed.

Use **Mask preview** to inspect tracking. **Detection geometry** shows the face
region in green, eye landmarks in red, and under-eye targets in blue.
**Treatment masks** shows the smoothing region in white and blends the
dark-eye treatment region toward blue. The detector retains the three largest
faces in each frame, and the GPU combines all three treatment masks in one pass.

The smoothing pass is a three-band decomposition. **Detail center** selects the
feature size to target and **Band width** controls the range around it. The
shader derives inner and outer low-pass radii from those controls, then removes
only the detail between them. A useful workflow is to start with a wide band,
sweep the center until the intended skin variation is selected, then tighten
the width and set the final amount. The extended ranges allow centers up to
80 px, widths up to 120 px, and deliberately exaggerated amounts up to 2.0.

Sharpening can be disabled or placed before or after smoothing. Both choices
are true GPU passes through an intermediate texture: **Before smoothing**
enhances fine texture before the selected middle band is removed, while
**After smoothing** sharpens the cleaned result. Sharpening defaults to luma
only; **Sharpen chroma** blends toward full RGB sharpening when desired.
**Sharpen scope** can process the full frame or restrict sharpening to the
three feathered detected-face regions. Select **Sharpen mask** under Mask
preview to inspect that region directly.

**Face-size scaling** interpolates between fixed pixel radii (`0.0`) and fully
face-relative radii (`1.0`). At full scaling, the smoothing center and width are
interpreted at a reference face height of 480 pixels and scale independently
for each of the three detected faces. Masked sharpening follows the same local
scale; full-frame sharpening remains fixed in screen pixels. Scaling is clamped
for very small or very large detections to avoid unstable sampling radii.
