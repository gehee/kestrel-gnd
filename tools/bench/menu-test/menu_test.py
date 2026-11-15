#!/usr/bin/env python3
"""Walk every VRX Pro menu setting on a live goggle and check each one.

A bench test for the VRX Pro (OSD/canopy menu, hud/osd_*), with an air unit
linked. It drives the real evdev input path through keysd (a uinput keyboard
on the goggle), and checks what each key did in kestrel-gnd's log:

  * every setting row of every tab: each value applied in turn (Enter), the
    row's own log line seen, kestrel-gnd still running, video still flowing;
    then the row's original value put back;
  * rows that restart something (Screen Mode, VRX Radio Firmware) and the
    trials (Screen Mode, Refresh Sync), the channel page (a move and back);
  * at the end the settings file is compared with the one at the start.

Never sent: KEY_VOLUMEUP (binds, menu open or not), Tab, Esc on the live
screen (it opens the gallery, where Down, Right, Enter deletes a clip).
Never pressed Enter on: RADIO > Bind, DVR > Format DVR. The channel page is
entered only by its own test.

  python3 menu_test.py [--host 192.168.3.1] [--only HUD,DISPLAY] [--skip-restarts]
                       [--max-values N] [--report FILE]

Needs: sshpass, the goggle on USB (root/fpvos), keysd built for the goggle
(see README.md; or the path in --keysd).
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

LOG = "/var/log/kestrel-gnd.log"
YAML = "/etc/kestrel/kestrel-gnd.yaml"
FIFO = "/tmp/kestrel-bench-keys.fifo"
ACK = "/tmp/kestrel-bench-keys.ack"
MENU_RE = re.compile(r"^menu: ([A-Z]+) > (.+?): (.*) \((edit|live|enter)\)$")
PICTURES_RE = re.compile(r"VdecRK: (\d+) pictures out in 5 s, (\d+) flagged with errors")
# Settings that change by themselves (measurements, last peer, clocks): not a
# difference between the start and the end of a test.
VOLATILE = re.compile(r"^(camera_period_us_|ar8030_last_peer|air_clock|screen_clock|frame_clock|"
                      r"display_lock_learn|.*_measured|.*_ppb)")


class Fail(Exception):
    pass


class Goggle:
    def __init__(self, host, password, keysd):
        self.host, self.password, self.keysd = host, password, keysd
        self.cm = os.path.join(tempfile.mkdtemp(prefix="menu-test-"), "cm")
        self.mark = 0
        self.pid = None
        self.sent = 0

    def ssh(self, cmd, data=None, timeout=60, check=True):
        args = ["sshpass", "-p", self.password, "ssh", "-q", "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=5",
                "-o", "ControlMaster=auto", "-o", "ControlPath=" + self.cm, "-o", "ControlPersist=900",
                "root@" + self.host, cmd]
        r = subprocess.run(args, input=data, capture_output=True, timeout=timeout)
        if check and r.returncode:
            raise Fail("ssh %r: %s" % (cmd, r.stderr.decode(errors="replace").strip()))
        return r.stdout.decode(errors="replace")

    # --- the log ---------------------------------------------------------------
    def size(self):
        return int(self.ssh("wc -c < %s" % LOG).strip() or 0)

    def set_mark(self):
        self.mark = self.size()

    def since(self):
        size = self.size()
        if size < self.mark:   # kestrel-gnd started again: a new log
            self.mark = 0
        out = self.ssh("tail -c +%d %s" % (self.mark + 1, LOG)) if size > self.mark else ""
        return out.splitlines()

    def wait(self, pattern, timeout, after_mark=True):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            for line in self.since() if after_mark else self.ssh("cat " + LOG).splitlines():
                m = rx.search(line)
                if m:
                    return m
            time.sleep(0.5)
        return None

    def kestrel_pid(self):
        out = self.ssh("pidof kestrel-gnd || true").split()
        return out[0] if out else None

    # --- keys ------------------------------------------------------------------
    def start_keys(self):
        with open(self.keysd, "rb") as f:
            self.ssh("cat > /tmp/kestrel-bench-keysd && chmod 755 /tmp/kestrel-bench-keysd", data=f.read())
        self.ssh("killall kestrel-bench-keysd 2>/dev/null; rm -f %s %s; "
                 "(setsid /tmp/kestrel-bench-keysd %s %s </dev/null >/dev/null 2>&1 &)" % (FIFO, ACK, FIFO, ACK))
        for _ in range(20):
            if "ready" in self.ssh("cat %s 2>/dev/null || true" % ACK):
                break
            time.sleep(0.25)
        else:
            raise Fail("keysd did not start")
        self.sent = 0
        time.sleep(3)   # kestrel-gnd rescans /dev/input every ~2 s

    def stop_keys(self):
        self.ssh("[ -p %s ] && echo quit > %s; sleep 0.5; killall kestrel-bench-keysd 2>/dev/null; true" % (FIFO, FIFO),
                 check=False)

    def keys(self, *names, gap=0.25):
        for n in names:
            if n in ("VOLUMEUP", "TAB"):
                raise Fail("refusing to send %s" % n)
        lines = []
        for n in names:
            lines.append(n)
            lines.append("sleep %d" % int(gap * 1000))
        # Synchronous: back only once keysd has pressed them all (one "ok"
        # line in the ack file per command), so a wait for a key's log line
        # starts when the key has really been pressed.
        self.sent += len(lines)
        self.ssh("cat > %s; n=0; while [ $(grep -c '^ok' %s) -lt %d ] && [ $n -lt 600 ]; do usleep 50000; n=$((n+1)); done"
                 % (FIFO, ACK, self.sent), data=("\n".join(lines) + "\n").encode(), timeout=40 + len(names))
        acked = int(self.ssh("grep -c '^ok' %s || true" % ACK).strip() or 0)
        if acked < self.sent:
            raise Fail("keysd acknowledged %d of %d commands" % (acked, self.sent))


class MenuTest:
    def __init__(self, g, args):
        self.g, self.args = g, args
        self.results = []      # (tab, row, value, ok, note)
        self.side = None       # blade the menu is on: "L" or "R"
        self.open = False

    def note(self, tab, row, value, ok, why=""):
        self.results.append((tab, row, value, ok, why))
        print("  %-7s %-22s %-22s %s %s" % (tab, row[:22], str(value)[:22], "ok  " if ok else "FAIL", why), flush=True)

    # --- health ----------------------------------------------------------------
    def video_ok(self, timeout=15):
        """Video flows: a 5 s decoder count of at least 100 pictures after the mark."""
        end = time.time() + timeout
        while time.time() < end:
            for line in self.g.since():
                m = PICTURES_RE.search(line)
                if m and int(m.group(1)) >= 100:
                    return True
            time.sleep(1)
        return False

    def linked(self):
        out = self.g.ssh("grep -E 'link state [0-9-]+ -> [0-9]' %s | tail -1" % LOG).strip()
        return out.endswith("(linked)") or "-> 2" in out

    def healthy(self, restarted=False, video_timeout=15):
        pid = self.g.kestrel_pid()
        if not pid:
            return False, "kestrel-gnd not running"
        if not restarted and self.g.pid and pid != self.g.pid:
            return False, "kestrel-gnd restarted (pid %s -> %s)" % (self.g.pid, pid)
        self.g.pid = pid
        if not self.video_ok(video_timeout):
            return False, "no video within %d s" % video_timeout
        return True, ""

    def wait_restart(self, old_pid, timeout=60):
        """kestrel-gnd started again since the mark: in place (execv, the same
        pid: "kestrel-gnd: restarting" then its screen line) or by its init
        script (a new pid and a new log)."""
        end = time.time() + timeout
        while time.time() < end:
            pid = self.g.kestrel_pid()
            lines = self.g.since()
            text = "\n".join(lines)
            inplace = re.search(r"kestrel-gnd: restarting[\s\S]*display: screen", text)
            if pid and (pid != old_pid or inplace):
                self.g.pid = pid
                time.sleep(6)   # the app up, its input rescan done
                self.g.set_mark()
                self.open = False
                self.side = None
                return True
            time.sleep(1)
        return False

    # --- navigation --------------------------------------------------------------
    def close_menu(self):
        if self.open:
            self.g.keys("M")
            self.open = False
            self.side = None
            time.sleep(0.5)

    def goto_tab(self, blade, index):
        """Unfold blade (L: HUD DISPLAY DVR SYSTEM; R: [VIDEO] RADIO SERIAL INFO) at tab index."""
        self.close_menu()
        self.g.keys("M")                     # opens at stage 0
        self.open = True
        self.g.keys("LEFT" if blade == "L" else "RIGHT", "ENTER")
        self.g.keys(*(["LEFT"] * 4))         # clamps on the first tab, first row
        if index:
            self.g.keys(*(["RIGHT"] * index))
        self.side = blade
        time.sleep(0.4)

    def probe(self):
        """Enter on the current row: its (edit) line if it is a setting, else None. Leaves edit with Back."""
        self.g.set_mark()
        self.g.keys("ENTER")
        m = self.g.wait(MENU_RE.pattern, 2.5)
        if m and m.group(4) == "edit":
            return m.group(1), m.group(2), m.group(3)
        if m:   # (enter): an action ran - must never happen on the rows probed
            raise Fail("Enter ran an action: %s" % m.group(0))
        return None

    def probe_keep(self):
        """As probe(): the row's (edit) line, staying in edit."""
        return self.probe()

    def back(self):
        self.g.keys("ESC")   # the app's Back; the menu is open here (never on the live screen)

    def find_row(self, blade, indices, tab_name, row_name, skip_first=False, rows_max=16):
        """Unfold blade at each tab index in turn and probe down for row_name.
        Back on that row (out of edit) on success: its (tab index, position)."""
        for idx in indices:
            self.goto_tab(blade, idx)
            for pos in range(rows_max):
                if pos == 0 and skip_first:
                    self.g.keys("DOWN")
                    continue
                p = self.probe()
                if p:
                    self.back()
                    if p[0] != tab_name:
                        break
                    if p[1] == row_name:
                        return idx, pos
                self.g.keys("DOWN")
        return None

    def restore_dependent(self, tab, blade, index, rows, values):
        """Rows that change each other (Area resets Country, Country the zone):
        put back in order, each found by name."""
        for row in rows:
            if row not in values:
                continue
            at = self.find_row(blade, [index], tab, row)
            if not at:
                self.note(tab, row, values[row], False, "row not found to restore")
                continue
            self.restore_value(tab, row, values[row])

    # --- one setting row -----------------------------------------------------------
    def walk_values(self, tab, row, v0, special=None):
        """In edit on row (value v0): apply every value with Enter, check, then put v0 back."""
        seen = [v0]
        restarts = special == "restart"
        limit = self.args.max_values
        cur = v0
        for step in range(1, 200):
            self.g.set_mark()
            down_mark = self.g.mark
            self.g.keys("DOWN")
            m = self.g.wait(r"^menu: %s > %s: (.*) \(live\)$" % (re.escape(tab), re.escape(row)), 1.2)
            live = bool(m)
            # A live row's own line comes with the Down, before the Enter.
            self.down_text = "\n".join(self.g.since()) if live else ""
            self.g.set_mark()
            old_pid = self.g.kestrel_pid()
            self.g.keys("ENTER")
            m = self.g.wait(r"^menu: %s > %s: (.*) \(enter\)$" % (re.escape(tab), re.escape(row)), 4)
            if not m:
                self.note(tab, row, "?", False, "no (enter) line after Enter")
                return False
            val = m.group(1)
            if val == cur and step > 1 and not live:
                # A clamped list: Down did not move. Done with this direction.
                break
            if restarts:
                ok = self.handle_restart_row(tab, row, val, old_pid)
                return ok
            ok, why = self.check_applied(tab, row, val)
            self.note(tab, row, val, ok, why)
            cur = val
            if val in seen:
                break       # wrapped round to a value already seen (v0 at the end)
            seen.append(val)
            if limit and len(seen) > limit:
                self.note(tab, row, "...", True, "stopped after %d values (--max-values)" % limit)
                break
            # Edit again from the row for the next value.
            again = self.probe()
            if not again or again[1] != row:
                self.note(tab, row, val, False, "lost the row after applying (got %r)" % (again,))
                return False
            cur = again[2]
        return self.restore_value(tab, row, v0)

    def restore_value(self, tab, row, v0):
        """From focus 1 on row: edit, step until the value is v0, Enter."""
        cur = self.probe()
        if not cur or cur[1] != row:
            self.note(tab, row, v0, False, "could not edit the row again to restore")
            return False
        if cur[2] == v0:
            self.back()
            return True
        for direction in ("UP", "DOWN"):
            for _ in range(120):
                self.g.set_mark()
                self.g.keys(direction)
                self.g.set_mark()
                self.g.keys("ENTER")
                m = self.g.wait(r"^menu: %s > %s: (.*) \(enter\)$" % (re.escape(tab), re.escape(row)), 4)
                if m and m.group(1) == v0:
                    self.note(tab, row, v0, True, "restored")
                    return True
                again = self.probe()
                if not again:
                    break
            # try the other direction (clamped lists)
        self.note(tab, row, v0, False, "could not restore")
        return False

    def check_applied(self, tab, row, val):
        lines = self.g.since()
        text = getattr(self, "down_text", "") + "\n" + "\n".join(lines)
        extra = EXPECT.get((tab, row))
        if extra and not re.search(extra(val) if callable(extra) else extra, text):
            return False, "expected log line missing"
        ok, why = self.healthy(video_timeout=VIDEO_WAIT.get((tab, row), 12))
        return ok, why

    def handle_restart_row(self, tab, row, val, old_pid):
        """Screen Mode / VRX Radio Firmware: kestrel-gnd restarts with the value."""
        if not self.wait_restart(old_pid):
            self.note(tab, row, val, False, "kestrel-gnd did not restart")
            return False
        self.note(tab, row, val, True, "restarted with it")
        return True

    # --- tabs -------------------------------------------------------------------------
    def walk_tab(self, blade, index, name, rows_max=24, skip_rows=()):
        """Every setting row of a tab, found by probing each row in turn."""
        print("== %s" % name, flush=True)
        first = {}
        if name in DEPENDENT:
            # Every row's value before any is changed (one changes the next).
            self.goto_tab(blade, index)
            for pos in range(rows_max):
                p = self.probe() if pos not in skip_rows else None
                if p:
                    self.back()
                    if p[1] in first:
                        break
                    first[p[1]] = p[2]
                self.g.keys("DOWN")
        self.goto_tab(blade, index)
        last = None
        if 0 in skip_rows:
            # Row 0 must never be entered (RADIO: Channel, or Bind unlinked),
            # and Down wraps from the last row onto it: learn the last row
            # first (Up from row 0 wraps to it) and stop the walk there.
            self.g.keys("UP")
            p = self.probe()
            if not p or p[0] != name:
                raise Fail("%s: the last row is not a setting row, the walk could wrap onto row 0" % name)
            self.back()
            last = p[1]
            self.goto_tab(blade, index)
        done = set()
        for pos in range(rows_max):
            if pos in skip_rows:
                self.g.keys("DOWN")
                continue
            p = self.probe()
            if p:
                tab, row, v0 = p
                if tab != name:
                    raise Fail("expected tab %s, the menu is on %s" % (name, tab))
                if row in done:
                    break           # wrapped
                done.add(row)
                if row in SPECIAL_ROWS:
                    self.back()
                    SPECIAL_ROWS[row](self, tab, row, v0, blade, index, pos)
                    self.goto_tab(blade, index)
                    self.g.keys(*(["DOWN"] * (pos + 1)))
                    continue
                if row in SKIP_ROWS:
                    self.note(tab, row, v0, True, "skipped: " + SKIP_ROWS[row])
                    self.back()
                else:
                    self.walk_values(tab, row, v0)
                if row == last:
                    break           # the next Down would wrap onto row 0
            self.g.keys("DOWN")
        if name in DEPENDENT:
            self.restore_dependent(name, blade, index, DEPENDENT[name], first)
        if not done:
            self.note(name, "-", "-", True, "no setting rows (read-only tab)")
        return done


# Rows whose change shows in its own log line too (beyond the menu line).
EXPECT = {
    ("DISPLAY", "Frame Mode"): lambda v: r"menu: frame mode",
    ("DISPLAY", "Sync Margin"): r"menu: refresh sync margin",
    ("RADIO", "TX Power"): r"ar8030: TX power",
    ("RADIO", "TX Power Mode"): r"ar8030: TX power",
}
# Rows after which video takes longer to come back (camera or link re-forming).
VIDEO_WAIT = {
    ("VIDEO", "Video Mode"): 30, ("VIDEO", "Rotate"): 25, ("RADIO", "Max Bandwidth"): 30,
    ("RADIO", "Max MCS"): 20, ("RADIO", "Auto Standby"): 20, ("RADIO", "TX Power"): 20,
    ("RADIO", "TX Power Mode"): 20, ("DVR", "Source"): 15,
}
SKIP_ROWS = {}
# Rows that reset the ones after them: restored together, in this order.
DEPENDENT = {"SYSTEM": ["Area", "Country", "Timezone"]}


def screen_mode(t, tab, row, v0, blade, index, pos):
    """Each other mode on trial, turned down (Back), back to v0 each time."""
    if t.args.skip_restarts:
        t.note(tab, row, v0, True, "skipped (--skip-restarts)")
        return
    tried = 0
    for k in range(1, 12):
        t.goto_tab(blade, index)
        t.g.keys(*(["DOWN"] * pos))
        p = t.probe()
        if not p or p[1] != row:
            t.note(tab, row, "?", False, "row not found")
            return
        t.g.keys(*(["DOWN"] * k))
        old = t.g.kestrel_pid()
        t.g.set_mark()
        t.g.keys("ENTER")
        m = t.g.wait(r"menu: screen mode (.+?) on trial", 8)
        if not m:
            t.back()
            break   # back on v0: every mode seen
        mode = m.group(1)
        if mode == v0 or (tried and mode in t.screen_modes_seen):
            t.back()
            break
        t.screen_modes_seen.add(mode)
        if not t.wait_restart(old, 90):
            t.note(tab, row, mode, False, "no restart into the trial")
            return
        ok = t.video_ok(40)
        old = t.g.kestrel_pid()
        t.g.set_mark()
        t.g.keys("ESC")   # turn the mode down: back to the previous one
        back = t.g.wait(r"turned down - restarting in the previous one", 6)
        restarted = t.wait_restart(old, 90)
        t.note(tab, row, mode, ok and bool(back) and restarted,
               "on trial, video %s; turned down %s" % ("ok" if ok else "MISSING", "ok" if back and restarted else "FAILED"))
        if not restarted:
            return
        tried += 1
    ok, why = t.healthy(restarted=True, video_timeout=40)
    t.note(tab, row, v0, ok, "back on the kept mode " + why)


def refresh_sync(t, tab, row, v0, blade, index, pos):
    """OFF / NUDGE / FOLLOW, live: each set (a trial kept with Enter, which the
    trial takes without leaving the edit), until back on v0; then Back."""
    t.goto_tab(blade, index)
    t.g.keys(*(["DOWN"] * pos))
    p = t.probe_keep()
    if not p or p[1] != row:
        t.note(tab, row, "?", False, "row not found")
        return
    for _ in range(4):
        t.g.set_mark()
        t.g.keys("DOWN")
        m = t.g.wait(r"^menu: DISPLAY > Refresh Sync: (\S+) \(live\)$", 4)
        if not m:
            t.note(tab, row, "?", False, "no (live) line")
            break
        val = m.group(1)
        trial = t.g.wait(r"refresh sync %s on trial" % re.escape(val), 1.5)
        kept = True
        if trial:
            t.g.set_mark()
            t.g.keys("ENTER")       # keeps the trial; the edit stays open
            kept = bool(t.g.wait(r"refresh sync %s kept" % re.escape(val), 3))
        ok, why = t.healthy(video_timeout=15)
        t.note(tab, row, val, ok and kept, ("trial kept " if trial else "set ") + why)
        if val == v0:
            break
    t.back()                        # live: the value in place is v0


def channel_page(t, tab, row, v0, blade, index, pos):
    pass   # the channel row is an action row: tested by test_channel()


def radio_firmware(t, tab, row, v0, blade, index, pos):
    """VRX Radio Firmware: the other firmware (radio and kestrel-gnd restart,
    the link may not form with it), then back to v0 from the unlinked RADIO
    tab, where it is the second row (after Bind, which is never entered)."""
    if t.args.skip_restarts:
        t.note(tab, row, v0, True, "skipped (--skip-restarts)")
        return
    t.goto_tab(blade, index)
    t.g.keys(*(["DOWN"] * pos))
    p = t.probe()
    if not p or p[1] != row:
        t.note(tab, row, "?", False, "row not found")
        return
    t.g.keys("DOWN")
    old = t.g.kestrel_pid()
    t.g.set_mark()
    t.g.keys("ENTER")
    m = t.g.wait(r"^menu: RADIO > VRX Radio Firmware: (\S+) \(enter\)$", 4)
    other = m.group(1) if m else "?"
    restarted = t.wait_restart(old, 120)
    time.sleep(20)
    t.note(tab, row, other, restarted, "radio and kestrel-gnd restarted" if restarted else "no restart")
    if not restarted:
        return
    # Back to v0: the row found by name. Linked or not decides the blade's
    # tabs ([VIDEO] RADIO ...) and RADIO's rows; row 0 (Channel or Bind) is
    # never entered.
    t.video_ok(60)
    at = t.find_row("R", [1, 0], "RADIO", row, skip_first=True)
    if not at:
        t.note(tab, row, v0, False, "could not find the row to switch back: put ar8030_firmware back by hand")
        return
    p = t.probe()
    if not p or p[1] != row:
        t.note(tab, row, v0, False, "lost the row: put ar8030_firmware back by hand")
        return
    t.g.keys("DOWN")
    old = t.g.kestrel_pid()
    t.g.set_mark()
    t.g.keys("ENTER")
    m = t.g.wait(r"^menu: RADIO > VRX Radio Firmware: (\S+) \(enter\)$", 4)
    restarted = t.wait_restart(old, 120)
    ok, why = t.healthy(restarted=True, video_timeout=120) if restarted else (False, "no restart")
    t.note(tab, row, m.group(1) if m else "?", ok and m and m.group(1) == v0, "back, link and video " + (why or "ok"))


SPECIAL_ROWS = {"Screen Mode": screen_mode, "Refresh Sync": refresh_sync, "VRX Radio Firmware": radio_firmware}


def test_channel(t):
    """RADIO > Channel: the page opened, the link moved one channel and back."""
    print("== RADIO > Channel (page)", flush=True)
    if not t.linked():
        t.note("RADIO", "Channel", "-", False, "not linked: the first RADIO row would be Bind, not tested")
        return
    blade_r = 1 if t.video_tab else 0
    def at_channel_row():
        """On RADIO row 0 and it is Channel: the next row probes as TX Power Mode."""
        t.goto_tab("R", blade_r)
        t.g.keys("DOWN")
        p = t.probe()
        if p:
            t.back()
        t.g.keys("UP")
        return bool(p) and p[0] == "RADIO" and p[1] == "TX Power Mode" and t.linked()
    if not at_channel_row():
        t.note("RADIO", "Channel", "-", False, "position not confirmed: not entered")
        return
    start = t.g.ssh("grep -E '^ar8030_chan_mhz' %s || true" % YAML).strip()
    for move, key in (("away", "LEFT"), ("back", "RIGHT")):
        t.g.set_mark()
        t.g.keys("ENTER")                   # opens the channel page
        if not t.g.wait(r"channel page|chan_open|channel scan", 3):
            pass   # the page has no open line on every build; the move line below is the check
        t.g.keys(key)                       # the cursor one channel left, then back right
        t.g.set_mark()
        t.g.keys("ENTER")
        m = t.g.wait(r"channel page: (moving the link to (\d+) MHz|.*)", 5)
        t.g.keys("ESC")                     # closes the page if still open
        ok, why = t.healthy(video_timeout=40)
        t.note("RADIO", "Channel", m.group(2) if m and m.group(2) else (m.group(1) if m else "?"),
               bool(m) and ok, move + " " + why)
        t.close_menu()
        if move == "back":
            break
        if not at_channel_row():
            t.note("RADIO", "Channel", "-", False, "position not confirmed for the move back: not entered")
            break
    end = t.g.ssh("grep -E '^ar8030_chan_mhz' %s || true" % YAML).strip()
    if start and end != start:
        t.note("RADIO", "Channel", end, False, "saved channel %s, was %s (restored from the backup at the end)" % (end, start))


def settings_diff(before, after):
    def parse(text):
        d = {}
        for line in text.splitlines():
            if ":" in line and not line.startswith("#"):
                k, v = line.split(":", 1)
                d[k.strip()] = v.strip()
        return d
    def same(k, x, y):
        norm = {"false": "0", "true": "1"}
        x, y = norm.get(x, x), norm.get(y, y)
        if k.endswith("_dnr3d") and {x, y} <= {"1", "5"}:
            return True     # both the Lite camera's automatic level (kestrel-air image_set_3dnr)
        return x == y
    b, a = parse(before), parse(after)
    diffs, written = [], []
    for k in sorted(set(b) | set(a)):
        if VOLATILE.match(k):
            continue
        if k not in b:
            written.append((k, a[k]))       # a default written out: not a change
        elif k not in a or not same(k, b[k], a[k]):
            diffs.append((k, b.get(k), a.get(k)))
    return diffs, written


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.3.1")
    ap.add_argument("--password", default="fpvos")
    ap.add_argument("--keysd", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "keysd"))
    ap.add_argument("--only", default="", help="comma list of tabs (VIDEO,RADIO,HUD,DISPLAY,DVR,SYSTEM,INFO,SERIAL,CHANNEL,FIRMWARE)")
    ap.add_argument("--skip-restarts", action="store_true", help="no Screen Mode trials, no radio firmware switch")
    ap.add_argument("--max-values", type=int, default=0, help="at most N values a row (0: all)")
    ap.add_argument("--report", default="")
    args = ap.parse_args()
    only = set(x.strip().upper() for x in args.only.split(",") if x.strip())

    g = Goggle(args.host, args.password, args.keysd)
    t = MenuTest(g, args)
    t.screen_modes_seen = set()
    before = g.ssh("cat " + YAML)
    g.ssh("cp -p %s %s.pre-menutest-run" % (YAML, YAML))
    # A fresh kestrel-gnd: the menu closed, in a known state.
    old = g.kestrel_pid()
    g.ssh("/etc/init.d/S70kestrel stop >/dev/null 2>&1; sleep 2; for p in $(pidof kestrel-gnd); do kill $p; done; "
          "sleep 1; (setsid /etc/init.d/S70kestrel start </dev/null >/dev/null 2>&1 &)")
    for _ in range(30):
        g.pid = g.kestrel_pid()
        if g.pid and g.pid != old:
            break
        time.sleep(1)
    if not g.pid:
        raise SystemExit("kestrel-gnd is not running")
    g.mark = 0
    if not t.video_ok(180):
        raise SystemExit("no video after the start: link an air unit first")
    t.video_tab = t.linked()
    print("kestrel-gnd pid %s, linked: %s" % (g.pid, t.video_tab), flush=True)
    g.start_keys()
    failed = False
    try:
        # Left blade: HUD, DISPLAY, DVR, SYSTEM.
        for i, name in enumerate(("HUD", "DISPLAY", "DVR", "SYSTEM")):
            if only and name not in only:
                continue
            skip = (3,) if name == "DVR" else ()     # Format DVR: never Enter there
            t.walk_tab("L", i, name, skip_rows=skip, rows_max=8 if name == "DVR" else 24)
        # Right blade: [VIDEO], RADIO, SERIAL, INFO.
        right = (["VIDEO"] if t.video_tab else []) + ["RADIO", "SERIAL", "INFO"]
        for i, name in enumerate(right):
            if only and name not in only:
                continue
            # RADIO row 0 is Channel (linked) or Bind (not): never probed here.
            skip = (0,) if name == "RADIO" else ()
            if name == "RADIO" and not t.linked():
                t.note("RADIO", "-", "-", False, "not linked: RADIO not walked (Bind would be row 0)")
                continue
            t.walk_tab("R", i, name, skip_rows=skip, rows_max={"SERIAL": 6, "INFO": 16}.get(name, 24))
        if not only or "CHANNEL" in only:
            test_channel(t)
    except Fail as e:
        failed = True
        print("STOPPED: %s" % e, flush=True)
    finally:
        try:
            t.close_menu()
        except Exception:
            pass
        g.stop_keys()
    after = g.ssh("cat " + YAML)
    diffs, written = settings_diff(before, after)
    bad = [r for r in t.results if not r[3]]
    lines = ["# VRX Pro menu test", "",
             "%d checks, %d failed%s" % (len(t.results), len(bad), " (stopped early)" if failed else ""), ""]
    lines += ["| Tab | Row | Value | Result | Note |", "|---|---|---|---|---|"]
    lines += ["| %s | %s | %s | %s | %s |" % (a, b, c, "ok" if d else "**FAIL**", e) for a, b, c, d, e in t.results]
    lines += ["", "Settings changed by the test (not counting measured values): %d" % len(diffs)]
    lines += ["- `%s`: %s -> %s" % d for d in diffs]
    if written:
        lines += ["", "Keys written out with their value (absent before): " + ", ".join("`%s: %s`" % w for w in written)]
    text = "\n".join(lines) + "\n"
    print(text)
    if args.report:
        with open(args.report, "w") as f:
            f.write(text)
    sys.exit(1 if bad or failed or diffs else 0)


if __name__ == "__main__":
    main()
