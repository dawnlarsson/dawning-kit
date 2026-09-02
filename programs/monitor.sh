#!/bin/sh
#
#       A live system monitor, written in the shell it is demonstrating.
#
#       This exists to put the shell under sustained, awkward load: functions
#       with locals, arithmetic, nested command substitution, pipelines into
#       awk, and a redraw every interval that has to be fast enough not to
#       flicker. A shell that is subtly wrong about any of those is obvious
#       here within a second, in a way a test that runs once is not.
#
#       Everything comes out of /proc, so it needs no privileges and nothing
#       outside the base system: grep, awk, sort, head, cut, stty, sleep,
#       printf.
#
#       Rates are real rates. Two samples are kept a frame apart in a scratch
#       directory and the deltas are taken between them, which is why the
#       first frame shows a settling reading and the rest do not.
#
#       Usage:  monitor.sh [interval] [frames]
#               interval  seconds between redraws, decimals accepted,
#                         default 0.5
#               frames    stop after this many, default 0 meaning never
#

interval=${1:-0.5}
frames=${2:-0}

#       Re-read on every frame, so dragging the window edge changes the next
#       frame instead of waiting for a restart. stty asks the input terminal:
#       stdout is temporarily a pipe while the command substitution runs.
screen_rows=24
screen_columns=80
cpu_rows=9
process_rows=9
network_rows=1
cpu_bar_width=64
memory_bar_width=42

scratch=${TMPDIR:-/tmp}/monitor.$$
mkdir -p "$scratch" 2>/dev/null

now_cpu=$scratch/cpu.now
old_cpu=$scratch/cpu.old
now_net=$scratch/net.now
old_net=$scratch/net.old
now_pid=$scratch/pid.now
old_pid=$scratch/pid.old
monitor_host=$(uname -n 2>/dev/null | cut -c1-14)

#       Put the terminal back the way it was found, whichever way we leave.
finish() {
        # End a frame transaction first in case a signal arrived during draw.
        printf '\033[?2026l\033[?25h\033[0m\033[?1049l'
        rm -rf "$scratch" 2>/dev/null
        exit 0
}
trap finish INT TERM HUP

#       A bar, coloured by how alarming the number is. Built a character at a
#       time because this shell has no ${var:offset:length} to slice a
#       prebuilt string with.
bar() {
        local value="$1" width="$2" filled i

        filled=$(( value * width / 100 ))
        [ "$filled" -lt 0 ] && filled=0
        [ "$filled" -gt "$width" ] && filled=$width

        if [ "$value" -ge 85 ]; then bar_colour=31
        elif [ "$value" -ge 60 ]; then bar_colour=33
        else bar_colour=32
        fi

        i=0
        bar_drawn=
        while [ "$i" -lt "$filled" ]; do bar_drawn="$bar_drawn|"; i=$(( i + 1 )); done
        while [ "$i" -lt "$width" ]; do bar_drawn="$bar_drawn "; i=$(( i + 1 )); done
}

#       Clear to end of line so a shorter line never leaves the tail of a
#       longer one behind. Cheaper and steadier than clearing the screen.
line() {
        printf '%s\033[K\n' "$1"
}

sample() {
        read sample_time ignored < /proc/uptime
        grep '^cpu' /proc/stat > "$now_cpu" 2>/dev/null
        grep ':' /proc/net/dev > "$now_net" 2>/dev/null

        #       awk opens each stat file itself rather than being handed
        #       them as arguments. /proc/[0-9]*/stat would work now, but this
        #       is one execve either way and does not build an argument list
        #       of several hundred paths to throw away a moment later.
        #
        #       Writing this is what found the ceiling that used to be here:
        #       the glob stopped at sixty four names without saying so, and
        #       the process list looked entirely plausible while being wrong.
        #
        #       utime and stime are fields 14 and 15 and rss is 24, but field
        #       2 is the command wrapped in parentheses and a command may
        #       contain spaces, so the line is split at the last ") " and the
        #       remaining fields counted from there: what would have been 14,
        #       15 and 24 are 12, 13 and 22 of the tail.
        ls /proc 2>/dev/null | awk '
                /^[0-9]+$/ {
                        file = "/proc/" $1 "/stat"
                        if ((getline line < file) > 0) {
                                close(file)
                                cut  = index(line, ") ")
                                head = substr(line, 1, cut)
                                tail = substr(line, cut + 2)
                                open = index(head, "(")
                                name = substr(head, open + 1, cut - open - 1)
                                split(tail, field, " ")
                                print $1, field[12] + field[13], field[22], name
                        }
                }' > "$now_pid" 2>/dev/null
}

header() {
        local up load load_one load_five load_fifteen clock s d h m

        s=${sample_time%%.*}
        d=$(( s / 86400 )); s=$(( s % 86400 ))
        h=$(( s / 3600 ));  s=$(( s % 3600 ))
        m=$(( s / 60 ))
        if [ "$d" -gt 0 ]; then up="${d}d ${h}h ${m}m"
        elif [ "$h" -gt 0 ]; then up="${h}h ${m}m"
        else up="${m}m"
        fi
        read load_one load_five load_fifteen ignored < /proc/loadavg
        load="$load_one $load_five $load_fifteen"
        clock=$(date +%H:%M:%S 2>/dev/null)

        if [ "$screen_columns" -ge 72 ]; then
                printf '\033[1m %-14s\033[0m up %-12s load %-18s %s #%02d\033[K\n' \
                        "$monitor_host" "$up" "$load" "$clock" "$(( count % 100 ))"
        else
                printf '\033[1m %-14s\033[0m %s #%02d\033[K\n' \
                        "$monitor_host" "$clock" "$(( count % 100 ))"
        fi
        line ""
}

cpus() {
        local shown
        shown=0

        #       First file seen is the previous sample, second is this one.
        #       Idle is the idle field plus iowait; everything else counts as
        #       work. A counter that went backwards means the sample was
        #       taken across a reset, so it is clamped rather than believed.
        awk 'FNR == NR {
                for (i = 2; i <= NF; i++) previous[$1, i] = $i
                seen[$1] = 1
                next
             }
             /^cpu/ {
                if (!($1 in seen)) next
                total = 0; idle = 0
                for (i = 2; i <= NF; i++) {
                        delta = $i - previous[$1, i]
                        if (delta < 0) delta = 0
                        total += delta
                        if (i == 5 || i == 6) idle += delta
                }
                tenths = (total > 0) ? int((total - idle) * 1000 / total + 0.5) : 0
                print $1, int((tenths + 5) / 10), sprintf("%.1f", tenths / 10)
             }' "$old_cpu" "$now_cpu" 2>/dev/null |
        while read name percent displayed; do
                [ -n "$displayed" ] || continue
                if [ "$name" = cpu ]; then
                        name=all
                else
                        shown=$(( shown + 1 ))
                        [ "$shown" -ge "$cpu_rows" ] && continue
                fi
                bar "$percent" "$cpu_bar_width"
                printf ' %-6s [\033[%sm%s\033[0m] %5s%%\033[K\n' \
                        "$name" "$bar_colour" "$bar_drawn" "$displayed"
        done
}

memory() {
        local report
        report=$(awk '/^MemTotal:/     { total = $2 }
                      /^MemAvailable:/ { free  = $2 }
                      /^SwapTotal:/    { swapt = $2 }
                      /^SwapFree:/     { swapf = $2 }
                      END {
                        used = total - free
                        pct  = (total > 0) ? int(used * 100 / total + 0.5) : 0
                        sp   = (swapt > 0) ? int((swapt - swapf) * 100 / swapt + 0.5) : 0
                        printf "%d %.1f %.1f %d %.1f", pct, used / 1048576,
                               total / 1048576, sp, (swapt - swapf) / 1048576
                      }' /proc/meminfo 2>/dev/null)

        set -- $report
        [ -n "$1" ] || return 0
        bar "$1" "$memory_bar_width"
        printf ' %-6s [\033[%sm%s\033[0m] %3s%%  %sG / %sG\033[K\n' \
                "mem" "$bar_colour" "$bar_drawn" "$1" "$2" "$3"
        if [ "${4:-0}" -gt 0 ]; then
                bar "$4" "$memory_bar_width"
                printf ' %-6s [\033[%sm%s\033[0m] %3s%%  %sG\033[K\n' \
                        "swap" "$bar_colour" "$bar_drawn" "$4" "$5"
        fi
}

network() {
        awk -v interval="$sample_interval" -v rows="$network_rows" \
            -v columns="$screen_columns" '
             function human(n) {
                if (n >= 1048576) return sprintf("%.1f MB", n / 1048576)
                if (n >= 1024) return sprintf("%.1f kB", n / 1024)
                return sprintf("%d B", n)
             }
             FNR == NR {
                split($0, part, ":")
                name = part[1]
                gsub(/ /, "", name)
                rest = part[2]
                split(rest, field, " ")
                previous[name "rx"] = field[1]
                previous[name "tx"] = field[9]
                next
             }
             {
                split($0, part, ":")
                name = part[1]
                gsub(/ /, "", name)
                if (name == "lo" || name == "") next
                rest = part[2]
                split(rest, field, " ")
                rx = field[1] - previous[name "rx"]
                tx = field[9] - previous[name "tx"]
                if (rx < 0) rx = 0
                if (tx < 0) tx = 0
                if (rx == 0 && tx == 0 && field[1] == 0) next
                if (shown >= rows) exit
                row = sprintf(" %-10s down %-12s up %-12s", name,
                              human(rx / interval) "/s",
                              human(tx / interval) "/s")
                printf "%s%c[K\n", substr(row, 1, columns - 1), 27
                shown++
             }' "$old_net" "$now_net" 2>/dev/null
}

processes() {
        line ""
        printf '\033[1m %-7s %6s %9s  %s\033[0m\033[K\n' \
                "pid" "cpu%" "memory" "command"

        #       Ticks used since the previous frame, over the ticks that
        #       elapsed. A process that was not there last frame simply has no
        #       previous entry and reads as zero rather than as its whole life.
        awk -v interval="$sample_interval" -v hz=100 '
             FNR == NR { previous[$1] = $2; next }
             {
                used = $2 - previous[$1]
                if (used < 0 || !($1 in previous)) used = 0
                percent = used * 100 / (hz * interval)
                name = $4
                for (i = 5; i <= NF; i++) name = name " " $i
                printf "%s %.1f %s %s\n", $1, percent, $3, name
             }' "$old_pid" "$now_pid" 2>/dev/null |
        sort -k2 -rn | head -"$process_rows" | awk -v columns="$screen_columns" '
                function human(n) {
                        if (n >= 1048576) return sprintf("%.1f MB", n / 1048576)
                        if (n >= 1024) return sprintf("%.1f kB", n / 1024)
                        return sprintf("%d B", n)
                }
                {
                        name = $4
                        for (i = 5; i <= NF; i++) name = name " " $i
                        row = sprintf(" %-7s %6s %9s  %s", $1, $2,
                                      human($3 * 4096), name)
                        if (held != "") printf "%s\033[K\n", held
                        held = substr(row, 1, columns - 1)
                }
                END {
                        # The final visible row must not end in newline: when
                        # it is the terminal bottom row, that would scroll.
                        if (held != "") printf "%s\033[K", held
                }'
}

draw() {
        printf '\033[H'

        if [ "$screen_rows" -lt 12 ] || [ "$screen_columns" -lt 40 ]; then
                printf ' monitor %sx%s' "$screen_columns" "$screen_rows" |
                        cut -c1-$(( screen_columns > 1 ? screen_columns - 1 : 1 ))
                printf '\033[K\033[J'
                return
        fi

        header
        cpus
        memory
        network
        processes
        printf '\033[J'
}

#       Enter visibly before doing any sampling. The old order cleared into an
#       empty alternate screen and then slept a whole interval before writing
#       the first frame, which made a healthy 15 ms launch look like a hung
#       half-second launch. This title is replaced by the settling frame as
#       soon as the first /proc walk completes.
printf '\033[?1049h\033[?25l\033[2J\033[H\033[1m monitor\033[0m  sampling...\033[K'
sample
cp "$now_cpu" "$old_cpu" 2>/dev/null
cp "$now_net" "$old_net" 2>/dev/null
cp "$now_pid" "$old_pid" 2>/dev/null
old_time_value=$sample_time

count=0
sample_interval=0.01
while :; do
        # The first sample is already in hand, so draw it immediately. CPU,
        # network and process rates are the documented settling values on that
        # frame. Every later frame sleeps first and gets a real delta. Besides
        # making startup instant, this preserves one sample and one draw per
        # frame instead of adding a special duplicate path.
        if [ "$count" -gt 0 ]; then
                sleep "$interval"
                sample
        fi

        # The dimensions are live state, not startup configuration. Keep this
        # outside a function: this shell deliberately stores function bodies,
        # and a dashboard should not spend that arena on straight-line work.
        set -- $(stty size 2>/dev/null)
        measured_rows=${1:-24}
        measured_columns=${2:-80}
        case "$measured_rows" in ''|*[!0-9]*|0) measured_rows=24 ;; esac
        case "$measured_columns" in ''|*[!0-9]*|0) measured_columns=80 ;; esac
        screen_rows=$measured_rows
        screen_columns=$measured_columns

        # Leave the last cell untouched: writing it puts real terminals into
        # pending-wrap state, where the following newline can consume a row.
        cpu_bar_width=$(( screen_columns - 18 ))
        memory_bar_width=$(( screen_columns - 39 ))
        [ "$cpu_bar_width" -lt 1 ] && cpu_bar_width=1
        [ "$memory_bar_width" -lt 1 ] && memory_bar_width=1

        # One reader answers all four layout questions. At the faster redraw
        # rate, two avoidable forks per frame are not bookkeeping, but load.
        set -- $(awk -v cpu_file="$now_cpu" -v net_file="$now_net" \
                -v before="$old_time_value" -v after="$sample_time" '
                FILENAME == cpu_file && /^cpu/ { cpus++ }
                FILENAME == net_file {
                        split($0, part, ":")
                        name = part[1]
                        gsub(/ /, "", name)
                        if (name != "" && name != "lo") networks++
                }
                FILENAME == "/proc/meminfo" && /^SwapTotal:/ { total = $2 }
                FILENAME == "/proc/meminfo" && /^SwapFree:/  { free = $2 }
                END {
                        pct = total > 0 ? int((total - free) * 100 / total + 0.5) : 0
                        elapsed = after - before
                        if (elapsed <= 0) elapsed = 0.01
                        print cpus + 0, networks + 0, pct > 0 ? 1 : 0, elapsed
                }' "$now_cpu" "$now_net" /proc/meminfo 2>/dev/null)
        cpu_count=${1:-1}
        network_count=${2:-0}
        swap_count=${3:-0}
        sample_interval=${4:-0.01}

        # Header (2), memory, optional swap and network, then the blank line
        # and process heading. Divide what remains between CPUs and tasks;
        # when there are fewer CPUs, the process table inherits every row.
        network_rows=$network_count
        network_room=$(( screen_rows - 7 - swap_count ))
        [ "$network_room" -lt 0 ] && network_room=0
        [ "$network_rows" -gt "$network_room" ] && network_rows=$network_room
        fixed_rows=$(( 5 + swap_count + network_rows ))
        available_rows=$(( screen_rows - fixed_rows ))
        [ "$available_rows" -lt 2 ] && available_rows=2
        cpu_rows=$(( available_rows / 2 ))
        [ "$cpu_rows" -gt "$cpu_count" ] && cpu_rows=$cpu_count
        [ "$cpu_rows" -lt 1 ] && cpu_rows=1
        process_rows=$(( available_rows - cpu_rows ))
        [ "$process_rows" -lt 1 ] && process_rows=1

        # DEC synchronized output is a transaction at the terminal/compositor
        # floor. All the ordinary shell writes above may arrive separately,
        # but no partially filled list is presented between these two modes.
        printf '\033[?2026h'
        draw
        printf '\033[?2026l'
        # The next sample recreates every .now file. Rename this frame into
        # place instead of opening, reading and rewriting the snapshots.
        mv -f "$now_cpu" "$old_cpu" 2>/dev/null
        mv -f "$now_net" "$old_net" 2>/dev/null
        mv -f "$now_pid" "$old_pid" 2>/dev/null
        old_time_value=$sample_time

        count=$(( count + 1 ))
        [ "$frames" -gt 0 ] && [ "$count" -ge "$frames" ] && break
done

finish
