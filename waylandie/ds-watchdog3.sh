#!/system/bin/sh
# Spin watchdog v3: 90s grace, kill only near-full-core sustained spin (>850/20s = 85%).
LOG=/data/local/tmp/ds-watchdog.log
GRACE_SECS=90
TICK_LIMIT=850
echo "watchdog3 start $(date) grace=${GRACE_SECS}s limit=${TICK_LIMIT}" >> $LOG
while true; do
  HP=$(pgrep -f '^/usr/bin/Hyprland' | head -1)
  if [ -n "$HP" ]; then
    echo $HP > /dev/cpuctl/ds-guard/tasks 2>/dev/null
    echo $HP > /dev/memcg/ds-guard/tasks 2>/dev/null
    START=$(cut -d' ' -f22 /proc/$HP/stat 2>/dev/null)
    UPTIME=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    AGE=$((UPTIME - START / 100))
    if [ "$AGE" -gt "$GRACE_SECS" ]; then
      U1=$(cut -d' ' -f14 /proc/$HP/stat 2>/dev/null)
      S1=$(cut -d' ' -f15 /proc/$HP/stat 2>/dev/null)
      sleep 20
      [ -d /proc/$HP ] || continue
      U2=$(cut -d' ' -f14 /proc/$HP/stat 2>/dev/null)
      S2=$(cut -d' ' -f15 /proc/$HP/stat 2>/dev/null)
      [ -z "$U2" ] && continue
      TICKS=$(( (U2 - U1) + (S2 - S1) ))
      if [ "$TICKS" -gt "$TICK_LIMIT" ]; then
        echo "$(date) KILL spin: $TICKS ticks/20s pid $HP" >> $LOG
        kill -9 $HP
      fi
    fi
  fi
  sleep 5
done
