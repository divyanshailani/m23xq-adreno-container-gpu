#!/bin/bash
# Hyprland launch: persistent bridge + freedreno/kgsl + plain config.
export PATH=/usr/local/bin:/usr/bin:/bin
export XDG_RUNTIME_DIR=/tmp/runtime-0
mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR

D=""
for d in $(ls -dt /tmp/waylandie-wayland.* 2>/dev/null); do
  [ -f "$d/socket-name.txt" ] && D="$d" && break
done
if [ -z "$D" ]; then
  FRAME_COUNT=100000000 SERVER_TIMEOUT_MS=100000000 CLIENT_MODE=external ACCEPT_CLIENT_COMPLETE=0 \
    nohup sh /root/waylandie/bridge/waylandie-wayland-bridge.sh > /root/bridge-final.log 2>&1 &
  for i in $(seq 1 120); do
    D=$(ls -dt /tmp/waylandie-wayland.* 2>/dev/null | head -1)
    [ -n "$D" ] && [ -f "$D/socket-name.txt" ] && break
    sleep 0.5
  done
fi
if [ -z "$D" ] || [ ! -f "$D/socket-name.txt" ]; then echo "FAIL: no bridge socket"; exit 1; fi
export WAYLAND_DISPLAY=$(cat "$D/socket-name.txt")
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export XDG_SESSION_TYPE=wayland
nohup /usr/bin/Hyprland --i-am-really-stupid -c /root/plain.conf > /root/hyprland-final.log 2>&1 &
HPID=$!
echo "hyprland-final pid $HPID socket $WAYLAND_DISPLAY dir $D"
sleep 5
kill -0 $HPID 2>/dev/null && echo "hyprland alive after 5s" || echo "hyprland DIED after 5s"
