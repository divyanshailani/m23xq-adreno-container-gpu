#!/bin/bash
export XDG_RUNTIME_DIR=/tmp/runtime-0
mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
cd /root
# generate xdg-shell protocol code via wayland-scanner if header missing
if [ ! -f /root/xdg-shell-client-protocol.h ]; then
  wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /root/xdg-shell-client-protocol.h
  wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /root/xdg-shell-client-protocol.c
fi
D=""
for d in $(ls -dt /tmp/waylandie-wayland.* 2>/dev/null); do
  [ -f "$d/socket-name.txt" ] && D="$d" && break
done
if [ -z "$D" ]; then
  nohup sh /root/waylandie/bridge/waylandie-wayland-bridge.sh > /root/bridge-c2.log 2>&1 &
  for i in $(seq 1 60); do
    D=$(ls -dt /tmp/waylandie-wayland.* 2>/dev/null | head -1)
    [ -n "$D" ] && [ -f "$D/socket-name.txt" ] && break
    sleep 0.5
  done
fi
[ -z "$D" ] && echo "FAIL no bridge" && exit 1
export WAYLAND_DISPLAY=$(cat "$D/socket-name.txt")
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
echo "stageC2 build $(date +%T)"
cc -O2 -o /root/gles2loop /root/gles2-wayland-loop.c /root/xdg-shell-client-protocol.c -I/root -lEGL -lGLESv2 -lwayland-client -lwayland-egl -lm 2>&1 | head -8
[ -x /root/gles2loop ] || { echo "FAIL build"; exit 1; }
echo "stageC2 run 120s $(date +%T)"
timeout 150 /root/gles2loop 120 1280 720
echo "stageC2 exit rc=$? $(date +%T)"
