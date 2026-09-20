# Moonwater's machine script.
#
# The kernel build bakes it in as the
# fallback, so a machine whose disks never appear still has a working
# power button and reset. On a running system, copy it to
# /root/main.moonwater.sh to overlay the builtin without rebuilding:
# a regular file, owned by root, not group- or world-writable, not a
# symlink, at most 64 KiB.
#
# moonwater_init, moonwater_event and moonwater_end are optional.
# moonwater_event, when present, runs for every event. A function
# moonwater_<event> is the hard binding for that row: moonwater bind
# prints its line and SET is refused. moonwater_canvas owns both
# canvas on and canvas off ($1 is on or off); moonwater_canvas_on
# owns only that side. A literal case arm in moonwater_event still
# owns that event; *) does not. Events this file does not name still
# use `moonwater bind`.
#
# The builtin init always forgets userspace. A kiosk is this file:
# wipe, then one already-installed command. /bowls stays. A machine
# that should keep /home omits moonwater_init, and bind init runs.
# Wifi, bluetooth, the internet preference, timezone, ntp and keyboard
# survive wipe on /root.

# Once, after boot has written a verdict. $1 is that line: live,
# disk <name>, or ask ...
function moonwater_init {
  case $1 in
  live|disk*)
    # moonwater wipe
    # After wipe, start whatever this machine is. A kiosk is one line
    # of already-installed software, for example:
    # chromium --kiosk --user-data-dir=/tmp/kiosk "$URL"
    # weston
    # /bowls/bin/exhibit
    # moonwater wifi add "ssid" "password"
    # moonwater wifi on
    # moonwater bluetooth on
    # moonwater priority internet wired
    # moonwater timezone Europe/Stockholm
    # moonwater keyboard se
    ;;
  esac
}

function moonwater_poweroff {
  poweroff
}

function moonwater_reset {
  reboot
}

function moonwater_ctrl_alt_delete {
  reboot
}

# Every event, including those with a function above. $1 is the name;
# canvas, tablet, headphone and dock pass $2 on or off.
function moonwater_event {
  :
  # mute)
  # volume_up / volume_down
  # brightness_up / brightness_down
  # lid_close / lid_open
  # sleep
  # micmute
  # rfkill
  # tablet / headphone / dock  ($2 is on or off)
  # resume
  # recover  (last event did not finish; not a bind row)
}

# The machine is stopping; filesystems still write.
# function moonwater_end {
#   :
# }
