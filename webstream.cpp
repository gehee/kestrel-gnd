#include "webstream.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "utils/time_util.h"
#include "dvr.hpp"         // also brings utils/minimp4.h, which must be included once
#include "hud_theme.hpp"
#include "settings.hpp"

#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <condition_variable>
#include <set>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

// ---- the page ---------------------------------------------------------------
//
// Two tabs, dressed in the HUD's current theme (/api/theme) and the app's own
// face (Chakra Petch, served from the image):
//
//   LIVE     /stream.mp4 through Media Source Extensions, a small jitter
//            buffer behind live (?buffer=ms), held by nudging the playback
//            rate. Appends in 'sequence' mode; only runs while its tab is open.
//   GALLERY  the DVR folder (/api/recordings), newest first; PLAY opens a
//            recording in the page, SAVE downloads it (/dvr/<name>, ranges).
static const char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>fpvOS</title>
<style>
@font-face{font-family:Chakra;src:url(/font/ChakraPetch-Medium.ttf);font-weight:500}
@font-face{font-family:Chakra;src:url(/font/ChakraPetch-SemiBold.ttf);font-weight:600}
:root{--ground:#06202f;--text:#ffffff;--data:#00e5ff;--accent:#b3ff00;--quiet:#8c9eb3}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;background:var(--ground);color:var(--text);
  font:500 15px Chakra,-apple-system,system-ui,sans-serif;-webkit-text-size-adjust:100%}
header{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:12px;
  padding:10px 16px;padding-top:max(10px,env(safe-area-inset-top));background:var(--ground);
  border-bottom:1px solid color-mix(in srgb,var(--quiet) 25%,transparent)}
.brand{font-weight:600;font-size:22px;letter-spacing:.02em}
.brand b{color:var(--accent);font-weight:600}
nav{display:flex;gap:6px;margin-left:auto}
nav button{font:600 13px Chakra,system-ui;letter-spacing:.14em;padding:8px 14px;border-radius:8px;
  border:1px solid color-mix(in srgb,var(--quiet) 40%,transparent);background:none;color:var(--quiet)}
nav button.on{color:var(--ground);background:var(--accent);border-color:var(--accent)}
main{padding:14px 16px calc(24px + env(safe-area-inset-bottom))}
section{display:none}section.on{display:block}
.frame{position:relative;background:#000;border-radius:10px;overflow:hidden;aspect-ratio:16/9}
.frame video{width:100%;height:100%;object-fit:contain;display:block}
#b{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);display:none;padding:12px 22px;
  font:600 15px Chakra,system-ui;letter-spacing:.1em;border:0;border-radius:10px;background:var(--accent);color:var(--ground)}
.status{margin-top:10px;color:var(--quiet);font-size:13px;line-height:1.4}
#list{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:14px}
.rec{border-radius:12px;overflow:hidden;background:color-mix(in srgb,var(--quiet) 12%,transparent)}
.shot{position:relative;aspect-ratio:16/9;background:color-mix(in srgb,var(--quiet) 18%,transparent)}
.thumb{display:block;width:100%;height:100%;object-fit:cover}
.thumb.play{cursor:pointer}
.badge{position:absolute;padding:3px 8px;border-radius:6px;background:rgba(0,0,0,.6);
  font:600 12px Chakra,system-ui;letter-spacing:.06em}
.badge.len{right:8px;bottom:8px}
.badge.rec-on{left:8px;top:8px;color:var(--accent)}
.info{display:flex;justify-content:space-between;align-items:baseline;gap:10px;padding:10px 12px 0}
.info .t{font-weight:600}
.info .m{color:var(--quiet);font-size:13px;white-space:nowrap}
.acts{display:flex;gap:8px;padding:10px 12px 12px}
.acts .btn{flex:1;text-align:center}
.btn{font:600 12px Chakra,system-ui;letter-spacing:.12em;padding:9px 12px;border-radius:8px;cursor:pointer;
  border:1px solid var(--accent);color:var(--accent);background:none;text-decoration:none;white-space:nowrap}
.btn.solid{background:var(--accent);color:var(--ground)}
.btn.quiet{border-color:color-mix(in srgb,var(--quiet) 60%,transparent);color:var(--quiet)}
.empty{grid-column:1/-1;color:var(--quiet);padding:40px 0;text-align:center}
#player{position:fixed;inset:0;z-index:10;display:none;flex-direction:column;justify-content:center;
  padding:16px;padding-top:max(16px,env(safe-area-inset-top));background:color-mix(in srgb,var(--ground) 94%,transparent)}
#player.on{display:flex}
#player .bar{display:flex;align-items:center;gap:10px;margin-bottom:12px}
#player .bar .t{flex:1;font-weight:600}
#pv{width:100%;max-height:78vh;background:#000;border-radius:10px}
</style></head><body>
<header><div class="brand">fpv<b>OS</b></div>
<nav><button id="tl" class="on">LIVE</button><button id="tg">GALLERY</button></nav></header>
<main>
<section id="live" class="on">
  <div class="frame"><video id="v" muted playsinline autoplay disableremoteplayback></video>
  <button id="b">TAP TO PLAY</button></div>
  <div class="status" id="s">connecting...</div>
</section>
<section id="gallery"><div id="list"></div></section>
</main>
<div id="player"><div class="bar"><span class="t" id="pt"></span>
  <a class="btn" id="pd" download>SAVE</a><button class="btn solid" id="pc">CLOSE</button></div>
  <video id="pv" controls playsinline></video></div>
<script>
const $ = id => document.getElementById(id);
const params = new URLSearchParams(location.search);

// Dress the page in the HUD's current theme.
async function theme() {
  try {
    const t = await (await fetch('/api/theme', { cache: 'no-store' })).json();
    for (const k of ['ground', 'text', 'data', 'accent', 'quiet'])
      document.documentElement.style.setProperty('--' + k, t[k]);
  } catch (e) {}
}

// ---- live ---------------------------------------------------------------------
// Plays /stream.mp4 through Media Source Extensions, TARGET behind live
// (?buffer=ms), holding it by nudging the playback rate. It runs only while
// the Live tab is open: the stream takes most of what the goggle's WiFi can
// carry, and a recording playing in the gallery needs that room.
const live = (() => {
  const v = $('v'), s = $('s'), b = $('b');
  const TYPE = 'video/mp4; codecs="__CODEC__"';
  const MS = window.ManagedMediaSource || window.MediaSource;
  const RECOVER = params.get('recover') === '1';
  const TARGET = Math.min(2000, Math.max(40, parseInt(params.get('buffer') || '250', 10) || 250)) / 1000;
  const say = t => { s.textContent = t; };
  let session = 0, running = false, recoveries = 0, lastErr = '', teardown = () => {};
  b.onclick = () => { b.style.display = 'none'; v.play(); };

  const quality = () => {
    const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
    return q ? q.totalVideoFrames + ' decoded, ' + q.droppedVideoFrames + ' dropped' : '';
  };

  function open() {
    const id = ++session, alive = () => id === session && running;
    const ctl = new AbortController(), ms = new MS();
    let timer = 0, bytes = 0, t0 = performance.now();
    teardown = () => {
      clearInterval(timer); ctl.abort();
      try { v.removeAttribute('src'); v.load(); } catch (e) {}
    };
    const fail = why => {
      if (!alive()) return;
      session++; teardown();
      if (!RECOVER) { say('Stopped: ' + why + (quality() ? ' (' + quality() + ')' : '')); return; }
      recoveries++; lastErr = why;
      setTimeout(() => { if (running) open(); }, 300);
    };
    v.onerror = () => { const e = v.error; fail(e ? 'error ' + e.code + (e.message ? ': ' + e.message : '') : 'error'); };
    v.disableRemotePlayback = true;
    v.src = URL.createObjectURL(ms);
    ms.addEventListener('sourceopen', async () => {
      if (!alive()) return;
      const sb = ms.addSourceBuffer(TYPE);
      try { sb.mode = 'sequence'; } catch (e) {}
      const q = [];
      const pump = () => {
        if (!alive() || sb.updating || !q.length || ms.readyState !== 'open') return;
        try {
          if (v.buffered.length && v.currentTime - v.buffered.start(0) > 8) { sb.remove(0, v.currentTime - 3); return; }
          let n = 0; for (const c of q) n += c.length;
          const all = new Uint8Array(n); let o = 0;
          for (const c of q) { all.set(c, o); o += c.length; }
          q.length = 0;
          sb.appendBuffer(all);
        } catch (e) { fail('append ' + e.name); }
      };
      sb.addEventListener('updateend', pump);
      timer = setInterval(() => {
        if (!alive()) return;
        const dt = (performance.now() - t0) / 1000; t0 = performance.now();
        const mbps = (bytes * 8 / dt / 1e6).toFixed(1); bytes = 0;
        const rec = recoveries ? ' · recovered ' + recoveries + 'x (' + lastErr + ')' : '';
        if (!v.buffered.length) { say('Waiting for a keyframe... ' + mbps + ' Mbps' + rec); return; }
        const end = v.buffered.end(v.buffered.length - 1), lag = end - v.currentTime;
        if (lag > TARGET + 1.0) { v.currentTime = end - TARGET; v.playbackRate = 1; }
        else if (lag > TARGET * 1.5) v.playbackRate = 1.05;
        else if (lag > TARGET * 1.15) v.playbackRate = 1.02;
        else if (lag < TARGET * 0.5) v.playbackRate = 0.95;
        else if (lag < TARGET * 0.85) v.playbackRate = 0.98;
        else v.playbackRate = 1;
        if (v.paused) v.play().catch(() => { b.style.display = 'block'; });
        say(mbps + ' Mbps · ' + Math.round(lag * 1000) + ' ms behind live' +
            (v.videoWidth ? ' · ' + v.videoWidth + 'x' + v.videoHeight : '') + rec);
      }, 500);
      try {
        const r = await fetch('/stream.mp4', { cache: 'no-store', signal: ctl.signal });
        const rd = r.body.getReader();
        for (;;) {
          const { done, value } = await rd.read();
          if (done || !alive()) break;
          bytes += value.length; q.push(value); pump();
        }
      } catch (e) {}
      fail('stream ended');
    });
  }

  return {
    start() {
      if (running) return;
      if (!MS) { say('This browser has no Media Source support (iPhone needs iOS 17.1 or later).'); return; }
      if (!MS.isTypeSupported(TYPE)) { say('This browser cannot play ' + TYPE); return; }
      running = true; say('Connecting...'); open();
    },
    stop() { running = false; session++; teardown(); b.style.display = 'none'; },
  };
})();

// ---- gallery ------------------------------------------------------------------
const size = n => n >= 1e9 ? (n / 1e9).toFixed(1) + ' GB' : Math.max(1, Math.round(n / 1e6)) + ' MB';
const length = d => d > 0 ? Math.floor(d / 60) + ':' + String(Math.round(d % 60)).padStart(2, '0') : '';
// fpvOS_YYYYMMDD_HHMMSS.mp4 (kestrel_ from earlier builds) - the goggle's own
// clock, as shown on the HUD. Only the digits are read, so either name works.
function when(r) {
  const m = /(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/.exec(r.name);
  return m ? m[1] + '-' + m[2] + '-' + m[3] + ' ' + m[4] + ':' + m[5] + ':' + m[6] : r.name;
}
const url = r => '/dvr/' + encodeURIComponent(r.name);

async function gallery() {
  const l = $('list');
  if (!l.children.length) l.innerHTML = '<div class="empty">Loading...</div>';
  let j;
  try { j = await (await fetch('/api/recordings', { cache: 'no-store' })).json(); }
  catch (e) { l.innerHTML = '<div class="empty">Could not list the recordings.</div>'; return; }
  if (!j.recordings.length) { l.innerHTML = '<div class="empty">No recordings yet - press REC on the goggle.</div>'; return; }
  l.innerHTML = '';
  for (const r of j.recordings) {
    const d = document.createElement('div');
    d.className = 'rec';
    d.innerHTML = '<div class="shot"><img class="thumb" alt=""></div>' +
                  '<div class="info"><span class="t"></span><span class="m"></span></div><div class="acts"></div>';
    const shot = d.querySelector('.shot');
    thumb(d.querySelector('.thumb'), r);
    if (length(r.duration)) {
      const b = document.createElement('span'); b.className = 'badge len'; b.textContent = length(r.duration);
      shot.append(b);
    }
    if (r.recording) {
      const b = document.createElement('span'); b.className = 'badge rec-on'; b.textContent = '● RECORDING';
      shot.append(b);
    }
    d.querySelector('.t').textContent = when(r);
    d.querySelector('.m').textContent = size(r.size);
    const acts = d.querySelector('.acts');
    if (r.playable) {
      const p = document.createElement('button');
      p.className = 'btn solid'; p.textContent = 'PLAY'; p.onclick = () => play(r);
      acts.append(p);
    }
    if (!r.recording) {
      const a = document.createElement('a');
      a.className = 'btn'; a.textContent = 'SAVE'; a.href = url(r) + '?dl=1'; a.download = r.name;
      const x = document.createElement('button');
      x.className = 'btn quiet'; x.textContent = 'DELETE'; x.onclick = () => remove(r);
      acts.append(a, x);
    }
    if (!acts.children.length) acts.remove();
    l.append(d);
  }
}

// A frame of the recording, made on the goggle the first time it is asked for;
// until then the server answers 202 and the image tries again.
function thumb(img, r) {
  if (r.recording || !r.playable) return;
  let tries = 0;
  img.onerror = () => { if (++tries < 40) setTimeout(() => { img.src = '/thumb/' + encodeURIComponent(r.name) + '?t=' + tries; }, 700); };
  img.src = '/thumb/' + encodeURIComponent(r.name);
  img.classList.add('play');
  img.onclick = () => play(r);
}

// Anyone on the goggle's network can reach this, so it asks first; the server
// refuses the recording in progress, and anything outside the DVR folder.
async function remove(r) {
  if (!confirm('Delete the recording from ' + when(r) + '?\nThis cannot be undone.')) return;
  try {
    const res = await fetch(url(r), { method: 'DELETE' });
    if (!res.ok) alert('Could not delete it (' + res.status + ').');
  } catch (e) { alert('Could not delete it.'); }
  gallery();
}

function play(r) {
  $('pt').textContent = when(r);
  $('pd').href = url(r) + '?dl=1'; $('pd').download = r.name;
  const pv = $('pv');
  pv.src = url(r);
  $('player').classList.add('on');
  pv.play().catch(() => {});
}
$('pc').onclick = () => {
  const pv = $('pv');
  pv.pause(); pv.removeAttribute('src'); pv.load();
  $('player').classList.remove('on');
};

// ---- tabs ---------------------------------------------------------------------
function show(tab) {
  for (const [btn, sec] of [['tl', 'live'], ['tg', 'gallery']]) {
    $(btn).classList.toggle('on', sec === tab);
    $(sec).classList.toggle('on', sec === tab);
  }
  theme();
  if (tab === 'live') live.start(); else { live.stop(); gallery(); }
  history.replaceState(null, '', location.pathname + location.search + (tab === 'gallery' ? '#gallery' : ''));
}
$('tl').onclick = () => show('live');
$('tg').onclick = () => show('gallery');
show(location.hash === '#gallery' ? 'gallery' : 'live');
</script></body></html>
)HTML";

// ---- server -------------------------------------------------------------------

namespace {

constexpr size_t kQueueMax    = 16;                // pictures waiting for the thread
constexpr size_t kSkipBytes   = 4u << 20;          // backlog: skip to next keyframe
constexpr size_t kDropBytes   = 16u << 20;         // backlog: give up on the viewer
constexpr int    kMaxClients  = 16;               // Safari opens several per video
constexpr size_t kFileChunk   = 256u << 10;        // file bytes read ahead per client

struct Item {
    std::shared_ptr<std::vector<uint8_t>> au;
    bool key;
};

struct Client {
    int fd = -1;
    std::string in;                 // request, until the blank line
    std::vector<uint8_t> out;       // bytes not yet sent
    size_t out_off = 0;
    bool streaming = false;         // /stream.mp4, as opposed to a one-shot reply
    bool close_after = false;       // one-shot reply: close once `out` is sent
    bool need_key = true;           // skipping until a keyframe
    MP4E_mux_t* mux = nullptr;
    mp4_h26x_writer_t wr{};
    int dur = 750;                  // sample duration, 90 kHz ticks
    int64_t written = 0;            // bytes the muxer has emitted
    int file_fd = -1;               // a recording being served, read in chunks
    uint64_t file_left = 0;         // bytes of it still to read

    size_t pending() const { return out.size() - out_off; }
};

// minimp4 writes forward only in fragmented mode, except for an optional
// header rewrite when the file is closed - which a live stream never needs.
int mux_write(int64_t offset, const void* buf, size_t size, void* token) {
    Client* c = static_cast<Client*>(token);
    if (offset != c->written) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    c->out.insert(c->out.end(), p, p + size);
    c->written += (int64_t)size;
    return 0;
}

void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

// A recording name the server will touch: a plain file name, nothing that
// could climb out of the DVR folder.
bool safe_name(const std::string& n) {
    if (n.empty() || n.size() > 128 || n[0] == '.') return false;
    for (char ch : n)
        if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '_' || ch == '-')) return false;
    return true;
}

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// Big-endian fields of the boxes read here.
uint32_t be32(const uint8_t* b) { return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
uint64_t be64(const uint8_t* b) { return ((uint64_t)be32(b) << 32) | be32(b + 4); }

// The child boxes of [b, b + n): calls f(type, body, body size) for each.
template <typename F> void each_box(const uint8_t* b, size_t n, F f) {
    size_t o = 0;
    while (o + 8 <= n) {
        const uint32_t size = be32(b + o);
        if (size < 8 || size > n - o) break;
        f(b + o + 4, b + o + 8, (size_t)size - 8);
        o += size;
    }
}

// A fragmented MP4's length, from the last of its fragments: that fragment's
// decode time plus its samples' durations, in the track's timescale. -1 if the
// tail of the file holds no fragment that parses.
double last_fragment_end(int fd, uint64_t file_size, uint32_t scale, uint32_t trex_duration) {
    // A fragment is one picture, so the last moof lies within the last
    // picture's size of the end; a big I-frame is well under this.
    const uint64_t tail = std::min<uint64_t>(file_size, 4u << 20);
    std::vector<uint8_t> t(tail);
    if (pread(fd, t.data(), tail, (off_t)(file_size - tail)) != (ssize_t)tail) return -1;
    for (size_t o = tail >= 16 ? tail - 16 : 0; o-- > 0;) {
        if (memcmp(&t[o + 4], "moof", 4) != 0 || memcmp(&t[o + 12], "mfhd", 4) != 0) continue;
        const uint32_t size = be32(&t[o]);
        if (size < 16 || size > tail - o) continue;
        uint64_t start = 0, sum = 0;
        bool have_tfdt = false, have_trun = false;
        each_box(&t[o + 8], size - 8, [&](const uint8_t* type, const uint8_t* b, size_t n) {
            if (memcmp(type, "traf", 4) != 0) return;
            uint32_t def_duration = trex_duration;
            each_box(b, n, [&](const uint8_t* type, const uint8_t* b, size_t n) {
                if (n < 8) return;
                const uint32_t flags = be32(b) & 0xffffff;
                if (memcmp(type, "tfhd", 4) == 0) {
                    size_t f = 8;                        // version/flags, track_ID
                    if (flags & 0x01) f += 8;            // base-data-offset
                    if (flags & 0x02) f += 4;            // sample-description-index
                    if ((flags & 0x08) && f + 4 <= n) def_duration = be32(b + f);
                } else if (memcmp(type, "tfdt", 4) == 0) {
                    if (b[0] == 1 && n >= 12) start = be64(b + 4);
                    else start = be32(b + 4);
                    have_tfdt = true;
                } else if (memcmp(type, "trun", 4) == 0) {
                    const uint32_t count = be32(b + 4);
                    size_t f = 8;
                    if (flags & 0x001) f += 4;           // data-offset
                    if (flags & 0x004) f += 4;           // first-sample-flags
                    const size_t per = 4 * (!!(flags & 0x100) + !!(flags & 0x200) +
                                            !!(flags & 0x400) + !!(flags & 0x800));
                    if (!(flags & 0x100)) {
                        sum += (uint64_t)count * def_duration;
                    } else {
                        for (uint32_t i = 0; i < count && f + 4 <= n; i++, f += per) sum += be32(b + f);
                    }
                    have_trun = true;
                }
            });
        });
        if (have_tfdt && have_trun) return (double)(start + sum) / scale;
    }
    return -1;
}

// Length of an MP4 in seconds, or -1. A plain MP4 has it in its movie header,
// written last; a fragmented one writes its moov first, before there is a
// length to put in it, and says so again at the end - unless the recording was
// cut short - so without one it is read off the last fragment. The file is
// only walked by its top-level box headers until the moov: a recording can be
// hundreds of megabytes.
double mp4_duration(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    double secs = -1;
    uint64_t off = 0;
    if (fstat(fd, &st) == 0) {
        for (int guard = 0; guard < 64 && off + 8 <= (uint64_t)st.st_size; guard++) {
            uint8_t h[16];
            if (pread(fd, h, 16, (off_t)off) < 8) break;
            uint64_t size = be32(h);
            uint64_t hdr = 8;
            if (size == 1) {
                size = be64(h + 8);
                hdr = 16;
            } else if (size == 0) {
                size = (uint64_t)st.st_size - off;
            }
            if (size < hdr) break;
            if (memcmp(h + 4, "moov", 4) == 0) {
                if (size - hdr > (1u << 20)) break;      // no moov this DVR writes
                std::vector<uint8_t> m(size - hdr);
                if (pread(fd, m.data(), m.size(), (off_t)(off + hdr)) != (ssize_t)m.size()) break;
                uint32_t track_scale = 0, trex_duration = 0;
                bool fragmented = false;
                each_box(m.data(), m.size(), [&](const uint8_t* type, const uint8_t* b, size_t n) {
                    if (memcmp(type, "mvhd", 4) == 0 && n >= 32) {
                        const uint32_t scale = b[0] == 1 ? be32(b + 20) : be32(b + 12);
                        const uint64_t dur = b[0] == 1 ? be64(b + 24) : be32(b + 16);
                        if (scale && dur) secs = (double)dur / scale;
                    } else if (memcmp(type, "trak", 4) == 0 && !track_scale) {
                        each_box(b, n, [&](const uint8_t* type, const uint8_t* b, size_t n) {
                            if (memcmp(type, "mdia", 4) != 0) return;
                            each_box(b, n, [&](const uint8_t* type, const uint8_t* b, size_t n) {
                                if (memcmp(type, "mdhd", 4) == 0 && n >= 24)
                                    track_scale = b[0] == 1 ? be32(b + 20) : be32(b + 12);
                            });
                        });
                    } else if (memcmp(type, "mvex", 4) == 0) {
                        fragmented = true;
                        each_box(b, n, [&](const uint8_t* type, const uint8_t* b, size_t n) {
                            if (memcmp(type, "trex", 4) == 0 && n >= 16 && !trex_duration)
                                trex_duration = be32(b + 12);
                        });
                    }
                });
                if (secs < 0 && fragmented && track_scale)
                    secs = last_fragment_end(fd, (uint64_t)st.st_size, track_scale, trex_duration);
                break;
            }
            off += size;
        }
    }
    close(fd);
    return secs;
}

// ---- thumbnails -----------------------------------------------------------------
//
// One JPEG per recording, 640 px wide, a second in (or at the start of a
// shorter one), cached in <dvr>/.thumbs/<name>.jpg. Made by the image's own
// ffmpeg on a worker thread at the lowest priority, one at a time - about a
// second each on the goggle - and only when the gallery asks for one.

std::string dvr_dir() { return Settings::getInstance().getString("dvr_dir", "/media/dvr"); }
std::string thumb_dir() { return dvr_dir() + "/.thumbs"; }
std::string thumb_path(const std::string& name) { return thumb_dir() + "/" + name + ".jpg"; }

bool run_ffmpeg_thumb(const std::string& in, const std::string& out, const char* at) {
    const std::string vf = "scale=640:-2";   // full-width cards on a 2x-3x phone screen
    const char* argv[] = {"nice", "-n", "19", "/usr/bin/ffmpeg", "-nostdin", "-loglevel", "error",
                          "-ss", at, "-i", in.c_str(), "-frames:v", "1", "-vf", vf.c_str(),
                          "-q:v", "6", "-y", out.c_str(), nullptr};
    pid_t pid;   // ::environ comes from <unistd.h> (g++ defines _GNU_SOURCE)
    if (posix_spawnp(&pid, "nice", nullptr, nullptr, const_cast<char* const*>(argv), ::environ) != 0)
        return false;
    int st = 0;
    waitpid(pid, &st, 0);
    struct stat o;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 && stat(out.c_str(), &o) == 0 && o.st_size > 0;
}

class Thumbnailer {
    public:
        // Queue a recording; a no-op if it is queued or being made already.
        void request(const std::string& name) {
            std::lock_guard<std::mutex> lk(m_);
            if (!queued_.insert(name).second) return;
            q_.push_back(name);
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
            cv_.notify_one();
        }
    private:
        void run() {
            pthread_setname_np(pthread_self(), "webstream-thumb");
            // Made from a real-time thread (a recording stopping), and ffmpeg
            // would inherit that: `nice` does nothing to SCHED_FIFO, and a
            // real-time software decode right after every recording held up
            // the live picture's decoder threads, which are ordinary ones.
            struct sched_param sp = {};
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
            for (;;) {
                std::string name;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this] { return !q_.empty(); });
                    name = q_.front();
                    q_.pop_front();
                }
                mkdir(thumb_dir().c_str(), 0755);
                const std::string in = dvr_dir() + "/" + name, out = thumb_path(name);
                const std::string tmp = out + ".tmp.jpg";
                const bool ok = run_ffmpeg_thumb(in, tmp, "1") || run_ffmpeg_thumb(in, tmp, "0");
                if (ok) rename(tmp.c_str(), out.c_str());
                else { unlink(tmp.c_str()); printf("webstream: no thumbnail for %s\n", name.c_str()); }
                std::lock_guard<std::mutex> lk(m_);
                queued_.erase(name);
            }
        }
        std::mutex m_;
        std::condition_variable cv_;
        std::deque<std::string> q_;
        std::set<std::string> queued_;
        std::thread worker_;
};

Thumbnailer& thumbnailer() { static Thumbnailer t; return t; }

std::string hex_colour(const float c[3]) {
    char b[8];
    snprintf(b, sizeof(b), "#%02x%02x%02x", (int)lroundf(c[0] * 255), (int)lroundf(c[1] * 255),
             (int)lroundf(c[2] * 255));
    return b;
}

// Annex-B (start codes) to an MP4 sample (4-byte big-endian length per NAL).
void annexb_to_sample(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.clear();
    const size_t n = in.size();
    size_t i = 0, start = SIZE_MAX;
    auto emit = [&](size_t a, size_t b) {
        while (b > a && in[b - 1] == 0) b--;           // trailing zero bytes of the next start code
        if (b <= a) return;
        const uint32_t len = (uint32_t)(b - a);
        const uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
        out.insert(out.end(), be, be + 4);
        out.insert(out.end(), in.begin() + (long)a, in.begin() + (long)b);
    };
    while (i + 3 <= n) {
        if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
            if (start != SIZE_MAX) emit(start, i);
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX) emit(start, n);
}

} // namespace

struct WebStream::Impl {
    int listen_fd = -1;
    int wake_fd = -1;
    pthread_t tid = 0;
    std::atomic<bool> run{false};
    std::atomic<int> viewers{0};   // streaming clients, read lock-free by feed()

    VideoCodec codec = VideoCodec::H265;

    std::mutex m;                  // guards everything below
    std::deque<Item> q;
    std::vector<std::vector<uint8_t>> ps;
    int w = 1920, h = 1080;
    uint64_t last_us = 0;
    double frame_us = 1e6 / 60;    // measured picture interval, EWMA

    std::vector<Client*> clients;  // server thread only

    void loop();
    void accept_one();
    void on_readable(Client* c);
    void on_request(Client* c, const std::string& method, const std::string& path,
                    const std::string& query, const std::string& req);
    void serve_file(Client* c, const std::string& method, const std::string& path,
                    const char* type, const std::string& req, const char* extra);
    std::string recordings_json();
    void flush(Client* c);
    void mux_item(Client* c, const Item& it,
                  const std::vector<std::vector<uint8_t>>& ps_now, int w_now, int h_now,
                  int dur_now);
    void drop(Client* c);
};

WebStream& WebStream::instance() {
    static WebStream s;
    return s;
}

bool WebStream::start(int port, VideoCodec codec) {
    if (impl_) return true;
    Impl* d = new Impl();
    d->codec = codec;

    d->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(d->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (d->listen_fd < 0 || bind(d->listen_fd, (sockaddr*)&a, sizeof(a)) < 0 ||
        listen(d->listen_fd, 8) < 0) {
        printf("webstream: cannot listen on port %d: %s\n", port, strerror(errno));
        if (d->listen_fd >= 0) close(d->listen_fd);
        delete d;
        return false;
    }
    set_nonblock(d->listen_fd);
    d->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    d->run = true;
    impl_ = d;
    pthread_create(&d->tid, nullptr, [](void* p) -> void* {
        pthread_setname_np(pthread_self(), "webstream");
        static_cast<Impl*>(p)->loop();
        return nullptr;
    }, d);
    printf("webstream: serving live video on http://<goggle>:%d/\n", port);
    return true;
}

void WebStream::stop() {
    Impl* d = impl_;
    if (!d) return;
    d->run = false;
    uint64_t one = 1;
    if (write(d->wake_fd, &one, sizeof(one)) < 0) {}
    pthread_join(d->tid, nullptr);
    for (Client* c : d->clients) d->drop(c);
    d->clients.clear();
    close(d->listen_fd);
    close(d->wake_fd);
    impl_ = nullptr;
    delete d;
}

void WebStream::set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->ps = ps;
}

void WebStream::set_frame_size(int w, int h) {
    if (!impl_ || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->w = w;
    impl_->h = h;
}

void WebStream::recording_finished(const std::string& name) {
    if (safe_name(name) && ends_with(name, ".mp4")) thumbnailer().request(name);
}

void WebStream::feed(const std::shared_ptr<std::vector<uint8_t>>& au, bool key) {
    Impl* d = impl_;
    if (!d) return;
    const uint64_t now = get_time_us();
    {
        std::lock_guard<std::mutex> lk(d->m);
        // The picture rate, measured rather than configured: sample durations
        // have to match the real rate or the player drifts off the live edge.
        if (d->last_us) {
            const double dt = (double)(now - d->last_us);
            if (dt > 2000 && dt < 100000) d->frame_us = 0.95 * d->frame_us + 0.05 * dt;
        }
        d->last_us = now;
        if (d->viewers.load(std::memory_order_relaxed) == 0) return;   // nobody watching
        if (d->q.size() >= kQueueMax) d->q.pop_front();   // the thread is behind
        d->q.push_back({au, key});
    }
    uint64_t one = 1;
    if (write(d->wake_fd, &one, sizeof(one)) < 0) {}
}

// ---- server thread ------------------------------------------------------------

void WebStream::Impl::loop() {
    std::vector<pollfd> pf;
    while (run) {
        // Top up the file bodies being served: a chunk at a time, only once
        // the last one has mostly gone, so a large recording never sits in RAM.
        for (Client* c : clients) {
            if (c->fd < 0 || c->file_fd < 0 || c->pending() >= kFileChunk) continue;
            const size_t want = (size_t)std::min<uint64_t>(kFileChunk, c->file_left);
            const size_t at = c->out.size();
            c->out.resize(at + want);
            const ssize_t n = read(c->file_fd, c->out.data() + at, want);
            if (n <= 0) { c->out.resize(at); close(c->file_fd); c->file_fd = -1; close(c->fd); c->fd = -1; continue; }
            c->out.resize(at + (size_t)n);
            c->file_left -= (uint64_t)n;
            if (!c->file_left) { close(c->file_fd); c->file_fd = -1; }
        }
        pf.clear();
        pf.push_back({listen_fd, POLLIN, 0});
        pf.push_back({wake_fd, POLLIN, 0});
        for (Client* c : clients) {
            short ev = POLLIN;
            if (c->pending()) ev |= POLLOUT;
            pf.push_back({c->fd, ev, 0});
        }
        if (poll(pf.data(), pf.size(), 500) < 0 && errno != EINTR) break;

        if (pf[1].revents & POLLIN) {
            uint64_t n;
            if (read(wake_fd, &n, sizeof(n)) < 0) {}
        }

        // New pictures: mux each one into every streaming viewer's buffer.
        std::deque<Item> items;
        std::vector<std::vector<uint8_t>> ps_now;
        int w_now, h_now, dur_now;
        {
            std::lock_guard<std::mutex> lk(m);
            items.swap(q);
            ps_now = ps;
            w_now = w; h_now = h;
            dur_now = (int)(frame_us * 90000.0 / 1e6 + 0.5);
        }
        for (const Item& it : items)
            for (Client* c : clients)
                if (c->streaming) mux_item(c, it, ps_now, w_now, h_now, dur_now);

        // Socket events. Indices 2.. match `clients` as it was when polled.
        // A client is finished once its fd is closed (-1); every path that
        // gives up on one closes it, and the sweep below removes it once.
        for (size_t i = 0; i < clients.size() && i + 2 < pf.size(); i++) {
            Client* c = clients[i];
            const short re = pf[i + 2].revents;
            if (c->fd < 0) continue;
            if (re & (POLLERR | POLLHUP | POLLNVAL)) { close(c->fd); c->fd = -1; continue; }
            if (re & POLLIN) on_readable(c);
        }
        for (Client* c : clients) {
            if (c->fd < 0) continue;
            if (c->pending()) flush(c);
            if (c->fd < 0) continue;
            if (c->streaming && c->pending() > kDropBytes) {
                printf("webstream: viewer %d too far behind, disconnecting\n", c->fd);
                close(c->fd); c->fd = -1;
            } else if (c->close_after && !c->pending() && c->file_fd < 0) {
                close(c->fd); c->fd = -1;
            }
        }
        for (size_t i = 0; i < clients.size();) {
            if (clients[i]->fd < 0) { drop(clients[i]); clients.erase(clients.begin() + i); }
            else i++;
        }

        if (pf[0].revents & POLLIN) accept_one();
    }
}

void WebStream::Impl::accept_one() {
    int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    if ((int)clients.size() >= kMaxClients) { close(fd); return; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Client* c = new Client();
    c->fd = fd;
    clients.push_back(c);
}

void WebStream::Impl::on_readable(Client* c) {
    char buf[2048];
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { close(c->fd); c->fd = -1; }
        return;
    }
    if (c->streaming || c->close_after) return;   // already answered; ignore
    c->in.append(buf, (size_t)n);
    if (c->in.size() > 8192) { close(c->fd); c->fd = -1; return; }
    const size_t eoh = c->in.find("\r\n\r\n");
    if (eoh == std::string::npos) return;
    // "GET /path HTTP/1.1"
    std::string path = "/";
    const size_t sp1 = c->in.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : c->in.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) path = c->in.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string query;
    const size_t qm = path.find('?');
    if (qm != std::string::npos) { query = path.substr(qm + 1); path.resize(qm); }
    on_request(c, c->in.substr(0, sp1 == std::string::npos ? 0 : sp1), path, query, c->in);
}

void WebStream::Impl::on_request(Client* c, const std::string& method, const std::string& path,
                                 const std::string& query, const std::string& req) {
    auto reply = [c](const char* status, const char* type, const std::string& body) {
        std::string r = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                        "\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
        c->out.insert(c->out.end(), r.begin(), r.end());
        c->close_after = true;
    };

    if (path == "/" || path == "/index.html") {
        // Codec strings MSE accepts for this stream: HEVC Main and H.264 High,
        // at a level high enough for 1080p at 120 fps.
        std::string page = kPage;
        const char* cs = codec == VideoCodec::H264 ? "avc1.640033" : "hvc1.1.6.L153.B0";
        const size_t p = page.find("__CODEC__");
        if (p != std::string::npos) page.replace(p, 9, cs);
        reply("200 OK", "text/html; charset=utf-8", page);
    } else if (path == "/stream.mp4") {
        static const char hdr[] =
            "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n";
        c->out.insert(c->out.end(), hdr, hdr + sizeof(hdr) - 1);
        c->streaming = true;
        c->need_key = true;
        viewers++;
        printf("webstream: viewer %d connected (%d watching)\n", c->fd, viewers.load());
    } else if (path == "/api/theme") {
        // The HUD's current theme, for the page to dress itself in.
        const HudTheme& t = hud_theme_current();
        reply("200 OK", "application/json",
              std::string("{\"name\":\"") + t.name + "\",\"text\":\"" + hex_colour(t.text) +
              "\",\"data\":\"" + hex_colour(t.data) + "\",\"accent\":\"" + hex_colour(t.accent) +
              "\",\"ground\":\"" + hex_colour(t.ground) + "\",\"quiet\":\"" + hex_colour(t.quiet) + "\"}");
    } else if (path == "/api/recordings") {
        reply("200 OK", "application/json", recordings_json());
    } else if (path.compare(0, 5, "/dvr/") == 0) {
        const std::string name = path.substr(5);
        const std::string dir = Settings::getInstance().getString("dvr_dir", "/media/dvr");
        // A recording still being written has no moov yet: not playable, and
        // not something to hand out half-done.
        if (!safe_name(name)) {
            reply("404 Not Found", "text/plain", "not found\n");
            return;
        }
        if (name == DvrRecorder::instance().current_file()) {
            reply("409 Conflict", "text/plain", "still recording\n");
            return;
        }
        if (method == "DELETE") {
            // Recordings only, in the DVR folder - the same names the gallery
            // lists. The one being written was refused just above.
            const std::string path = dir + "/" + name;
            struct stat st;
            if (!(ends_with(name, ".mp4") || ends_with(name, ".h265")) ||
                stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                reply("404 Not Found", "text/plain", "not found\n");
            } else if (unlink(path.c_str()) != 0) {
                reply("500 Internal Server Error", "text/plain", std::string(strerror(errno)) + "\n");
            } else {
                unlink(thumb_path(name).c_str());
                sync();
                printf("webstream: deleted recording %s\n", name.c_str());
                reply("200 OK", "application/json", "{\"deleted\":\"" + name + "\"}");
            }
            return;
        }
        std::string extra;
        if (query.find("dl=1") != std::string::npos)
            extra = "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
        serve_file(c, method, dir + "/" + name,
                   ends_with(name, ".mp4") ? "video/mp4" : "application/octet-stream", req,
                   extra.c_str());
    } else if (path.compare(0, 7, "/thumb/") == 0) {
        const std::string name = path.substr(7);
        struct stat rs, ts;
        if (!safe_name(name) || !ends_with(name, ".mp4") ||
            name == DvrRecorder::instance().current_file() ||
            stat((dvr_dir() + "/" + name).c_str(), &rs) != 0) {
            reply("404 Not Found", "text/plain", "no thumbnail\n");
        } else if (stat(thumb_path(name).c_str(), &ts) == 0 && ts.st_mtime >= rs.st_mtime) {
            serve_file(c, method, thumb_path(name), "image/jpeg", req, "Cache-Control: max-age=3600\r\n");
        } else {
            // Not made yet: ask for it, and tell the page to come back.
            thumbnailer().request(name);
            std::string r = "HTTP/1.1 202 Accepted\r\nRetry-After: 1\r\nContent-Length: 0\r\n"
                            "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
            c->out.insert(c->out.end(), r.begin(), r.end());
            c->close_after = true;
        }
    } else if (path == "/font/ChakraPetch-SemiBold.ttf" || path == "/font/ChakraPetch-Medium.ttf") {
        // The app's own face (SIL OFL), shipped in the image.
        serve_file(c, method, "/usr/share/fonts/truetype/chakra-petch/" + path.substr(6), "font/ttf",
                   req, "Cache-Control: max-age=86400\r\n");
    } else {
        reply("404 Not Found", "text/plain", "not found\n");
    }
}

// A file, with byte ranges: Safari will not play a video without them. The
// body is read in chunks as the socket drains (see loop()), never whole.
void WebStream::Impl::serve_file(Client* c, const std::string& method, const std::string& path,
                                 const char* type, const std::string& req, const char* extra) {
    auto fail = [c](const char* status) {
        std::string r = std::string("HTTP/1.1 ") + status +
                        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        c->out.insert(c->out.end(), r.begin(), r.end());
        c->close_after = true;
    };
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        fail("404 Not Found");
        return;
    }
    const uint64_t size = (uint64_t)st.st_size;
    uint64_t first = 0, last = size ? size - 1 : 0;
    bool partial = false;
    // Header names are case-insensitive; find "range: bytes=" on a lowered copy.
    static const char kRange[] = "\r\nrange: bytes=";
    std::string low = req;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char)tolower(ch); });
    const size_t rh = low.find(kRange);
    if (rh != std::string::npos && size) {
        const char* p = req.c_str() + rh + sizeof(kRange) - 1;
        char* e;
        if (*p == '-') {                                  // bytes=-N: the last N
            const uint64_t n = strtoull(p + 1, &e, 10);
            first = n >= size ? 0 : size - n;
        } else {
            first = strtoull(p, &e, 10);
            if (*e == '-' && isdigit((unsigned char)e[1])) last = std::min<uint64_t>(strtoull(e + 1, nullptr, 10), size - 1);
        }
        if (first > last || first >= size) {
            close(fd);
            std::string r = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                            std::to_string(size) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            c->out.insert(c->out.end(), r.begin(), r.end());
            c->close_after = true;
            return;
        }
        partial = true;
    }
    const uint64_t len = size ? last - first + 1 : 0;
    std::string h = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    h += std::string("Content-Type: ") + type + "\r\nContent-Length: " + std::to_string(len) +
         "\r\nAccept-Ranges: bytes\r\n";
    if (partial)
        h += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
             std::to_string(size) + "\r\n";
    h += std::string(extra) + "Connection: close\r\n\r\n";
    c->out.insert(c->out.end(), h.begin(), h.end());
    c->close_after = true;
    if (method == "HEAD" || len == 0) { close(fd); return; }
    lseek(fd, (off_t)first, SEEK_SET);
    c->file_fd = fd;
    c->file_left = len;
}

// The DVR folder, newest first. Only names the server would also serve.
std::string WebStream::Impl::recordings_json() {
    struct Rec { std::string name; uint64_t size; int64_t mtime; };
    std::vector<Rec> recs;
    const std::string dir = Settings::getInstance().getString("dvr_dir", "/media/dvr");
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (!safe_name(n) || !(ends_with(n, ".mp4") || ends_with(n, ".h265"))) continue;
            struct stat st;
            if (stat((dir + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            recs.push_back({n, (uint64_t)st.st_size, (int64_t)st.st_mtime});
        }
        closedir(d);
    }
    // Newest first, by the time in the name (YYYYMMDD_HHMMSS): the prefix
    // changed from kestrel_ to fpvOS_, and sorting on the whole name put every
    // new recording below the old ones. A name without one sorts by its mtime.
    auto stamp = [](const Rec& r) {
        for (size_t i = 0; i + 15 <= r.name.size(); i++) {
            bool ok = r.name[i + 8] == '_';
            for (size_t k = 0; ok && k < 15; k++)
                if (k != 8 && !isdigit((unsigned char)r.name[i + k])) ok = false;
            if (ok) return r.name.substr(i, 15);
        }
        char t[32];
        struct tm tmv;
        time_t mt = (time_t)r.mtime;
        gmtime_r(&mt, &tmv);
        strftime(t, sizeof(t), "%Y%m%d_%H%M%S", &tmv);
        return std::string(t);
    };
    std::sort(recs.begin(), recs.end(), [&](const Rec& a, const Rec& b) {
        const std::string sa = stamp(a), sb = stamp(b);
        return sa != sb ? sa > sb : a.name > b.name;
    });
    // Thumbnails of recordings that are gone (deleted on the goggle, or by the
    // DVR's own low-space purge) go with them.
    if (DIR* d = opendir(thumb_dir().c_str())) {
        while (dirent* e = readdir(d)) {
            std::string t = e->d_name;
            if (!ends_with(t, ".jpg")) continue;
            const std::string rec = t.substr(0, t.size() - 4);
            if (!std::any_of(recs.begin(), recs.end(), [&](const Rec& r) { return r.name == rec; }))
                unlink((thumb_dir() + "/" + t).c_str());
        }
        closedir(d);
    }
    const std::string active = DvrRecorder::instance().current_file();
    std::string j = "{\"recordings\":[";
    for (size_t i = 0; i < recs.size(); i++) {
        const Rec& r = recs[i];
        const bool rec = (r.name == active);
        const bool mp4 = ends_with(r.name, ".mp4");
        const double dur = (mp4 && !rec) ? mp4_duration(dir + "/" + r.name) : -1;
        // Missing thumbnails are queued now, while the page is still laying
        // out, not one by one as each image asks.
        struct stat ts;
        if (mp4 && !rec && dur > 0 &&
            (stat(thumb_path(r.name).c_str(), &ts) != 0 || ts.st_mtime < r.mtime))
            thumbnailer().request(r.name);
        char num[96];
        snprintf(num, sizeof(num), ",\"size\":%llu,\"mtime\":%lld,\"duration\":%.1f",
                 (unsigned long long)r.size, (long long)r.mtime, dur);
        j += std::string(i ? "," : "") + "{\"name\":\"" + r.name + "\"" + num +
             ",\"recording\":" + (rec ? "true" : "false") +
             ",\"playable\":" + ((mp4 && !rec && dur > 0) ? "true" : "false") + "}";
    }
    return j + "]}";
}

void WebStream::Impl::mux_item(Client* c, const Item& it,
                               const std::vector<std::vector<uint8_t>>& ps_now,
                               int w_now, int h_now, int dur_now) {
    if (c->fd < 0) return;
    // Behind: stop adding until the backlog drains, then pick up at a keyframe
    // so the phone never decodes a picture whose references it skipped.
    if (c->pending() > kSkipBytes) {
        if (!c->need_key)
            printf("webstream: viewer %d behind (%zu KB unsent), skipping to the next keyframe\n",
                   c->fd, c->pending() / 1024);
        c->need_key = true;
        return;
    }
    if (c->need_key) {
        if (!it.key) return;
        if (!c->mux) {
            if (ps_now.empty()) return;          // nothing to build hvcC/avcC from yet
            c->dur = dur_now > 0 ? dur_now : 750;
            c->mux = MP4E_open(0, 1 /*fragmented*/, c, mux_write);
            if (!c->mux || MP4E_STATUS_OK != mp4_h26x_write_init(&c->wr, c->mux, w_now, h_now,
                                                                 codec == VideoCodec::H265)) {
                printf("webstream: muxer init failed\n");
                close(c->fd); c->fd = -1;
                return;
            }
            // Each parameter set exactly once: minimp4 appends a repeated one
            // to hvcC as a second, malformed entry (see Ar8030Source::emit_nal).
            for (const auto& p : ps_now)
                mp4_h26x_write_nal(&c->wr, p.data(), (int)p.size(), c->dur);
        }
        c->need_key = false;
    }
    // The picture as ONE sample, built here rather than by
    // mp4_h26x_write_nal(): that hands every slice after the first to the
    // muxer as a "continuation", which minimp4 honours in a plain MP4 but not
    // in fragmented mode, where each slice became a fragment of its own. This
    // stream sends two slices per picture, so every other sample started
    // mid-picture - FFmpeg shrugged ("First slice in a frame missing"), the
    // iPhone's hardware decoder refused the lot ("media failed to decode").
    static thread_local std::vector<uint8_t> smp;
    annexb_to_sample(*it.au, smp);
    if (!smp.empty())
        MP4E_put_sample(c->mux, c->wr.mux_track_id, smp.data(), (int)smp.size(), c->dur,
                        it.key ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT);
}

void WebStream::Impl::flush(Client* c) {
    while (c->pending()) {
        ssize_t n = send(c->fd, c->out.data() + c->out_off, c->pending(), MSG_NOSIGNAL);
        if (n > 0) { c->out_off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close(c->fd); c->fd = -1;
        return;
    }
    // Reclaim the sent prefix now and then instead of on every send.
    if (c->out_off && (c->out_off == c->out.size() || c->out_off > (1u << 20))) {
        c->out.erase(c->out.begin(), c->out.begin() + (long)c->out_off);
        c->out_off = 0;
    }
}

void WebStream::Impl::drop(Client* c) {
    if (c->streaming) {
        viewers--;
        printf("webstream: viewer disconnected (%d watching)\n", viewers.load());
    }
    if (c->mux) {                   // same order as the DVR
        MP4E_close(c->mux);
        mp4_h26x_write_close(&c->wr);
    }
    if (c->file_fd >= 0) close(c->file_fd);
    if (c->fd >= 0) close(c->fd);
    delete c;
}
