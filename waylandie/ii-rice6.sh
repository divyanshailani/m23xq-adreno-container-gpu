#!/bin/bash
# ii-rice6: 1604x720@120, input-debug ON, clean kill of ALL quickshell forms.
export PATH=/usr/local/bin:/usr/bin:/bin
export XDG_RUNTIME_DIR=/tmp/runtime-0
mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR

# Kill everything from the previous session — both qs cmdline forms,
# the clipboard watchers, Hyprland, and the bridge server.
pkill -f 'qs -c ii' 2>/dev/null
pkill -f '^/usr/bin/qs' 2>/dev/null
pkill -f 'wl-paste' 2>/dev/null
pgrep -f '^/usr/bin/Hyprland' | xargs -r kill 2>/dev/null
pkill -f 'wayland-shm-ahb-server' 2>/dev/null
sleep 2

export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export XDG_SESSION_TYPE=wayland

FRAME_COUNT=100000000 SERVER_TIMEOUT_MS=100000000 CLIENT_MODE=external ACCEPT_CLIENT_COMPLETE=0 \
CLIENT_WIDTH=1203 CLIENT_HEIGHT=540 \
WAYLANDIE_WAYLAND_FRAME_CALLBACK_MODE=immediate WAYLANDIE_WAYLAND_REFRESH_HZ=120 \
WAYLANDIE_WAYLAND_INPUT_DEBUG=1 WAYLANDIE_WAYLAND_FRACTIONAL_SCALE=1.5 \
EXTERNAL_CLIENT_COMMAND="env MESA_LOADER_DRIVER_OVERRIDE=kgsl XDG_SESSION_TYPE=wayland XDG_RUNTIME_DIR=/tmp/runtime-0 /usr/bin/Hyprland --i-am-really-stupid > /root/hyprland-rice6.log 2>&1" \
  nohup sh /root/waylandie/bridge/waylandie-wayland-bridge.sh > /root/bridge-rice6.log 2>&1 &
echo "rice6 session pid $!"
sleep 20
pgrep -f '^/usr/bin/Hyprland' >/dev/null && echo "rice6 hyprland alive" || echo "rice6 hyprland DIED"
pgrep -f 'qs' | head -5
