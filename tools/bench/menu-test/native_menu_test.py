#!/usr/bin/env python3
"""Walk every setting of a native AR9311 receiver's menu and check each one.

For the receivers on the LVGL UI (hud/lvgl): Skyzone SteadyDigital, Ascent
VRX, P1 goggles. Keys go through kestrel-gnd's control socket
(--send-key), whose reply is the menu state (open, stage, side, tab, row),
so every key's effect is checked before the next. The native menu logs no
row changes: a change is seen in the settings files (ui.conf for the
receiver's own UI rows, receiver.conf for the radio and air-unit rows) and
in the radio's own log lines ("set ... = ...", "TX power ...", "caps: ...").

  * every editable row of every tab: each value applied in turn (Down,
    Enter); the change seen in a settings file or the log; kestrel-gnd
    still running; with a link, video still flowing; until the settings
    are back where the row started;
  * Screen Mode: each other mode on trial (the session restarts into it),
    not kept, so it reverts by itself after 15 s;
  * RADIO > Channel (with a link): the page, one channel away and back;
  * at the end both settings files compared with the ones at the start,
    then put back from the copies (the menu cannot restore Area, Country,
    Timezone alone, and AUTO on the channel page leaves a channel pinned).

Never: Enter on Bind, Record, Gallery, Format SD Card, the channel page
outside its own test; Back on the live screen (gallery); the link, bind or
upgrade keys (not on the socket anyway).

  python3 native_menu_test.py [--host 192.168.3.104] [--iface enx...] [--only HUD,DISPLAY]
                              [--skip-restarts] [--report FILE]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

KEY = "/opt/kestrel/bin/kestrel-gnd --control /run/kestrel-gnd/control --send-key"
LOG = "/run/kestrel-gnd/current.log"
UI = "/var/lib/kestrel/ui.conf"
RECV = "/var/lib/kestrel/receiver.conf"
STATUS_RE = re.compile(r"OK open=(\d) stage=(\w+) side=(\w+) tab=(\w+) row=(\d+)")
# Settings that change by themselves.
VOLATILE = re.compile(r"^(camera_period_us_|ar8030_last_peer|screen_clock|frame_clock|.*_ppb|sky_[0-9A-F]+_(air|prj))")
# Rows Enter must never be pressed on, by (tab, row index). RADIO row 0 is
# Channel linked, Bind unlinked; DVR's are actions (record, gallery, format).
ACTIONS = {"RADIO": {0}, "DVR": {1, 2, 5}}
TABS = {"ground": ["HUD", "DISPLAY", "DVR", "SYSTEM"], "air": ["VIDEO", "RADIO", "SERIAL", "INFO"]}
# Rows not saved anywhere (only a log line): walked this many values, which
# brings them back. Auto Standby: OFF/ON, momentary.
LOG_ONLY = {"set standby": 2}
# Keys only the area/country/timezone rows change: the menu cannot bring one
# back alone (Area resets Country, Country the zone), so the walk stops when
# its values repeat and the file copy puts them back at the end.
ZONE_KEYS = {"time_zone"}


class Fail(Exception):
    pass


class Unit:
    def __init__(self, host, password, iface):
        self.host, self.password, self.iface = host, password, iface
        self.cm = os.path.join(tempfile.mkdtemp(prefix="native-test-"), "cm")

    def ssh(self, cmd, timeout=60, check=True):
        args = ["sshpass", "-p", self.password, "ssh", "-q", "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=5",
                "-o", "ControlMaster=auto", "-o", "ControlPath=" + self.cm, "-o", "ControlPersist=900"]
        if self.iface:
            proxy = os.path.join(os.path.dirname(os.path.abspath(__file__)), "devproxy.py")
            args += ["-o", "ProxyCommand=python3 %s %s %%h %%p" % (proxy, self.iface)]
        r = subprocess.run(args + ["root@" + self.host, cmd], capture_output=True, timeout=timeout)
        if check and r.returncode:
            raise Fail("ssh %r: %s" % (cmd, r.stderr.decode(errors="replace").strip()))
        return r.stdout.decode(errors="replace")

    def key(self, name):
        """One key; the menu state after it: (open, stage, side, tab, row)."""
        for attempt in range(20):
            out = self.ssh("%s %s 2>&1 || true" % (KEY, name))
            m = STATUS_RE.search(out)
            if m:
                return m.group(1) == "1", m.group(2), m.group(3), m.group(4), int(m.group(5))
            time.sleep(0.5)     # the session restarting: no socket yet
        raise Fail("no reply from the control socket to %r" % name)

    def status(self):
        return self.key("status")

    def conf(self):
        out = self.ssh("cat %s %s 2>/dev/null" % (UI, RECV))
        d = {}
        for line in out.splitlines():
            if "=" in line and ":" not in line:
                k, v = line.split("=", 1)
            elif ":" in line:
                k, v = line.split(":", 1)
            else:
                continue
            k = k.strip()
            if not VOLATILE.match(k):
                d[k] = v.strip()
        return d

    def log_size(self):
        return int(self.ssh("wc -c < %s" % LOG).strip() or 0)

    def log_since(self, mark):
        size = self.log_size()
        if size < mark:
            mark = 0
        return self.ssh("tail -c +%d %s" % (mark + 1, LOG)).splitlines() if size > mark else []

    def pid(self):
        out = self.ssh("pidof kestrel-gnd || true").split()
        return out[0] if out else None


class NativeTest:
    def __init__(self, u, args):
        self.u, self.args = u, args
        self.results = []

    def note(self, tab, row, value, ok, why=""):
        self.results.append((tab, str(row), str(value), ok, why))
        print("  %-7s %-14s %-34s %s %s" % (tab, row, str(value)[:34], "ok  " if ok else "FAIL", why), flush=True)

    # --- health --------------------------------------------------------------------
    def linked(self):
        out = self.u.ssh("grep -E 'link state [0-9-]+ -> [0-9]' %s | tail -1" % LOG).strip()
        return out.endswith("(linked)")

    def video_ok(self, timeout=15):
        """Video flowing: the decoder's count growing by 100+ in 5 s, or the
        radio reporting video above 1 Mbit/s (strips release the decoder)."""
        end = time.time() + timeout
        mark = self.u.log_size()
        first = None
        while time.time() < end:
            for line in self.u.log_since(mark):
                m = re.search(r"v4l2: decoded=(\d+)", line)
                if m:
                    n = int(m.group(1))
                    if first is not None and n - first >= 100:
                        return True
                    first = n if first is None or n < first else first
                m = re.search(r"ar8030: rx=.* video=([\d.]+) Mbps", line)
                if m and float(m.group(1)) > 1.0:
                    return True
            time.sleep(1)
        return False

    def healthy(self, want_video):
        if not self.u.pid():
            return False, "kestrel-gnd not running"
        if want_video and not self.video_ok(15):
            return False, "no video within 15 s"
        return True, ""

    # --- navigation -------------------------------------------------------------------
    def close(self):
        for _ in range(4):
            if not self.u.status()[0]:
                return
            self.u.key("menu")
        raise Fail("the menu does not close")

    def goto(self, side, tab, row=None):
        """Menu open on tab (Browse), on row (index incl. headings) if given."""
        self.close()
        s = self.u.key("menu")
        if not s[0] or s[1] != "pick":
            raise Fail("menu did not open at pick: %r" % (s,))
        s = self.u.key("left" if side == "ground" else "right")
        s = self.u.key("enter")
        for _ in range(4):
            s = self.u.key("left")
        tabs = [t for t in TABS[side] if t != "VIDEO" or self.has_video]
        for _ in range(tabs.index(tab)):
            s = self.u.key("right")
        if s[3] != tab or s[1] != "browse":
            raise Fail("expected %s in browse, got %r" % (tab, s))
        if row is not None:
            for _ in range(40):
                if s[4] == row:
                    break
                s = self.u.key("down")
            if s[4] != row:
                raise Fail("row %d not reached on %s" % (row, tab))
        return s

    def rows_of(self, side, tab):
        """The row indices Down visits on tab (headings skipped), in order."""
        s = self.goto(side, tab)
        seen = [s[4]]
        for _ in range(40):
            s = self.u.key("down")
            if s[4] in seen:
                break
            seen.append(s[4])
        return seen

    # --- one row ------------------------------------------------------------------------
    def walk_row(self, side, tab, row, want_video):
        if row in ACTIONS.get(tab, set()):
            self.note(tab, row, "-", True, "action row: not entered")
            return
        s = self.goto(side, tab, row)
        start = self.u.conf()
        mark = self.u.log_size()
        s = self.u.key("enter")
        if s[1] != "edit":
            self.note(tab, row, "-", True, "read-only")
            if s[1] != "browse":
                raise Fail("Enter on %s row %d left the menu at %r" % (tab, row, s))
            return
        self.u.key("back")              # out of edit: it was only the probe
        if tab == "DISPLAY" and row == 6:
            return self.screen_mode(side, tab, row, start)
        history = []
        for step in range(80):
            s = self.goto(side, tab, row)
            if self.u.key("enter")[1] != "edit":
                self.note(tab, row, "?", False, "no longer editable")
                return
            mark = self.u.log_size()
            self.u.key("down")
            s = self.u.key("enter")
            time.sleep(1.5)             # radio/air settings: queued, saved on ack
            now = self.u.conf()
            lines = "\n".join(self.u.log_since(mark))
            changed = {k: now.get(k) for k in set(now) | set(start) if now.get(k) != start.get(k)}
            said = re.findall(r"ar8030: (set [^\n]*|TX power [^\n]*|caps: [^\n]*|menu requested mode [^\n]*|RF channel[^\n]*)", lines)
            shown = ", ".join("%s=%s" % kv for kv in sorted(changed.items())) or (said[0] if said else "")
            ok, why = self.healthy(want_video and self.linked())
            rejected = "REJECTED" in lines
            if rejected:
                ok, why = False, "the air unit rejected it"
            if not changed and not said:
                if step > 0:
                    # Back to the first value: the settings as they were.
                    self.note(tab, row, "first value", ok, "back where it started " + why)
                    break
                ok, why = False, "no change seen (settings files, log)"
            if not changed and said and step > 0 and history and history[-1] == said[0]:
                # Nothing new: a value the air unit already runs; count it once.
                pass
            log_only = next((n for k, n in LOG_ONLY.items() if said and said[0].startswith(k)), 0)
            self.note(tab, row, shown or "?", ok, why)
            key = tuple(sorted(changed.items()))
            if log_only:
                if step + 1 >= log_only:
                    break       # that many values: back on the first
                continue
            if not changed:
                break           # back where it started
            if key in history:
                absent = {k for k in changed if k not in start}
                if absent and set(changed) <= absent | ZONE_KEYS and key == history[0]:
                    # The row's setting was not saved at the start (its default):
                    # writing the default shows as a change, so the start value
                    # is the one before the first step. Up once puts it back.
                    self.goto(side, tab, row)
                    self.u.key("enter")
                    self.u.key("up")
                    self.u.key("enter")
                    time.sleep(1.5)
                    back = tuple(sorted((k, v) for k, v in self.u.conf().items()
                                        if k in changed and v != start.get(k)))
                    ok, why = self.healthy(want_video and self.linked())
                    self.note(tab, row, "first value", ok and back == history[-1],
                              "back on the default (now saved) " + why if back == history[-1]
                              else "Up did not reach the value before the first step: %r" % (back,))
                elif set(changed) <= ZONE_KEYS:
                    self.note(tab, row, shown, True, "zone rows: put back from the copy at the end")
                else:
                    self.note(tab, row, shown, False, "cycled without returning to the first value")
                break
            history.append(key)
        self.close()

    def screen_mode(self, side, tab, row, start):
        if self.args.skip_restarts:
            self.note(tab, row, "-", True, "skipped (--skip-restarts)")
            return
        for k in range(1, 8):
            self.goto(side, tab, row)
            self.u.key("enter")
            for _ in range(k):
                self.u.key("down")
            mark = self.u.log_size()
            self.u.key("enter")
            time.sleep(3)
            lines = "\n".join(self.u.log_since(mark))
            if "source starting" not in lines and "display presentations" not in lines:
                self.u.key("back")
                self.note(tab, row, "mode +%d" % k, True, "the running mode again: every mode tried")
                break
            # On trial: not kept, so the session reverts by itself after 15 s.
            video = self.video_ok(25)
            time.sleep(18)
            ok, why = self.healthy(True)
            same = self.u.conf().get("ascent_screen_hz") == start.get("ascent_screen_hz") and \
                self.u.conf().get("ascent_screen_height") == start.get("ascent_screen_height")
            self.note(tab, row, "mode +%d" % k, video and ok and same,
                      "on trial with video %s, reverted %s %s" % ("ok" if video else "MISSING", "ok" if same else "NOT", why))
        self.close()

    def channel(self):
        if not self.linked():
            self.note("RADIO", 0, "-", False, "not linked: row 0 would be Bind, not entered")
            return
        # The page lists AUTO first, then the scan's channels by frequency.
        # From AUTO (no fixed channel saved) the move away is to a channel and
        # back is AUTO again; from a channel it is the one below and back.
        auto = int(self.u.conf().get("ar8030_chan_mhz") or 0) <= 0
        moves = (("away", "right"), ("back", "left")) if auto else (("away", "left"), ("back", "right"))
        for move, key in moves:
            s = self.goto("air", "RADIO", 0)
            if not self.linked():
                self.note("RADIO", 0, "-", False, "link dropped: not entered")
                return
            mark = self.u.log_size()
            self.u.key("enter")         # the channel page
            self.u.key(key)
            self.u.key("enter")         # moves the link
            time.sleep(3)
            lines = "\n".join(self.u.log_since(mark))
            m = re.search(r"set_rf_channel freq=(\d+)|(searching all \d+ channels)", lines)
            self.u.key("back")
            ok, why = self.healthy(True)
            self.note("RADIO", "Channel", (m.group(1) or "AUTO") if m else "?", bool(m) and ok, move + " " + why)
            self.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.3.104")
    ap.add_argument("--password", default="root")
    ap.add_argument("--iface", default="", help="USB network interface (two units on 192.168.3.x)")
    ap.add_argument("--only", default="")
    ap.add_argument("--skip-restarts", action="store_true")
    ap.add_argument("--report", default="")
    args = ap.parse_args()
    only = set(x.strip().upper() for x in args.only.split(",") if x.strip())
    u = Unit(args.host, args.password, args.iface)
    t = NativeTest(u, args)
    u.ssh("cp -p %s %s.pre-menutest && cp -p %s %s.pre-menutest" % (UI, UI, RECV, RECV))
    before = u.conf()
    t.has_video = t.linked()
    print("kestrel-gnd pid %s, linked: %s" % (u.pid(), t.has_video), flush=True)
    stopped = False
    try:
        for side in ("ground", "air"):
            for tab in TABS[side]:
                if (only and tab not in only) or (tab == "VIDEO" and not t.has_video):
                    continue
                if side == "air" and tab == "RADIO" and not t.linked():
                    t.note("RADIO", "-", "-", True, "not linked: Bind and VRX Radio Firmware only, not walked")
                    continue
                print("== %s" % tab, flush=True)
                for row in t.rows_of(side, tab):
                    t.walk_row(side, tab, row, t.has_video)
        if t.has_video and (not only or "CHANNEL" in only):
            print("== RADIO > Channel (page)", flush=True)
            t.channel()
    except Fail as e:
        stopped = True
        print("STOPPED: %s" % e, flush=True)
    finally:
        try:
            t.close()
        except Exception:
            pass
    after = u.conf()
    diffs = [(k, before.get(k), after.get(k)) for k in sorted(set(before) | set(after)) if before.get(k) != after.get(k)]
    # Back to the start, whatever the menu could not restore by itself.
    if diffs:
        u.ssh("/etc/init.d/S60kestrel-gnd stop >/dev/null 2>&1; sleep 1; cp -p %s.pre-menutest %s; cp -p %s.pre-menutest %s; "
              "(setsid /etc/init.d/S60kestrel-gnd start </dev/null >/dev/null 2>&1 &)" % (UI, UI, RECV, RECV), timeout=60)
    bad = [r for r in t.results if not r[3]]
    lines = ["# Native receiver menu test", "",
             "%d checks, %d failed%s" % (len(t.results), len(bad), " (stopped early)" if stopped else ""), "",
             "| Tab | Row | Value | Result | Note |", "|---|---|---|---|---|"]
    lines += ["| %s | %s | %s | %s | %s |" % (a, b, c, "ok" if d else "**FAIL**", e) for a, b, c, d, e in t.results]
    lines += ["", "Settings that differed at the end (put back from the copies): %d" % len(diffs)]
    lines += ["- `%s`: %s -> %s" % d for d in diffs]
    text = "\n".join(lines) + "\n"
    print(text)
    if args.report:
        open(args.report, "w").write(text)
    sys.exit(1 if bad or stopped else 0)


if __name__ == "__main__":
    main()
