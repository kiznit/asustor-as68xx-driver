#!/bin/sh
# ZED zedlet — drive AS68xxT bay error LEDs from live vdev statechange events.
#
# Install:
#     sudo install -m 0755 statechange-led.sh /etc/zfs/zed.d/
#     sudo systemctl restart zed
#
# Pairs with asustor-zfs-sync-leds (run at boot) which handles the case
# where a pool is imported already-degraded — in that path no statechange
# event fires, so this zedlet alone is not sufficient.

# shellcheck disable=SC1091
[ -f /etc/zfs/zed.d/zed-functions.sh ] && . /etc/zfs/zed.d/zed-functions.sh

HELPER=${ASUSTOR_BAY_LED:-/usr/local/sbin/asustor-bay-led}

[ -x "$HELPER" ] || exit 0
[ -n "${ZEVENT_VDEV_PATH:-}" ] || exit 0

case "${ZEVENT_VDEV_PATH}" in
/dev/nvme*) exit 0 ;;
esac

case "${ZEVENT_VDEV_STATE_STR:-UNKNOWN}" in
ONLINE)                                action=off   ;;
DEGRADED)                              action=blink ;;
FAULTED | UNAVAIL | REMOVED | OFFLINE) action=on    ;;
*)                                     action=on    ;;
esac

"$HELPER" "$ZEVENT_VDEV_PATH" "$action" >/dev/null 2>&1 || true
exit 0
