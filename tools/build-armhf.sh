#!/usr/bin/env bash
# Build inside the Debian bookworm container in Dockerfile.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work="$root/.build/armhf"
sysroot="$work/sysroot"
mkdir -p "$work/packages" "$work/deps" "$sysroot" "$root/dist"
(
 cd "$work/packages"
 apt-get download libssl-dev:armhf zlib1g-dev:armhf libwebrtc-audio-processing-dev:armhf libc6:armhf libgcc-s1:armhf libstdc++6:armhf
 for package in ./*.deb; do dpkg-deb -x "$package" "$sysroot"; done
)
clone_tag() {
 local url=$1 tag=$2 target=$3
 if [[ ! -d "$target/.git" ]]; then git clone --depth 1 --branch "$tag" "$url" "$target"; fi
 [[ "$(git -C "$target" describe --tags --exact-match)" == "$tag" ]]
}
clone_tag https://github.com/nlohmann/json.git v3.11.3 "$work/deps/json"
clone_tag https://github.com/machinezone/IXWebSocket.git v11.4.5 "$work/deps/ixwebsocket"
clone_tag https://github.com/gabime/spdlog.git v1.15.1 "$work/deps/spdlog"
clone_tag https://github.com/k2-fsa/sherpa-onnx.git v1.12.25 "$work/deps/sherpa-source"
archive="$work/sherpa-shared.tar.bz2"
if [[ ! -d "$work/sherpa-shared/sherpa-onnx-v1.12.25-linux-arm-gnueabihf-shared" ]]; then
 curl --fail --location --retry 3 https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.25/sherpa-onnx-v1.12.25-linux-arm-gnueabihf-shared.tar.bz2 -o "$archive"
 mkdir -p "$work/sherpa-shared"
 tar -xjf "$archive" -C "$work/sherpa-shared"
fi
cmake -S "$root/device/edge" -B "$work/build" -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$root/device/edge/armv7-linux-gnueabihf.cmake" \
 -DEDGE_ROOT="$work" -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/build" --parallel "${BUILD_JOBS:-2}"
package="$work/package"
mkdir -p "$package/lib" "$package/assets" "$package/LICENSES"
cp "$work/build/doubao-edge" "$package/doubao-edge"
arm-linux-gnueabihf-strip "$package/doubao-edge"
sherpa="$work/sherpa-shared/sherpa-onnx-v1.12.25-linux-arm-gnueabihf-shared"
for lib in libsherpa-onnx-cxx-api.so libsherpa-onnx-c-api.so libonnxruntime.so; do cp -L "$sherpa/lib/$lib" "$package/lib/$lib"; done
for lib in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libgcc_s.so.1; do
 source=$(find "$sysroot" -type f -name "$lib" -print -quit)
 [[ -n "$source" ]]
 cp "$source" "$package/lib/$lib"
done
source=$(find "$sysroot" -type f -name 'libstdc++.so.6.*' -print -quit)
[[ -n "$source" ]]
cp "$source" "$package/lib/libstdc++.so.6"
model_archive="$work/kws-model.tar.bz2"
if [[ ! -d "$work/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20" ]]; then
 curl --fail --location --retry 3 https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2 -o "$model_archive"
 tar -xjf "$model_archive" -C "$work"
fi
model="$work/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20"
cp "$model/encoder-epoch-13-avg-2-chunk-8-left-64.int8.onnx" "$package/assets/encoder.onnx"
cp "$model/decoder-epoch-13-avg-2-chunk-8-left-64.onnx" "$package/assets/decoder.onnx"
cp "$model/joiner-epoch-13-avg-2-chunk-8-left-64.int8.onnx" "$package/assets/joiner.onnx"
cp "$model/tokens.txt" "$package/assets/tokens.txt"
curl --fail --location --retry 3 https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx -o "$package/assets/silero_vad.onnx"
printf '9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6  %s\n' "$package/assets/silero_vad.onnx" | sha256sum -c -
cp "$root/device/edge/keywords.txt" "$package/assets/keywords.txt"
cp "$root/device/edge/"{start.sh,stop.sh,config.ini.example,UPSTREAM-LICENSE} "$package/"
cp "$root/device/doubao-lease.sh" "$package/lease.sh"
cp "$root/device/asound-micref.conf" "$package/asound.conf"
cp "$root/device/enable-autostart.sh" "$package/enable-autostart.sh"
cp "$root/LICENSE" "$root/NOTICE.md" "$package/"
for dependency in json ixwebsocket spdlog sherpa-source; do
 mkdir -p "$package/LICENSES/$dependency"
 find "$work/deps/$dependency" -maxdepth 1 -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' \) -exec cp {} "$package/LICENSES/$dependency/" \;
done
cp -a "$sysroot/usr/share/doc" "$package/LICENSES/debian-packages"
cp -a /usr/share/common-licenses "$package/LICENSES/debian-common-licenses"
find "$sherpa" "$model" -maxdepth 2 -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' \) -exec cp {} "$package/LICENSES/" \;
chmod 700 "$package/doubao-edge" "$package/"*.sh
(cd "$package"; find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -czf "$root/dist/doubao-edge-runtime.tar.gz" -C "$package" .
cp "$work/build/doubao-edge" "$root/dist/doubao-edge"
printf 'Build complete: %s\n' "$root/dist/doubao-edge-runtime.tar.gz"
