#!/bin/sh
# Opt-in boot integration, after the private key and fixed greeting are installed.
set -eu
app=/data/doubao-edge
[ "$(id -u)" = 0 ] || { echo root_required; exit 1; }
for file in config.ini vivi-wake.pcm asound.conf doubao-edge lib/ld-linux-armhf.so.3 assets/encoder.onnx assets/silero_vad.onnx; do
 [ -s "$app/$file" ] || { echo "missing_runtime_file: $file"; exit 1; }
done
if grep -q SET_LOCALLY_WITHOUT_SHARING "$app/config.ini"; then echo configure_api_key_first; exit 1; fi
chmod 600 "$app/config.ini"
init=/data/init.sh
[ ! -L "$init" ] || { echo init_symlink_requires_manual_review; exit 1; }
if [ ! -e "$init" ]; then printf '#!/bin/sh\n' > "$init"; fi
if ! grep -q '^# BEGIN DOUBAO EDGE$' "$init"; then
 [ -e /data/init.sh.before-doubao-edge ] || cp -p "$init" /data/init.sh.before-doubao-edge
 printf '\n# BEGIN DOUBAO EDGE\n[ ! -f /data/doubao-edge/enabled ] || sh /data/doubao-edge/start.sh\n# END DOUBAO EDGE\n' >> "$init"
fi
sh -n "$init"
touch "$app/enabled"
sh "$app/start.sh"
echo edge_autostart_enabled
