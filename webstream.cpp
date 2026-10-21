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
#include "dvr_library.hpp"
#include "zones.hpp"
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
//            Over the picture: a LIVE badge (amber catching up, grey
//            reconnecting) whose tap shows the numbers. A phone turned
//            sideways gets the picture alone.
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
#fs{position:absolute;right:10px;bottom:10px;width:40px;height:40px;padding:0;border:0;border-radius:8px;
  background:rgba(0,0,0,.55);color:#fff;cursor:pointer;display:flex;align-items:center;justify-content:center}
#fs svg{width:22px;height:22px;fill:none;stroke:currentColor;stroke-width:2.2;stroke-linecap:round}
.frame:fullscreen{border-radius:0;aspect-ratio:auto;width:100vw;height:100vh}
.frame:-webkit-full-screen{border-radius:0;aspect-ratio:auto;width:100vw;height:100vh}
[hidden]{display:none!important}
.ov{position:absolute;top:10px;left:max(10px,env(safe-area-inset-left));display:flex;gap:8px;align-items:center}
.pill{display:inline-flex;align-items:center;gap:7px;padding:6px 11px;border:0;border-radius:999px;
  background:rgba(0,0,0,.55);color:#fff;font:600 12px Chakra,system-ui;letter-spacing:.14em;cursor:pointer}
.pill i{width:8px;height:8px;border-radius:50%;background:var(--quiet)}
.pill.live i{background:var(--accent)}
.pill.behind i{background:#f2c94c}
.pill.wait i{animation:blink 1s infinite}
.pill.off i{background:#ff4d4d}
@keyframes blink{50%{opacity:.2}}
.det{position:absolute;bottom:10px;left:max(10px,env(safe-area-inset-left));right:62px;padding:6px 10px;
  border-radius:8px;background:rgba(0,0,0,.55);color:#fff;font-size:12px;line-height:1.4}
.msg{position:absolute;left:16px;right:16px;top:50%;transform:translateY(-50%);text-align:center;
  color:#fff;font-size:14px;line-height:1.4}
/* A phone on its side: the picture, edge to edge, nothing else. */
@media (orientation:landscape) and (max-height:540px){
  body.watching header,body.watching .tz{display:none}
  body.watching main{padding:0}
  body.watching .frame{border-radius:0;aspect-ratio:auto;width:100vw;height:100vh;height:100dvh}
}
.tz{margin-top:14px;display:flex;flex-wrap:wrap;align-items:center;gap:10px;color:var(--quiet);font-size:13px}
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
  <div class="msg" id="m"></div>
  <div class="ov"><button class="pill wait" id="lb" title="Show the numbers"><i></i><span id="lt">CONNECTING</span></button></div>
  <div class="det" id="s" hidden></div>
  <button id="b">TAP TO PLAY</button>
  <button id="fs" aria-label="Full screen" title="Full screen"><svg viewBox="0 0 24 24"><path id="fsi" d="M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5"/></svg></button></div>
  <div class="tz" id="tz"></div>
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
  const RECOVER = params.get('recover') !== '0';
  const TARGET = Math.min(2000, Math.max(40, parseInt(params.get('buffer') || '250', 10) || 250)) / 1000;
  const say = t => { s.textContent = t; };          // the numbers, under a tap on the badge
  const msg = t => { $('m').textContent = t; };      // something to read over the picture
  const lb = $('lb'), lt = $('lt');
  const badge = (kind, text) => { lb.className = 'pill ' + kind; lt.textContent = text; };
  const store = (k, v) => { try { v === undefined ? localStorage.removeItem(k) : localStorage.setItem(k, v); } catch (e) {} };
  try { s.hidden = localStorage.getItem('fpvos-numbers') !== '1'; } catch (e) {}
  lb.onclick = () => { s.hidden = !s.hidden; store('fpvos-numbers', s.hidden ? undefined : '1'); };
  // ?debug=1: the player's state and events, into the goggle's log.
  const DEBUG = params.get('debug') === '1', T0 = performance.now();
  const plog = m => { if (DEBUG) fetch('/api/plog?m=' + encodeURIComponent(((performance.now() - T0) / 1000).toFixed(2) + ' ' + m), { cache: 'no-store' }).catch(() => {}); };
  if (DEBUG) for (const e of ['waiting', 'stalled', 'seeking', 'seeked', 'playing', 'pause', 'error', 'emptied', 'suspend'])
    v.addEventListener(e, () => plog('ev ' + e + ' ct ' + v.currentTime.toFixed(2) + ' rs ' + v.readyState));
  let session = 0, running = false, recoveries = 0, lastErr = '', teardown = () => {};
  b.onclick = () => { b.style.display = 'none'; v.play(); };
  // Full screen: the picture's frame (button included) where the browser can;
  // iPhone Safari only lets a video go full screen, through its own player.
  const frame = v.parentElement, fsi = $('fsi');
  const fsEl = () => document.fullscreenElement || document.webkitFullscreenElement;
  $('fs').onclick = () => {
    if (fsEl()) { (document.exitFullscreen || document.webkitExitFullscreen).call(document); return; }
    const go = frame.requestFullscreen || frame.webkitRequestFullscreen;
    if (go) {
      const r = go.call(frame);
      if (r && r.catch) r.catch(() => { if (v.webkitEnterFullscreen) v.webkitEnterFullscreen(); });
      try { screen.orientation.lock('landscape').catch(() => {}); } catch (e) {}
    } else if (v.webkitEnterFullscreen) {
      v.webkitEnterFullscreen();
    }
  };
  const fsIcon = () => fsi.setAttribute('d', fsEl() ? 'M9 4v5H4M20 9h-5V4M15 20v-5h5M4 15h5v5'
                                                    : 'M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5');
  document.addEventListener('fullscreenchange', fsIcon);
  document.addEventListener('webkitfullscreenchange', fsIcon);

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
      // Reconnect, which starts again at a keyframe: after a dropped connection
      // (WiFi) and after a decode error (a picture that never arrived) alike.
      // ?recover=0 stops at a decode error instead, to see it.
      if (!RECOVER && why !== 'stream ended') {
        badge('off', 'STOPPED');
        msg('Stopped: ' + why + (quality() ? ' (' + quality() + ')' : ''));
        return;
      }
      badge('wait', 'RECONNECTING');
      plog('reconnect after ' + why);
      recoveries++; lastErr = why;
      setTimeout(() => { if (running) open(); }, 300);
    };
    v.onerror = () => { const e = v.error; fail(e ? 'error ' + e.code + (e.message ? ': ' + e.message : '') : 'error'); };
    v.disableRemotePlayback = true;
    v.src = URL.createObjectURL(ms);
    if (DEBUG) for (const e of ['startstreaming', 'endstreaming', 'sourceclose', 'sourceended'])
      ms.addEventListener(e, () => plog('ms ' + e));
    ms.addEventListener('sourceopen', async () => {
      if (!alive()) return;
      const sb = ms.addSourceBuffer(TYPE);
      try { sb.mode = 'sequence'; } catch (e) {}
      const q = [];
      const pump = () => {
        if (!alive() || sb.updating || !q.length || ms.readyState !== 'open') return;
        try {
          // Played video is trimmed, but never the last 15 s: removing a
          // keyframe drops every picture up to the next one, and the goggle
          // sends one every 10 s (intra refresh has none of its own).
          if (v.buffered.length && v.currentTime - v.buffered.start(0) > 20) { sb.remove(0, v.currentTime - 15); return; }
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
        if (!v.buffered.length) {
          if (!recoveries) badge('wait', 'CONNECTING');
          say('Waiting for a keyframe... ' + mbps + ' Mbps' + rec);
          return;
        }
        const end = v.buffered.end(v.buffered.length - 1), lag = end - v.currentTime;
        if (DEBUG) {
          let rg = ''; for (let i = 0; i < v.buffered.length; i++) rg += ' [' + v.buffered.start(i).toFixed(1) + '-' + v.buffered.end(i).toFixed(1) + ']';
          plog('st ct ' + v.currentTime.toFixed(2) + ' lag ' + lag.toFixed(2) + ' rate ' + v.playbackRate + ' rs ' + v.readyState +
               ' paused ' + v.paused + ' ' + mbps + 'Mbps ' + quality() + ' buf' + rg + (ms.streaming === undefined ? '' : ' streaming ' + ms.streaming));
        }
        // Far behind: jump (it lands between keyframes, which the browser
        // has to decode its way to - a visible stall); behind: catch up by
        // playing faster, which nobody notices for a moment.
        if (lag > TARGET + 4.0) { plog('jump to ' + (end - TARGET).toFixed(2)); v.currentTime = end - TARGET; v.playbackRate = 1; }
        else if (lag > TARGET + 0.6) v.playbackRate = 1.25;
        else if (lag > TARGET * 1.5) v.playbackRate = 1.05;
        else if (lag > TARGET * 1.15) v.playbackRate = 1.02;
        else if (lag < TARGET * 0.5) v.playbackRate = 0.95;
        else if (lag < TARGET * 0.85) v.playbackRate = 0.98;
        else v.playbackRate = 1;
        if (v.paused) { v.play().catch(() => { b.style.display = 'block'; }); badge('wait', 'PAUSED'); }
        // Playing within the catch-up band: live. Beyond it, playing faster to get back.
        else if (lag > TARGET + 0.6) badge('behind', 'CATCHING UP'); else badge('live', 'LIVE');
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
      if (!MS) { badge('off', 'NO VIDEO'); msg('This browser has no Media Source support (iPhone needs iOS 17.1 or later).'); return; }
      if (!MS.isTypeSupported(TYPE)) { badge('off', 'NO VIDEO'); msg('This browser cannot play ' + TYPE); return; }
      running = true; msg(''); badge('wait', 'CONNECTING'); say('Connecting...'); open();
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

// ---- time zone ----------------------------------------------------------------
// SYSTEM > Time Zone sets the goggle's clock and the country whose channel rules
// its channel page shows. A phone knows its own zone, so one tap copies it over.
async function tz() {
  const el = $('tz');
  let mine = '';
  try { mine = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch (e) {}
  let cur;
  try {
    cur = await (await fetch('/api/timezone?phone=' + encodeURIComponent(mine), { cache: 'no-store' })).json();
  } catch (e) { return; }
  el.textContent = '';
  const t = document.createElement('span');
  t.textContent = 'Goggle time zone: ' + (cur.zone ? cur.zone + ' (' + cur.country_name + ')' : 'not set');
  el.appendChild(t);
  if (!mine) return;
  if (!cur.phone) {
    const n = document.createElement('span');
    n.textContent = '(this phone\'s, ' + mine + ', is not in the goggle\'s list)';
    el.appendChild(n);
  } else if (cur.phone !== cur.zone) {
    const btn = document.createElement('button');
    btn.className = 'btn';
    btn.textContent = 'USE THIS PHONE\'S: ' + cur.phone;
    btn.onclick = async () => {
      btn.disabled = true;
      try { await fetch('/api/timezone?zone=' + encodeURIComponent(cur.phone), { method: 'POST' }); } catch (e) {}
      tz();
    };
    el.appendChild(btn);
  }
}

// ---- tabs ---------------------------------------------------------------------
function show(tab) {
  for (const [btn, sec] of [['tl', 'live'], ['tg', 'gallery']]) {
    $(btn).classList.toggle('on', sec === tab);
    $(sec).classList.toggle('on', sec === tab);
  }
  theme();
  document.body.classList.toggle('watching', tab === 'live');
  if (tab === 'live') { live.start(); tz(); } else { live.stop(); gallery(); }
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
constexpr int    kSndBuf      = 256 << 10;         // the viewer socket's kernel buffer
// A viewer whose backlog has stayed over skip_bytes this long is not catching
// up: given up on. A size limit cannot say it - mux_item stops queuing at
// skip_bytes, so the backlog never grows much past it.
constexpr uint64_t kBehindUs  = 10000000;
constexpr int    kMaxClients  = 16;               // Safari opens several per video
constexpr size_t kFileChunk   = 256u << 10;        // file bytes read ahead per client

struct Item {
    std::shared_ptr<std::vector<uint8_t>> au;
    bool key;
};

// After the last viewer leaves, the screen encoder keeps running this long: a
// page reconnecting (WiFi, a decode error) is back within a second.
constexpr uint64_t kScreenIdleUs = 10000000;

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
    unsigned gen = 0;               // the screen source its muxer was made for
    int dur = 750;                  // sample duration, 90 kHz ticks
    int64_t written = 0;            // bytes the muxer has emitted
    int file_fd = -1;               // a recording being served, read in chunks
    uint64_t file_left = 0;         // bytes of it still to read
    uint64_t behind_since_us = 0;   // backlog over skip_bytes since then, 0 not behind

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

// The DVR folder and what is in it (list, lengths, thumbnails) is dvr_library's,
// shared with the goggle's own gallery.
using dvr_lib::safe_name;
using dvr_lib::ends_with;
using dvr_lib::mp4_duration;
using dvr_lib::thumb_dir;
using dvr_lib::thumb_path;
std::string dvr_dir() { return dvr_lib::dir(); }


std::string hex_colour(const float c[3]) {
    char b[8];
    snprintf(b, sizeof(b), "#%02x%02x%02x", (int)lroundf(c[0] * 255), (int)lroundf(c[1] * 255),
             (int)lroundf(c[2] * 255));
    return b;
}

// Annex-B (start codes) to an MP4 sample (4-byte big-endian length per NAL).
// strip_ps: an H.264 stream's SPS/PPS/AUD are left out - they are in the
// avcC already, and Safari refuses a sample that repeats them.
void annexb_to_sample(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool strip_ps = false) {
    out.clear();
    const size_t n = in.size();
    size_t i = 0, start = SIZE_MAX;
    auto emit = [&](size_t a, size_t b) {
        while (b > a && in[b - 1] == 0) b--;           // trailing zero bytes of the next start code
        if (b <= a) return;
        if (strip_ps) {
            const int t = in[a] & 0x1F;
            if (t == 7 || t == 8 || t == 9) return;
        }
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

// Each NAL of an Annex-B buffer with a 4-byte start code of its own.
std::vector<std::vector<uint8_t>> split_nals(const uint8_t* p, size_t n) {
    std::vector<std::vector<uint8_t>> v;
    size_t i = 0, start = SIZE_MAX;
    auto emit = [&](size_t a, size_t b) {
        while (b > a && p[b - 1] == 0) b--;
        if (b <= a) return;
        std::vector<uint8_t> nal = {0, 0, 0, 1};
        nal.insert(nal.end(), p + a, p + b);
        v.push_back(std::move(nal));
    };
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) emit(start, i);
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX) emit(start, n);
    return v;
}

} // namespace

struct WebStream::Impl {
    int listen_fd = -1;
    int wake_fd = -1;
    pthread_t tid = 0;
    std::atomic<bool> run{false};
    std::atomic<int> viewers{0};   // streaming clients, read lock-free by feed_screen()
    // Unsent bytes past which a viewer skips to the next keyframe: about a
    // second at the web view's cap.
    size_t skip_bytes = 0;
    std::atomic<bool> want_screen_key{false};
    bool screen_on = false;        // server thread: the encoder asked for
    uint64_t idle_since = 0;       // server thread: no viewer since then, us
    uint64_t screen_since = 0;     // server thread: asked for since then, us
    uint64_t retry_us = 0;         // server thread: asked again then, us
    std::atomic<uint64_t> last_screen_us{0};   // the encoder's last picture, us
    unsigned cur_gen = 0;          // server thread: the source being muxed

    std::mutex m;                  // guards everything below
    std::deque<Item> q;
    // The screen source: SPS/PPS, size, picture duration, and a generation
    // that changes with any of them.
    std::vector<std::vector<uint8_t>> screen_ps;
    int sw = 0, sh = 0, sdur = 0;
    unsigned screen_gen = 0;
    unsigned q_dropped = 0;        // pictures dropped off q (the thread behind)

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

bool WebStream::start(int port) {
    if (impl_) return true;
    Impl* d = new Impl();
    d->skip_bytes = (size_t)DvrRecorder::web_cap_kbps() * 1000 / 8;

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

void WebStream::recording_finished(const std::string& name) {
    if (safe_name(name) && ends_with(name, ".mp4")) dvr_lib::request_thumbnail(name);
}

void WebStream::set_screen_source(const std::vector<uint8_t>& header, int w, int h, int dur90k) {
    Impl* d = impl_;
    if (!d) return;
    auto ps = split_nals(header.data(), header.size());
    {
        std::lock_guard<std::mutex> lk(d->m);
        // A viewer's muxer holds the SPS/PPS and size: only a change in them
        // restarts it. Its picture duration is its own (fixed per viewer), so
        // another encoder's period a tick off (the recording's slots x 2 vs
        // every 4th refresh) is no reason to reconnect.
        d->sdur = dur90k > 0 ? dur90k : 3000;
        if (ps == d->screen_ps && w == d->sw && h == d->sh) return;
        d->screen_ps = std::move(ps);
        d->sw = w; d->sh = h;
        d->screen_gen++;
        d->q.clear();
    }
    printf("webstream: screen source %dx%d, %.2f fps\n", w, h, 90000.0 / (dur90k > 0 ? dur90k : 3000));
    uint64_t one = 1;
    if (write(d->wake_fd, &one, sizeof(one)) < 0) {}
}

bool WebStream::feed_screen(const uint8_t* p, size_t n, int dur) {
    Impl* d = impl_;
    if (!d || !n) return false;
    d->last_screen_us.store(get_time_us(), std::memory_order_relaxed);
    if (d->viewers.load(std::memory_order_relaxed) == 0) return false;
    bool key = false;   // an IDR slice in it
    for (size_t i = 0; i + 3 < n && !key; i++)
        key = p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 5;
    auto au = std::make_shared<std::vector<uint8_t>>(p, p + n);
    (void)dur;   // every picture is the source's duration (constant rate, see DVR)
    {
        std::lock_guard<std::mutex> lk(d->m);
        if (d->q.size() >= kQueueMax) { d->q.pop_front(); d->q_dropped++; }
        d->q.push_back({au, key});
    }
    uint64_t one = 1;
    if (write(d->wake_fd, &one, sizeof(one)) < 0) {}
    return d->want_screen_key.load(std::memory_order_relaxed);
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
        unsigned dropped = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            items.swap(q);
            dropped = q_dropped; q_dropped = 0;
            ps_now = screen_ps;
            w_now = sw; h_now = sh;
            dur_now = sdur;
            cur_gen = screen_gen;
        }
        // A new screen source (another encoder, another size): a viewer's
        // muxer was made for the old one, so the page connects again.
        for (Client* c : clients)
            if (c->streaming && c->mux && c->gen != cur_gen && c->fd >= 0) {
                printf("webstream: viewer %d - the screen source changed, reconnecting it\n", c->fd);
                close(c->fd); c->fd = -1;
            }
        if (dropped) {
            printf("webstream: %u pictures dropped behind - viewers restart at the next keyframe\n", dropped);
            for (Client* c : clients)
                if (c->streaming) c->need_key = true;
        }
        for (const Item& it : items)
            for (Client* c : clients)
                if (c->streaming && c->fd >= 0) mux_item(c, it, ps_now, w_now, h_now, dur_now);

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
            const bool behind = c->streaming && c->pending() > skip_bytes;
            if (!behind) {
                c->behind_since_us = 0;
            } else if (!c->behind_since_us) {
                c->behind_since_us = get_time_us();
            }
            if (behind && get_time_us() - c->behind_since_us >= kBehindUs) {
                printf("webstream: viewer %d too far behind for %llu s, disconnecting\n",
                       c->fd, (unsigned long long)(kBehindUs / 1000000));
                close(c->fd); c->fd = -1;
            } else if (c->close_after && !c->pending() && c->file_fd < 0) {
                close(c->fd); c->fd = -1;
            }
        }
        for (size_t i = 0; i < clients.size();) {
            if (clients[i]->fd < 0) { drop(clients[i]); clients.erase(clients.begin() + i); }
            else i++;
        }
        bool waiting = false;
        // Only once it can take one: a keyframe that comes while it is still
        // behind is skipped like the rest, and costs the radio link for nothing.
        for (Client* c : clients) waiting |= c->streaming && c->need_key && c->pending() <= skip_bytes;
        {
            // The screen encoder makes a keyframe every 2 s by itself; one is
            // forced only for a viewer waiting to start. It runs while anyone
            // watches, and a little longer (kScreenIdleUs).
            want_screen_key.store(waiting, std::memory_order_relaxed);
            const uint64_t now = get_time_us();
            if (viewers.load() > 0) {
                idle_since = 0;
                if (!screen_on) {
                    screen_on = true;
                    screen_since = now;
                    DvrRecorder::instance().set_web_watching(true);
                } else {
                    // Nothing from the encoder for 3 s: it could not start (the
                    // screen not known yet, no encoder) - ask again, every 3 s.
                    const uint64_t last = std::max(last_screen_us.load(std::memory_order_relaxed), screen_since);
                    if (now - last > 3000000 && now - retry_us > 3000000) {
                        retry_us = now;
                        DvrRecorder::instance().web_retry();
                    }
                }
            } else if (screen_on) {
                if (!idle_since) idle_since = now;
                else if (now - idle_since > kScreenIdleUs) {
                    screen_on = false;
                    DvrRecorder::instance().set_web_watching(false);
                }
            }
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
        // The codec string MSE is given: H.264 Main as the VEPU encodes it,
        // level 4.2 (1080p60), covering both the web view's encoder and a
        // screen recording's.
        std::string page = kPage;
        const char* cs = "avc1.4d402a";
        const size_t p = page.find("__CODEC__");
        if (p != std::string::npos) page.replace(p, 9, cs);
        reply("200 OK", "text/html; charset=utf-8", page);
    } else if (path == "/stream.mp4") {
        static const char hdr[] =
            "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n";
        c->out.insert(c->out.end(), hdr, hdr + sizeof(hdr) - 1);
        // Small, so a backlog shows up here within a fraction of a second.
        // Left to autotune it grew to 8 MB - five seconds of a 13 Mbit/s
        // stream queued unseen while the page froze, waiting for it to drain
        // over a WiFi that had dipped below the video's rate.
        setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &kSndBuf, sizeof(kSndBuf));
        // Sent at the web view's cap, never faster - a keyframe, or what
        // piled up while the WiFi stalled, goes out paced instead of as a
        // burst the goggle's WiFi (and a dozing iPhone) chokes on.
        {
            const uint32_t rate = (uint32_t)DvrRecorder::web_cap_kbps() * 1000 / 8;   // bytes/s
            setsockopt(c->fd, SOL_SOCKET, SO_MAX_PACING_RATE, &rate, sizeof(rate));
        }
        // A phone that walks out of range without closing: its socket is given
        // up once nothing it was sent has been acknowledged for this long,
        // instead of the kernel's 15-30 minutes of retransmission - the viewer
        // count, and with it the screen encoder, follow.
        const unsigned user_timeout_ms = 20000;
        setsockopt(c->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms));
        c->streaming = true;
        c->need_key = true;
        viewers++;
        printf("webstream: viewer %d connected (%d watching)\n", c->fd, viewers.load());
    } else if (path == "/api/plog") {
        // ?debug=1 on the page: its player's state, a line at a time, into
        // this log (what a phone's browser does is otherwise out of sight).
        std::string m;
        for (size_t i = query.compare(0, 2, "m=") == 0 ? 2 : query.size(); i < query.size() && m.size() < 400; i++) {
            char ch = query[i];
            if (ch == '%' && i + 2 < query.size()) { ch = (char)strtol(query.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
            else if (ch == '+') ch = ' ';
            m += (ch >= 32 && ch < 127) ? ch : '?';
        }
        printf("webpage %d: %s\n", c->fd, m.c_str());
        reply("204 No Content", "text/plain", "");
    } else if (path == "/api/theme") {
        // The HUD's current theme, for the page to dress itself in.
        const HudTheme& t = hud_theme_current();
        reply("200 OK", "application/json",
              std::string("{\"name\":\"") + t.name + "\",\"text\":\"" + hex_colour(t.text) +
              "\",\"data\":\"" + hex_colour(t.data) + "\",\"accent\":\"" + hex_colour(t.accent) +
              "\",\"ground\":\"" + hex_colour(t.ground) + "\",\"quiet\":\"" + hex_colour(t.quiet) + "\"}");
    } else if (path == "/api/timezone") {
        // SYSTEM > Time Zone. GET: the zone in force, and with ?phone= the zone a
        // phone reports as the goggle names it ("" when it has no such zone).
        // POST ?zone=: pick it (old names like Asia/Calcutta are found too).
        auto arg = [&](const char* key) {
            const std::string k = std::string(key) + "=";
            size_t p = 0;
            while (p < query.size()) {
                size_t e = query.find('&', p);
                if (e == std::string::npos) e = query.size();
                if (query.compare(p, k.size(), k) == 0) {
                    std::string v;
                    for (size_t i = p + k.size(); i < e; i++) {
                        if (query[i] == '%' && i + 2 < e) {
                            v += (char)strtol(query.substr(i + 1, 2).c_str(), nullptr, 16);
                            i += 2;
                        } else v += query[i] == '+' ? ' ' : query[i];
                    }
                    return v;
                }
                p = e + 1;
            }
            return std::string();
        };
        auto json_str = [](const std::string& t) {
            std::string o;
            for (char ch : t) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
            return o;
        };
        if (method == "POST") {
            const int z = zones::find(arg("zone"));
            if (z < 0) {
                reply("404 Not Found", "application/json", "{\"error\":\"no such time zone\"}");
                return;
            }
            zones::select(z);
            printf("webstream: time zone set to %s from the web page\n", zones::zone(z).name);
        }
        const int z = zones::selected();
        const int pz = zones::find(arg("phone"));
        std::string j = "{\"zone\":\"";
        if (z >= 0) {
            const zones::Country& c = zones::country(zones::zone(z).country);
            j += json_str(zones::zone(z).name) + "\",\"country\":\"" + c.code +
                 "\",\"country_name\":\"" + json_str(c.name);
        } else {
            j += "\",\"country\":\"\",\"country_name\":\"";
        }
        j += "\",\"phone\":\"" + std::string(pz >= 0 ? zones::zone(pz).name : "") + "\"}";
        reply("200 OK", "application/json", j);
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
            dvr_lib::request_thumbnail(name);
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
    using Rec = dvr_lib::Recording;
    std::vector<Rec> recs = dvr_lib::list_recordings();
    const std::string dir = dvr_dir();
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
            dvr_lib::request_thumbnail(r.name);
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
    if (c->pending() > skip_bytes) {
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
            c->gen = cur_gen;
            c->mux = MP4E_open(0, 1 /*fragmented*/, c, mux_write);
            if (!c->mux || MP4E_STATUS_OK != mp4_h26x_write_init(&c->wr, c->mux, w_now, h_now, 0 /* H.264 */)) {
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
    annexb_to_sample(*it.au, smp, true);
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
