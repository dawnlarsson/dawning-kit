# Moonwater's machine script.
#
# The kernel build bakes it in as the
# fallback, so a machine whose disks never appear still has a working
# power button and reset. On a running system, copy it to
# /root/main.moonwater.sh to overlay the builtin without rebuilding:
# a regular file, owned by root, not group- or world-writable, not a
# symlink, at most 64 KiB.
#
# Three functions are the contract. None of them have to exist.
# A literal arm owns that event at that line; canvas) owns both
# canvas on and canvas off ($2 is on or off); *) owns the rest.
# Events this file does not name still use `moonwater bind`.
# moonwater bind then prints these line numbers, and SET is refused.

# Once, after boot has written a verdict. $1 is that line: live,
# disk <name>, or ask ...
# function moonwater_init {
#   case $1 in
#   live)
#     # headless media can `moonwater live` here
#     ;;
#   esac
# }

function moonwater_event {
  case $1 in
  poweroff)
    poweroff
    ;;
  reset|ctrl_alt_delete)
    reboot
    ;;
  # mute)
  #   ;;
  # volume_up|volume_down)
  #   ;;
  # brightness_up|brightness_down)
  #   ;;
  # lid_close|lid_open)
  #   ;;
  # sleep)
  #   ;;
  # canvas)
  #   # $1 is canvas; $2 is on or off
  #   ;;
  # recover)
  #   # last event did not finish; not a bind row
  #   ;;
  # *)
  #   # every event without its own arm
  #   ;;
  esac
}

# The machine is stopping; filesystems still write.
# function moonwater_end {
#   :
# }
