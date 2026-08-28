#!/bin/bash
# Full rice: native 2408x1080 output, Hyprland as bridge's external client.
export PATH=/usr/local/bin:/usr/bin:/bin
export XDG_RUNTIME_DIR=/tmp/runtime-0
mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR

pgrep -f '^/usr/bin/Hyprland' | xargs -r kill 2>/dev/null
pkill -f 'wayland-shm-ahb-server' 2>/dev/null
sleep 2

export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export XDG_SESSION_TYPE=wayland

FRAME_COUNT=100000000 SERVER_TIMEOUT_MS=100000000 CLIENT_MODE=external ACCEPT_CLIENT_COMPLETE=0 \
CLIENT_WIDTH=1604 CLIENT_HEIGHT=720 \
WAYLANDIE_WAYLAND_FRAME_CALLBACK_MODE=immediate WAYLANDIE_WAYLAND_REFRESH_HZ=120 \
EXTERNAL_CLIENT_COMMAND="env MESA_LOADER_DRIVER_OVERRIDE=kgsl XDG_SESSION_TYPE=wayland XDG_RUNTIME_DIR=/tmp/runtime-0 /usr/bin/Hyprland --i-am-really-stupid > /root/hyprland-rice5.log 2>&1" \
  nohup sh /root/waylandie/bridge/waylandie-wayland-bridge.sh > /root/bridge-rice5.log 2>&1 &
echo "native-res session pid $!"
sleep 20
pgrep -f '^/usr/bin/Hyprland' >/dev/null && echo "rice5 hyprland alive" || echo "rice5 hyprland DIED"
