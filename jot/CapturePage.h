#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

//////////////////////////////////////////////////////////////////////////////////////////////////
// CapturePage - a phone-sized page for writing jots, served by jot at GET /jot/m?k=TOKEN.
//
// WRITES JOTS, NEVER READS THEM. It posts to /jot. The "Sent" list is this browser's own record of
// what it sent, kept in localStorage. The one thing it reads from the server is the reminder
// notifier's list of upcoming todos (GET /jot/upcoming: summaries and due times, never a jot's
// text) - see notify/jotnotify.py.
//
// THE TOKEN RIDES IN THE URL because a browser cannot attach an Authorization header to a page
// navigation. Without a valid k the page is the same bare 404 as every other rejection, so it does
// not mark /jot as special. The cost is that the token lands in the proxy's access log and in the
// phone's history. The page loads nothing from elsewhere and sends no Referer, so the URL goes
// nowhere else.
//
// OFFLINE QUEUE. A jot that fails to send stays queued in localStorage and is retried when the page
// comes back online or is reopened. A 404 means the token is wrong, so auto-retry stops then:
// retrying a bad token would trip jot's rate limiter and block this address.
//
// Add to Home Screen keeps the ?k= in the saved URL, so the icon opens straight into the page.
//
// TODO, PRIORITY AND DUE TRAVEL AS [bracket] TAGS at the front of the entry - "[todo]
// [priority:high] [due:2026-10-20t12:00] call the bank". That is the convention persist/Watcher.cpp
// already parses at triage, so nothing downstream had to change, and a priority or due given here
// stops triage from guessing that field.
//
// LOCATION is optional and off until switched on (the choice is remembered). It is sent as a
// "loc" field beside the entry; persist/Importer.cpp turns it into the jot's last line.
//
// REMINDERS use Declarative Web Push (iOS 18.4+): window.pushManager, no service worker. iOS only
// offers push to a page opened from its Home Screen icon, and only asks for permission from a tap,
// hence the button. The subscription is re-sent every time the page opens, because iOS has been
// seen to drop them silently. Tapping a reminder opens this page with &r=<id>, which opens that
// todo's snooze / mute row.
//
// THE LOOK is the Instrument finish from the ZImageViewer material studies: a graphite scan for the
// plate, capsule trays, a switch with a centred light, one orange accent. The icon and the texture
// are base64 in jot/CaptureAssets.h (regenerate with jot/assets/make_assets.py). The icon is spliced
// in as a data: URI; the texture is too big to inline in a page that is never cached, so it is served
// beside the page (GET /jot/a/graphite.jpg, token-gated, cached by the phone). Nothing is loaded from
// any other origin.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "jot/CaptureAssets.h"

#include <cstdint>
#include <cstring>
#include <string>

inline const char* JotCapturePageHtml()
{
    return
R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="referrer" content="no-referrer">
<meta name="robots" content="noindex,nofollow">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Jot">
<meta name="theme-color" content="#0d0e0f">
<title>Jot</title>
<link rel="apple-touch-icon" href="data:image/png;base64,@@ICON_PNG@@">
<style>
/* INSTRUMENT: the material finish from the ZImageViewer studies (design/instrument in ZGameFrame).
   A real graphite scan for the plate, controls as recessed capsule trays with keys butted together,
   a switch with a centred indicator light, one orange accent. Dark only - the finish is a material,
   not a palette, and has no light form. What the studies taught, and this keeps to: no slate over
   large areas, no indicator dots on keys, no bright glows - the light is small and the lettering
   is etched rather than lit. */
:root{
  /* The studies' own scan at the studies' own scale: 1024px shown at 512px, colour untouched. */
  --graphite:url(@@GRAPHITE_URL@@) 0 0/512px;
  --text:#d6d9dc; --ink:#eceef0; --dim:rgba(255,255,255,.58); --faint:rgba(255,255,255,.36);
  --accent:#ff6a1a; --accent-hi:#ff8a3e; --danger:#ff6a55;
  --etch:0 -1px 0 rgba(0,0,0,.75);
  --well:#060707;
  --pocket:linear-gradient(rgba(0,0,0,.52),rgba(0,0,0,.38)),var(--graphite);
  --key:linear-gradient(180deg,rgba(255,255,255,.04),rgba(0,0,0,.18)),var(--graphite);
  --key-down:linear-gradient(rgba(0,0,0,.32),rgba(0,0,0,.12)),var(--graphite);
  --sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
  color-scheme:dark;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
/* manipulation: no double-tap-to-zoom, so iOS has no reason to hold a tap back before acting on it. */
html{background:#0d0e0f;touch-action:manipulation}
html,body{height:100%}
/* A FIXED COMPOSER over ONE SCROLLING PANE: nothing on the page moves when a control changes, and
   only the lists scroll. The ground is the studies' window plate: graphite under a top-left sheen
   and a falling shade. */
body{margin:0 auto;max-width:640px;height:100dvh;overflow:hidden;display:flex;flex-direction:column;
  color:var(--text);font:16px/1.45 var(--sans);padding:calc(12px + env(safe-area-inset-top)) 16px 0;
  background:radial-gradient(90% 60% at 18% 0%,rgba(255,255,255,.06),transparent 70%),
             linear-gradient(rgba(255,255,255,.015),rgba(0,0,0,.25)),var(--graphite);
  background-color:#141517}
button{font:inherit;color:inherit;border:0;padding:0;background:none;cursor:pointer}
/* Etched caps: the lettering of every label, heading and key. */
.caps,h2,.sw span,.seg button,.btn,.acts span,.due span{font-stretch:semi-condensed;text-transform:uppercase;
  font-weight:600;text-shadow:var(--etch)}

header{flex:none;display:flex;align-items:center;gap:10px;height:40px;padding:0 2px 8px;margin-bottom:12px;
  border-bottom:1px solid rgba(0,0,0,.75);box-shadow:0 1px 0 rgba(255,255,255,.07)}
.brand{font-size:14px;letter-spacing:.38em;color:var(--dim)}
.model{font:500 10px var(--mono);letter-spacing:.12em;color:var(--accent-hi);text-shadow:none}
#state{margin-left:auto;font:500 11px var(--mono);letter-spacing:.06em;color:var(--faint);text-transform:uppercase}
#state.bad{color:var(--danger)}
#state.warn{color:var(--accent-hi)}

/* The writing well: cut into the plate. */
textarea{flex:none;display:block;width:100%;height:17vh;min-height:96px;padding:12px 15px;border:0;border-radius:16px;resize:none;
  background:var(--well);color:var(--ink);caret-color:var(--accent);font:inherit;font-size:17px;line-height:1.45;
  box-shadow:inset 0 2px 5px #000,inset 0 0 0 1px rgba(0,0,0,.7),0 1px 0 rgba(255,255,255,.07)}
textarea::placeholder{color:rgba(255,255,255,.26)}
textarea:focus{outline:0;box-shadow:inset 0 2px 5px #000,inset 0 0 0 1px rgba(255,106,26,.35),0 1px 0 rgba(255,255,255,.07)}

.opts,.opts2{flex:none;display:flex;align-items:center}
.opts{gap:14px;margin-top:12px;height:42px}
#send{margin-left:auto;min-width:100px}
#locInfo{font:500 10px var(--mono);color:var(--faint)}
/* Priority and due are always there - showing them only with Todo on made everything below jump.
   Dimmed until Todo is on; picking either turns Todo on. */
.opts2{gap:10px;margin-top:10px;height:40px;transition:opacity .12s}
.opts2.dim{opacity:.42}
/* Switch: a capsule slot with a round key-cap knob; on, the knob slides over and its light comes on. */
.sw{display:flex;align-items:center;gap:9px;user-select:none;position:relative}
.sw input{position:absolute;opacity:0;width:1px;height:1px}
.sw i{position:relative;width:46px;height:26px;border-radius:999px;background:var(--well);flex:none;
  box-shadow:inset 0 1px 3px #000,inset 0 0 0 1px rgba(0,0,0,.6),0 1px 0 rgba(255,255,255,.07);transition:background .15s}
.sw i::after{content:"";position:absolute;left:3px;top:3px;width:20px;height:20px;border-radius:50%;
  background:radial-gradient(circle,#2a2c2e 0 3px,transparent 3.5px),var(--key);transition:left .15s;
  box-shadow:inset 0 1px 0 rgba(255,255,255,.14),inset 0 -1px 0 rgba(0,0,0,.6),0 1px 2px rgba(0,0,0,.6)}
.sw input:checked+i{background:linear-gradient(90deg,rgba(255,106,26,.18),rgba(255,106,26,0) 70%),var(--well)}
.sw input:checked+i::after{left:23px;
  background:radial-gradient(circle,#ffb27a 0 1.5px,var(--accent) 2px 3px,transparent 3.5px),var(--key);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.14),inset 0 -1px 0 rgba(0,0,0,.6),0 1px 2px rgba(0,0,0,.6),0 0 6px rgba(255,106,26,.25)}
.sw input:focus-visible+i{outline:1px solid rgba(255,106,26,.6);outline-offset:2px}
.sw span{font-size:12px;letter-spacing:.2em;color:var(--dim)}
.sw small{font:500 10.5px var(--mono);color:var(--faint);text-shadow:none}

/* Tray: a capsule pocket holding keys butted together, rounded only at the ends of the run. */
.seg,.acts{display:flex;padding:3px;border-radius:999px;background:var(--pocket);
  box-shadow:inset 0 2px 3px rgba(0,0,0,.75),inset 0 -1px 0 rgba(255,255,255,.08)}
.seg{flex:none}
.seg button,.acts span{padding:8px 13px;font-size:11.5px;letter-spacing:.16em;color:var(--dim);background:var(--key);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.09),inset 0 -1px 0 rgba(0,0,0,.55),inset -1px 0 0 rgba(0,0,0,.7)}
.seg button:first-child,.acts span:first-child{border-radius:999px 0 0 999px;padding-left:15px}
.seg button:last-child,.acts span:last-child{border-radius:0 999px 999px 0;padding-right:15px;
  box-shadow:inset 0 1px 0 rgba(255,255,255,.09),inset 0 -1px 0 rgba(0,0,0,.55)}
.seg button.on{background:var(--key-down);color:var(--accent-hi);
  box-shadow:inset 0 2px 3px rgba(0,0,0,.6),inset -1px 0 0 rgba(0,0,0,.7)}
.seg button:active,.acts span:active{background:var(--key-down)}

/* Due: a capsule well holding the native picker, a "DUE" mark while empty, and a clear key. */
.due{position:relative;flex:1;min-width:0;display:flex;align-items:center;height:38px;padding:0 34px;
  border-radius:999px;background:var(--well);
  box-shadow:inset 0 1px 3px #000,inset 0 0 0 1px rgba(0,0,0,.6),0 1px 0 rgba(255,255,255,.07)}
.due:focus-within{box-shadow:inset 0 1px 3px #000,inset 0 0 0 1px rgba(255,106,26,.35)}
/* The native picker is laid invisibly over the well - a tap anywhere opens it - and the value is
   shown in our own short form, because iOS draws the full "Oct 18, 2026 at 6:00 PM" and lets it
   run out of a narrow field. */
.due input{position:absolute;inset:0;width:100%;height:100%;margin:0;opacity:0;border:0}
.due .val,.due .ph{flex:1;min-width:0;text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;pointer-events:none}
.due .val{font:500 13.5px var(--mono);color:var(--ink)}
.due .ph{font-size:11px;letter-spacing:.2em;color:var(--faint);display:none}
.due.empty .ph{display:block}
.due.empty .val{display:none}
.due button{position:absolute;z-index:1;right:4px;top:4px;width:30px;height:30px;border-radius:50%;font-size:17px;line-height:30px;color:var(--faint)}
.due.empty button{visibility:hidden}

/* Keys standing on the plate. Primary carries the one small light. */
.btn{position:relative;display:inline-flex;align-items:center;justify-content:center;height:42px;padding:0 22px;
  border-radius:999px;font-size:12.5px;letter-spacing:.24em;color:rgba(255,255,255,.72);background:var(--key);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.1),inset 0 -1px 0 rgba(0,0,0,.6),0 0 0 1px rgba(0,0,0,.75),0 3px 6px rgba(0,0,0,.4)}
.btn:active{background:var(--key-down);box-shadow:inset 0 2px 3px rgba(0,0,0,.6),0 0 0 1px rgba(0,0,0,.75)}
.btn.primary{color:#fff;padding-left:34px;
  background:radial-gradient(70% 90% at 50% 140%,rgba(255,106,26,.2),transparent 70%),var(--key)}
.btn.primary::before{content:"";position:absolute;left:18px;top:50%;width:2px;height:12px;margin-top:-6px;
  background:var(--accent);box-shadow:0 0 4px rgba(255,106,26,.7)}
.btn:disabled{opacity:.38}
.btn.primary:disabled::before{background:#3a3c3e;box-shadow:none}
.btn.mini{height:26px;padding:0 12px;font-size:10.5px;letter-spacing:.18em}
#retry{display:none;color:var(--accent-hi)}
#len{font:500 11px var(--mono);color:var(--faint)}
#len.bad{color:var(--danger)}

/* The one scrolling region. */
.lists{flex:1;min-height:0;overflow-y:auto;overflow-x:hidden;-webkit-overflow-scrolling:touch;overscroll-behavior:contain;
  margin:14px -16px 0;padding:0 16px calc(24px + env(safe-area-inset-bottom));
  border-top:1px solid rgba(0,0,0,.75);box-shadow:inset 0 1px 0 rgba(255,255,255,.06)}
h2{display:flex;align-items:baseline;gap:8px;margin:20px 2px 10px;padding-bottom:9px;font-size:12px;letter-spacing:.32em;
  color:var(--dim);border-bottom:1px solid rgba(0,0,0,.75);box-shadow:0 1px 0 rgba(255,255,255,.07)}
/* Plates: anything listed sits on the ground as a slightly raised piece of the same graphite. */
.item,.todo{display:block;width:100%;text-align:left;margin-bottom:8px;padding:11px 14px;border-radius:14px;
  background:var(--key);color:inherit;
  box-shadow:inset 0 1px 0 rgba(255,255,255,.08),inset 0 -1px 0 rgba(0,0,0,.6),0 0 0 1px rgba(0,0,0,.65),0 4px 10px rgba(0,0,0,.35)}
.item .e,.todo .t{color:var(--ink);overflow-wrap:anywhere}
.item .e{white-space:pre-wrap;display:-webkit-box;-webkit-line-clamp:4;-webkit-box-orient:vertical;overflow:hidden}
.todo .t{font-weight:600}
.item .m,.todo .m{display:flex;gap:10px;flex-wrap:wrap;margin-top:5px;font:500 11px var(--mono);color:var(--faint)}
.item.q{position:relative}
.item.q::before{content:"";position:absolute;left:0;top:12px;bottom:12px;width:2px;background:var(--accent);
  box-shadow:0 0 4px rgba(255,106,26,.6)}
.item.q .m{color:var(--accent-hi)}
.chip{display:inline-block;margin:0 5px 6px 0;padding:2px 8px;border-radius:999px;background:var(--well);
  font:500 10px var(--mono);letter-spacing:.08em;text-transform:uppercase;color:var(--dim);
  box-shadow:inset 0 1px 2px #000,0 1px 0 rgba(255,255,255,.06)}
.empty,.note{font-size:13.5px;color:var(--faint);padding:4px 2px 8px}
.note{color:var(--dim)}
#remOn{width:100%;margin-bottom:12px}
.todo{position:relative}
.todo.today .t::before{content:"";display:inline-block;width:5px;height:5px;margin:0 9px 2px 0;border-radius:50%;
  background:var(--accent);box-shadow:0 0 4px rgba(255,106,26,.7);vertical-align:middle}
.todo .m .late{color:var(--danger)}
.todo .m .hi{color:var(--accent-hi)}
.todo.quiet{opacity:.5}
.acts{display:none;margin-top:12px;width:max-content;max-width:100%}
.todo.open .acts{display:flex}
.acts span{padding:7px 9px;font-size:10px;letter-spacing:.1em}
.acts span.mute{color:var(--danger)}
.acts span.done{color:var(--ink)}
/* SWIPE a card left: a Due soon todo is marked done, a sent jot leaves this phone's list. pan-y
   leaves vertical drags to the list's own scrolling; what the swipe will do is lettered underneath,
   shown only while a card is moving. */
.row{position:relative;margin-bottom:8px}
.row>.card{margin-bottom:0;touch-action:pan-y;position:relative;z-index:1}
.row .under{position:absolute;inset:0;display:flex;align-items:center;justify-content:flex-end;padding-right:20px;
  border-radius:14px;background:var(--well);font-size:11px;letter-spacing:.24em;color:var(--accent-hi);opacity:0;
  font-stretch:semi-condensed;text-transform:uppercase;font-weight:600;text-shadow:var(--etch)}
.row.swiping .under{opacity:1}
h2 .btn{margin-left:auto;align-self:center}
.btn.arm{color:var(--accent-hi)}

.compose{flex:none;display:flex;flex-direction:column;min-width:0}
/* LANDSCAPE: side by side, full width - the lists on the left, writing on the right where the right
   thumb reaches Send. Each column is fixed in place; only the lists scroll. The text box takes
   whatever height is left. */
@media (orientation:landscape) and (min-width:640px){
  body{max-width:none;flex-direction:row;
    padding:calc(8px + env(safe-area-inset-top)) calc(16px + env(safe-area-inset-right)) 0 calc(16px + env(safe-area-inset-left))}
  .compose{order:2;flex:0 0 52%;padding:0 0 calc(10px + env(safe-area-inset-bottom)) 18px}
  header{height:34px;margin-bottom:8px}
  textarea{flex:1;height:auto;min-height:56px}
  .opts{margin-top:10px}
  .opts2{margin-top:8px}
  .lists{order:1;margin:0 0 0 calc(-16px - env(safe-area-inset-left));
    padding:0 18px calc(20px + env(safe-area-inset-bottom)) calc(16px + env(safe-area-inset-left));
    border-top:0;border-right:1px solid rgba(0,0,0,.75);box-shadow:inset -1px 0 0 rgba(255,255,255,.06)}
  .lists h2{margin-top:12px}
}

/* Settings: a plate over a dimmed page. */
.gear{flex:none;width:32px;height:32px;margin-left:4px;border-radius:50%;display:grid;place-items:center;color:var(--dim);
  background:var(--key);box-shadow:inset 0 1px 0 rgba(255,255,255,.1),inset 0 -1px 0 rgba(0,0,0,.6),0 0 0 1px rgba(0,0,0,.75)}
.gear svg{width:17px;height:17px}
.sheet{position:fixed;inset:0;z-index:10;background:rgba(0,0,0,.55);display:flex;justify-content:center;align-items:flex-start;
  padding:calc(16px + env(safe-area-inset-top)) 14px calc(16px + env(safe-area-inset-bottom))}
.sheet[hidden]{display:none}
.plate{width:100%;max-width:520px;max-height:100%;overflow-y:auto;padding:16px 18px 20px;border-radius:18px;
  background:radial-gradient(100% 40% at 20% 0%,rgba(255,255,255,.05),transparent 70%),linear-gradient(rgba(0,0,0,.05),rgba(0,0,0,.3)),var(--graphite);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.12),inset 0 -1px 0 rgba(0,0,0,.7),0 0 0 1px #000,0 20px 40px rgba(0,0,0,.6)}
.plate .sh{display:flex;align-items:center;justify-content:space-between;padding-bottom:10px;
  border-bottom:1px solid rgba(0,0,0,.75);box-shadow:0 1px 0 rgba(255,255,255,.07)}
.plate .sh .caps{font-size:13px;letter-spacing:.32em;color:var(--dim)}
.plate h3{margin:18px 0 8px;font-size:11px;letter-spacing:.24em;color:var(--faint);font-stretch:semi-condensed;
  text-transform:uppercase;font-weight:600;text-shadow:var(--etch)}
.kv{display:flex;justify-content:space-between;gap:12px;padding:5px 2px;font-size:14px;color:var(--dim)}
.kv b{font:500 12.5px var(--mono);color:var(--ink);text-align:right}
.kv b.ok{color:#9fd8b0}.kv b.bad{color:var(--danger)}
.btns{display:flex;gap:10px;flex-wrap:wrap;margin-top:8px}
.plate p{margin:6px 2px;font-size:13.5px;line-height:1.5;color:var(--dim)}
#sState{margin-left:auto;margin-right:12px;font:500 11px var(--mono);letter-spacing:.06em;color:var(--faint);text-transform:uppercase;text-align:right}
#sState.bad{color:var(--danger)}
#sState.warn{color:var(--accent-hi)}
.spot{display:flex;gap:10px;margin-top:8px}
.spot input{flex:1;min-width:0;height:42px;padding:0 16px;border:0;border-radius:999px;background:var(--well);color:var(--ink);
  font:inherit;font-size:16px;caret-color:var(--accent);
  box-shadow:inset 0 1px 3px #000,inset 0 0 0 1px rgba(0,0,0,.6),0 1px 0 rgba(255,255,255,.07)}
.spot input:focus{outline:0;box-shadow:inset 0 1px 3px #000,inset 0 0 0 1px rgba(255,106,26,.35)}
.spot input::placeholder{color:rgba(255,255,255,.26)}
a.chip{text-decoration:none;color:var(--accent-hi)}
</style>
</head>
<body>
<div class="compose">
<header><span class="caps brand">Jot</span><span class="model">LOOM</span><span id="locInfo"></span><span id="state"></span>
  <span id="len"></span><button class="btn mini" id="retry">Retry</button>
  <button class="gear" id="gear" aria-label="Settings"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M4 7h10M18 7h2M4 17h4M12 17h8"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/></svg></button></header>
<textarea id="t" placeholder="What do you want to remember?" autofocus></textarea>
<div class="opts">
  <label class="sw"><input type="checkbox" id="todo"><i></i><span>Todo</span></label>
  <label class="sw"><input type="checkbox" id="loc"><i></i><span>Place</span></label>
  <button class="btn primary" id="send" disabled>Send</button>
</div>
<div class="opts2 dim" id="todoOpts">
  <div class="seg" id="prio"><button data-p="low">Low</button><button data-p="normal">Norm</button><button data-p="high">High</button></div>
  <div class="due empty" id="dueBox"><span class="ph">Due</span><span class="val" id="dueVal"></span><input type="datetime-local" id="due" aria-label="Due"><button id="dueClear" aria-label="Clear due date">&times;</button></div>
</div>
</div>
<div class="lists" id="lists">
  <h2 id="qh" style="display:none">Waiting to send</h2><div id="ql"></div>
  <h2 id="remH">Due soon</h2><div id="rem"></div>
  <h2>Recently sent<button class="btn mini" id="clr">Clear</button></h2><div id="sl"></div>
</div>
<div class="sheet" id="sheet" hidden><div class="plate">
  <div class="sh"><span class="caps">Settings</span><span id="sState"></span><button class="btn mini" id="sheetClose">Close</button></div>
  <h3>Reminders</h3>
  <div class="kv"><span>This phone</span><b id="sRem"></b></div>
  <div class="kv"><span>Notifications</span><b id="sNotif"></b></div>
  <div class="btns"><button class="btn" id="sTest">Send a test</button><button class="btn" id="sRemToggle"></button></div>
  <h3>Location</h3>
  <div class="kv"><span>Permission</span><b id="sLoc"></b></div>
  <h3>Permissions</h3>
  <p>A Home Screen web app can't open iOS Settings itself. Notifications: Settings &rarr; Notifications &rarr; Jot.
     Location: Settings &rarr; Privacy &amp; Security &rarr; Location Services &rarr; Safari Websites (or Jot, if it is listed).</p>
  <h3>Places</h3>
  <div id="sPlaces"></div>
  <div class="spot"><input id="spotName" maxlength="40" placeholder="Name this spot" autocomplete="off" enterkeyhint="done"><button class="btn" id="spotSave" disabled>Save</button></div>
  <p>Saves where the phone is now as a place in Loom. A jot sent with Place within about 150 m of it is tagged with
     the place - older ones too. Saving a name again moves it. Edit or remove places in Loom's known-places jot.</p>
  <h3>About</h3>
  <p>Jot writes straight into Loom. Todo, priority and due travel as tags; Place adds where you were.
     Reminders come from Loom's notifier for todos with a due date: a week, a day and an hour ahead depending on how
     far out it is, or at the time itself when it's sooner, plus an 8:00 summary. The badge counts what's due today
     or overdue.</p>
  <p>Recently sent is the last 30 jots sent from this phone, kept on the phone only. Swipe a card left to remove it
     from the list (the jot stays in Loom); swipe a Due soon card left to mark it done.</p>
  <div class="kv"><span>Version</span><b>2026.10.18.2</b></div>
</div></div>
<script>
'use strict';
const $=s=>document.querySelector(s);
const P=new URLSearchParams(location.search);
const K=P.get('k')||'';
const MAX=16000;   // jot caps a body at 16KB; leave room for the JSON around the entry
const KEEP=30;
function load(k,d){try{const v=localStorage.getItem('jot.'+k);return v==null?d:JSON.parse(v);}catch(e){return d;}}
function save(k,v){try{localStorage.setItem('jot.'+k,JSON.stringify(v));}catch(e){}}
let queue=load('queue',[]),sent=load('sent',[]),busy=false,authBad=false;
let prio='',fix=null,upcoming=null,openId=P.get('r')||'';
const auth={'Authorization':'Bearer '+K};

function pad(n){return String(n).padStart(2,'0');}
/* Local wall-clock time, the format persist/Importer.cpp parses. */
function stamp(d){return d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+' '+
  pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());}
function when(ts){
  const today=stamp(new Date()).slice(0,10);
  return ts.slice(0,10)===today?ts.slice(11,16):ts.slice(5,16);
}
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
/* Status shows in the header, and in the settings sheet while it is open, since the sheet covers
   the header. */
function state(msg,kind){
  for(const s of [$('#state'),$('#sState')]){s.textContent=msg||'';s.className=kind||'';}
}

/*------------------------------------------------------------------------------------------------
  Places: names from Loom's known-places jot (persist/Places.h), passed along by the notifier in
  upcoming.json, plus any this phone saved in the last ten minutes - those may not have come back
  through triage and the notifier yet, and they win over an older copy of the same name. */
let myPlaces=load('places',[]);
/* The same rule as PLACES::Slug, so a name means the same place on both sides. */
const slug=s=>String(s).toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-+/,'').slice(0,32).replace(/-+$/,'');
function places(){
  myPlaces=myPlaces.filter(p=>Date.now()-p.at<600000);
  const srv=(upcoming&&upcoming.places)||[];
  return srv.filter(s=>!myPlaces.some(p=>slug(p.name)===slug(s.name))).concat(myPlaces);
}
function metres(aLat,aLon,bLat,bLon){
  const r=Math.PI/180,dp=(bLat-aLat)*r,dl=(bLon-aLon)*r;
  const h=Math.sin(dp/2)**2+Math.cos(aLat*r)*Math.cos(bLat*r)*Math.sin(dl/2)**2;
  return 2*6371000*Math.asin(Math.sqrt(Math.min(1,h)));
}
function placeAt(lat,lon){
  let best=null,bd=0;
  for(const p of places()){const d=metres(p.lat,p.lon,lat,lon);if(d<=p.r&&(!best||d<bd)){best=p;bd=d;}}
  return best;
}

/* The leading [tags] of an entry shown as chips, the rest as the text. A sent jot sits in a row
   that can be swiped away; a queued one cannot. */
function itemHtml(i,bQueued,idx){
  const m=i.entry.match(/^(\s*\[[^\]]+\])+/);
  const tags=m?m[0].match(/\[[^\]]+\]/g).map(t=>t.slice(1,-1)):[];
  const body=m?i.entry.slice(m[0].length).replace(/^\s+/,''):i.entry;
  const pl=i.loc&&placeAt(i.loc.lat,i.loc.lon);
  const map=i.loc?'<a class="chip" href="https://maps.apple.com/?ll='+i.loc.lat+','+i.loc.lon+'&q='+
    encodeURIComponent(pl?pl.name:'Jot')+'" target="_blank" rel="noopener">'+(pl?esc(pl.name):'map')+'</a>':'';
  const card='<div class="item card'+(bQueued?' q':'')+'">'+
    (tags.length||map?'<div>'+tags.map(t=>'<span class="chip">'+esc(t)+'</span>').join('')+map+'</div>':'')+
    '<div class="e">'+esc(body)+'</div><div class="m">'+when(i.ts)+(bQueued?' · queued':'')+'</div></div>';
  return bQueued?card:'<div class="row" data-i="'+idx+'"><div class="under">Remove</div>'+card+'</div>';
}
/* Split so a tap on a control repaints only the controls. Rebuilding both lists - every item a
   textured plate with shadows - on every keystroke and tap was the lag. */
function renderLists(){
  $('#qh').style.display=queue.length?'':'none';
  $('#ql').innerHTML=queue.map(i=>itemHtml(i,true)).join('');
  $('#sl').innerHTML=sent.length?sent.map((i,n)=>itemHtml(i,false,n)).join(''):'<div class="empty">Nothing yet</div>';
  disarmClear();
  $('#clr').style.visibility=sent.length?'':'hidden';
}
/* Clear asks twice: the first tap arms it for three seconds. */
let clrArmed=0;
function disarmClear(){clrArmed=0;$('#clr').textContent='Clear';$('#clr').classList.remove('arm');}
function renderControls(){
  $('#todoOpts').classList.toggle('dim',!$('#todo').checked);
  document.querySelectorAll('#prio button').forEach(b=>b.classList.toggle('on',b.dataset.p===prio));
  $('#dueBox').classList.toggle('empty',!$('#due').value);
  $('#dueVal').textContent=$('#due').value?shortWhen(new Date($('#due').value),false):'';
  $('#retry').style.display=queue.length&&!busy?'inline-flex':'none';
  const v=$('#t').value,n=v.length>MAX/4?new Blob([v]).size:v.length;
  $('#len').textContent=n>MAX*0.8?n+' / '+MAX:'';
  $('#len').className=n>MAX?'bad':'';
  $('#send').disabled=!K||!v.trim()||n>MAX;
}
function render(){renderLists();renderControls();}

/*------------------------------------------------------------------------------------------------
  Sending */
async function post(item){
  const body={ts:item.ts,entry:item.entry};
  if(item.loc)body.loc=item.loc;
  const r=await fetch('/jot',{method:'POST',cache:'no-store',
    headers:Object.assign({'Content-Type':'application/json'},auth),body:JSON.stringify(body)});
  if(r.status===204||r.ok)return;
  throw Object.assign(new Error('HTTP '+r.status),{status:r.status});
}
/* Oldest first, stopping at the first failure so one bad moment costs one request, not many. */
async function flush(manual){
  if(busy||!queue.length||(authBad&&!manual))return;
  busy=true;render();state('sending…');
  try{
    while(queue.length){
      await post(queue[0]);
      sent.unshift(queue.shift());sent=sent.slice(0,KEEP);
      save('queue',queue);save('sent',sent);
    }
    authBad=false;state('sent','');setTimeout(()=>{if(!busy&&!queue.length)state('');},2500);
    setTimeout(loadUpcoming,90000);   // triage and the notifier take a minute or so to catch up
  }catch(e){
    if(e.status===404){authBad=true;state('key rejected','bad');}
    else if(e.status===429)state('slow down - try in a minute','warn');
    else state(navigator.onLine?'not sent - will retry':'offline - queued','warn');
  }finally{busy=false;render();}
}

/* Location: a fix no older than two minutes is reused, so sending is instant when the switch has
   been on a while. Coordinates are rounded to about a metre - a phone fix claims no better. */
function locate(fresh){
  return new Promise(res=>{
    if(!navigator.geolocation)return res(null);
    navigator.geolocation.getCurrentPosition(p=>{
      fix={lat:+p.coords.latitude.toFixed(5),lon:+p.coords.longitude.toFixed(5),
           acc:Math.round(p.coords.accuracy),at:Date.now()};
      showFix();res(fix);
    },()=>{if($('#loc').checked)$('#locInfo').textContent='unavailable';res(null);},
    {enableHighAccuracy:true,timeout:fresh?15000:8000,maximumAge:fresh?0:120000});
  });
}
/* Beside the switch: the place's name when the phone is at one, otherwise how good the fix is. */
function showFix(){
  if(!$('#loc').checked||!fix){$('#locInfo').textContent='';return;}
  const pl=placeAt(fix.lat,fix.lon);
  $('#locInfo').textContent=pl?'at '+pl.name:'±'+fix.acc+' m';
}
async function send(){
  const text=$('#t').value.replace(/\s+$/,'');
  if(!text.trim()||!K)return;
  let pre='';
  if($('#todo').checked){
    pre='[todo] '+(prio?'[priority:'+prio+'] ':'');
    // datetime-local gives "2026-10-20T12:00" in local time; tags are lowercase.
    const due=$('#due').value;
    if(due)pre+='[due:'+due.slice(0,16).toLowerCase()+'] ';
  }
  const item={ts:stamp(new Date()),entry:pre+text};
  if($('#loc').checked){
    $('#send').disabled=true;state('locating…');
    const f=fix&&Date.now()-fix.at<120000?fix:await locate();
    if(f)item.loc={lat:f.lat,lon:f.lon,acc:f.acc};
  }
  queue.push(item);save('queue',queue);
  $('#t').value='';$('#todo').checked=false;prio='';$('#due').value='';
  render();flush(true);
}

/*------------------------------------------------------------------------------------------------
  Reminders */
const standalone=navigator.standalone===true||matchMedia('(display-mode: standalone)').matches;
/* iOS has no Notification object at all outside a Home Screen web app. */
const perm=()=>window.Notification?Notification.permission:'default';
function b64urlBytes(s){
  const b=atob(s.replace(/-/g,'+').replace(/_/g,'/')+'='.repeat((4-s.length%4)%4));
  return Uint8Array.from(b,c=>c.charCodeAt(0));
}
async function sendSubscription(sub){
  const r=await fetch('/jot/push',{method:'PUT',cache:'no-store',
    headers:Object.assign({'Content-Type':'application/json'},auth),body:JSON.stringify(sub.toJSON())});
  if(!(r.status===204||r.ok))throw new Error('HTTP '+r.status);
}
/* Called from the button (a tap is required for the permission prompt), and on every open once
   permission is granted, to keep the server's copy fresh. */
async function enableReminders(fromTap){
  if(!window.pushManager||!window.Notification||!upcoming||!upcoming.vapid_key)return false;
  if(perm()!=='granted'){
    if(!fromTap)return false;
    if(await Notification.requestPermission()!=='granted'){state('notifications blocked','warn');return false;}
  }
  try{
    let sub=await window.pushManager.getSubscription();
    if(!sub)sub=await window.pushManager.subscribe({userVisibleOnly:true,
      applicationServerKey:b64urlBytes(upcoming.vapid_key)});
    await sendSubscription(sub);
    upcoming.subscribed=true;renderRem();
    if(fromTap)state('reminders on','');
    return true;
  }catch(e){state('could not turn reminders on','warn');return false;}
}
async function loadUpcoming(){
  if(!K)return;
  try{
    const r=await fetch('/jot/upcoming',{cache:'no-store',headers:auth});
    upcoming=r.status===200?await r.json():null;
  }catch(e){}
  applyPending();
  renderRem();
  renderLists();showFix();   // the place names may have changed
  if(upcoming&&standalone&&window.pushManager&&perm()==='granted')enableReminders(false);
}
/* One short way of saying when, used by the due field and the Due soon list: "Today 6:00 PM",
   "Tomorrow 9:00 AM", "Tue 6:00 PM" within the week, "Oct 30 6:00 PM" beyond it. */
const clockOf=d=>d.toLocaleTimeString(undefined,{hour:'numeric',minute:'2-digit'});
function daysFrom(d){
  const now=new Date(),a=new Date(d.getFullYear(),d.getMonth(),d.getDate()),b=new Date(now.getFullYear(),now.getMonth(),now.getDate());
  return Math.round((a-b)/864e5);
}
function shortWhen(d,allDay){
  const days=daysFrom(d),clock=allDay?'':' '+clockOf(d);
  if(days===0)return 'Today'+clock;
  if(days===1)return 'Tomorrow'+clock;
  if(days>1&&days<7)return d.toLocaleDateString(undefined,{weekday:'short'})+clock;
  return d.toLocaleDateString(undefined,{month:'short',day:'numeric'})+clock;
}
function dueText(t){
  const d=new Date(t.all_day?t.due+'T09:00':t.due),now=new Date(),days=daysFrom(d);
  if(d<now&&(days<0||!t.all_day))return{late:true,s:'overdue · '+shortWhen(d,t.all_day)};
  return{today:days===0,s:shortWhen(d,t.all_day)};
}
function renderRem(){
  const box=$('#rem');let h='';
  if(!upcoming){box.innerHTML='<div class="empty">Reminders are not set up on the server yet.</div>';return;}
  if(!standalone)h+='<div class="note">Add this page to your Home Screen (Share → Add to Home Screen) and open it from there to get reminders.</div>';
  else if(!window.pushManager)h+='<div class="note">Reminders need iOS 18.4 or later.</div>';
  else if(!upcoming.subscribed||perm()!=='granted')h+='<button class="btn primary" id="remOn">Turn on reminders</button>';
  const list=upcoming.todos||[];
  if(!list.length)h+='<div class="empty">No todos with a due date.</div>';
  for(const t of list){
    const w=dueText(t),sn=t.snoozed_until&&t.snoozed_until*1000>Date.now();
    const cls='todo'+(w.late?' late':w.today?' today':'')+(t.muted?' quiet':'')+(String(t.id)===openId?' open':'');
    h+='<div class="row"><div class="under">Done</div><button class="card '+cls+'" data-id="'+t.id+'"><div class="t">'+esc(t.title)+'</div><div class="m">'+
       '<span class="'+(w.late?'late':'')+'">'+esc(w.s)+'</span>'+
       (t.priority?'<span'+(t.priority==='high'?' class="hi"':'')+'>'+esc(t.priority)+'</span>':'')+
       (t.muted?'<span>muted</span>':'')+
       (sn?'<span>snoozed until '+pad(new Date(t.snoozed_until*1000).getHours())+':'+pad(new Date(t.snoozed_until*1000).getMinutes())+'</span>':'')+
       '</div><div class="acts">'+
       '<span class="done" data-a="done">Done</span><span data-a="snooze" data-s="1h">1 hr</span><span data-a="snooze" data-s="eve">Evening</span>'+
       '<span data-a="snooze" data-s="tmw">Tomorrow</span>'+
       (t.muted?'<span data-a="unmute">Unmute</span>':'<span class="mute" data-a="mute">Mute</span>')+
       '</div></button></div>';
  }
  box.innerHTML=h;
  if(openId){const el=box.querySelector('.todo.open');if(el)el.scrollIntoView({block:'center'});openId='';}
  updateBadge();
}
/* Each push sets the icon's badge; setting it here too means opening the app always leaves it
   right - the count of todos due today or overdue, not muted - and clears it when there are none. */
function updateBadge(){
  if(!upcoming||!('setAppBadge' in navigator))return;
  const now=new Date(),end=new Date(now.getFullYear(),now.getMonth(),now.getDate()+1);
  const n=(upcoming.todos||[]).filter(t=>!t.muted&&new Date(t.all_day?t.due+'T00:00':t.due)<end).length;
  (n?navigator.setAppBadge(n):navigator.clearAppBadge()).catch(()=>{});
}
function snoozeUntil(which){
  const now=new Date(),d=new Date(now);
  if(which==='1h')return Math.floor(now.getTime()/1000)+3600;
  if(which==='eve'){d.setHours(18,0,0,0);if(d<=now)d.setTime(now.getTime()+3*3600e3);}
  else{d.setDate(d.getDate()+1);d.setHours(8,0,0,0);}
  return Math.floor(d.getTime()/1000);
}
/* Actions this phone has sent that the notifier may not have applied yet. It works through them
   once a minute and stamps upcoming.json with when that run started, so until a list newer than the
   action arrives, the action is laid over it - otherwise reopening the app within the minute shows
   a todo just marked done as still overdue, and puts its badge back. */
let pend=load('pend',{});
function applyPending(){
  if(!upcoming)return;
  const gen=(upcoming.generated||0)*1000;
  for(const id of Object.keys(pend))if(pend[id].at<gen)delete pend[id];
  save('pend',pend);
  upcoming.todos=(upcoming.todos||[]).filter(t=>!(pend[t.id]&&pend[t.id].a==='done'));
  for(const t of upcoming.todos){
    const p=pend[t.id];if(!p)continue;
    if(p.a==='mute')t.muted=true;
    if(p.a==='unmute')t.muted=false;
    if(p.a==='snooze')t.snoozed_until=p.until;
  }
}
async function remind(id,action,which){
  const body={id:Number(id),action};
  if(action==='snooze')body.until=snoozeUntil(which);
  try{
    const r=await fetch('/jot/remind',{method:'POST',cache:'no-store',
      headers:Object.assign({'Content-Type':'application/json'},auth),body:JSON.stringify(body)});
    if(!(r.status===204||r.ok))throw new Error('HTTP '+r.status);
    // Shown at once and remembered until the notifier has applied it.
    pend[id]={a:action,until:body.until,at:Date.now()};
    applyPending();
    state(action==='snooze'?'snoozed':action==='mute'?'muted':action==='done'?'done':'unmuted','');
    renderRem();
  }catch(e){state('not saved - try again','warn');renderRem();}   // renderRem brings back a swiped card
}

/* Swipe left. Past a third of the card's width it goes and onSwipe runs; short of that it springs
   back. A drag that starts out more vertical than horizontal is left to the list's scrolling, and
   the browser cancels the pointer once it scrolls. A click straight after a swipe is not a tap. */
let swipedAt=0;
function swipeable(box,onSwipe){
  let c=null,id=0,x0=0,y0=0,dx=0,live=false;
  const back=()=>{c.style.transition='transform .18s ease-out';c.style.transform='';
    const r=c.parentNode;setTimeout(()=>r.classList.remove('swiping'),180);};
  box.addEventListener('pointerdown',e=>{
    const el=e.target.closest('.row>.card');
    if(!el||e.target.closest('a')||(e.pointerType==='mouse'&&e.button))return;
    c=el;id=e.pointerId;x0=e.clientX;y0=e.clientY;dx=0;live=false;
  });
  box.addEventListener('pointermove',e=>{
    if(!c||e.pointerId!==id)return;
    const mx=e.clientX-x0,my=e.clientY-y0;
    if(!live){
      if(Math.abs(mx)<10&&Math.abs(my)<10)return;
      if(Math.abs(my)>=Math.abs(mx)||mx>0){c=null;return;}
      live=true;c.setPointerCapture(id);c.style.transition='none';c.parentNode.classList.add('swiping');
    }
    dx=Math.min(0,mx);c.style.transform='translateX('+dx+'px)';
  });
  box.addEventListener('pointerup',e=>{
    if(!c||e.pointerId!==id)return;
    const el=c;
    if(live){
      swipedAt=Date.now();
      if(dx<-el.offsetWidth/3){
        el.style.transition='transform .16s ease-in';el.style.transform='translateX(-110%)';
        setTimeout(()=>onSwipe(el),160);
      }else back();
    }
    c=null;
  });
  box.addEventListener('pointercancel',e=>{if(c&&e.pointerId===id){if(live)back();c=null;}});
}
swipeable($('#rem'),el=>remind(el.dataset.id,'done'));
swipeable($('#sl'),el=>{
  const i=Number(el.parentNode.dataset.i);
  if(i>=0&&i<sent.length){sent.splice(i,1);save('sent',sent);}
  renderLists();
});

/*------------------------------------------------------------------------------------------------ */
$('#t').addEventListener('input',renderControls);
$('#t').addEventListener('keydown',e=>{if(e.key==='Enter'&&(e.metaKey||e.ctrlKey)){e.preventDefault();send();}});
$('#send').onclick=send;
$('#retry').onclick=()=>flush(true);
/* Todo off clears its priority and due; picking either turns Todo on. */
$('#todo').addEventListener('change',()=>{if(!$('#todo').checked){prio='';$('#due').value='';}renderControls();});
$('#due').addEventListener('change',()=>{if($('#due').value)$('#todo').checked=true;renderControls();});
$('#dueClear').onclick=()=>{$('#due').value='';renderControls();};
$('#prio').addEventListener('click',e=>{
  const b=e.target.closest('button');if(!b)return;
  prio=prio===b.dataset.p?'':b.dataset.p;
  if(prio)$('#todo').checked=true;
  renderControls();
});
$('#loc').checked=load('loc',false);
$('#loc').addEventListener('change',()=>{save('loc',$('#loc').checked);
  if($('#loc').checked)locate();else $('#locInfo').textContent='';});
$('#rem').addEventListener('click',e=>{
  if(e.target.id==='remOn'){enableReminders(true);return;}
  if(Date.now()-swipedAt<400)return;
  const a=e.target.closest('[data-a]'),card=e.target.closest('.todo');
  if(a&&card){remind(card.dataset.id,a.dataset.a,a.dataset.s);return;}
  if(card)card.classList.toggle('open');
});
/*------------------------------------------------------------------------------------------------
  Settings */
async function renderSheet(){
  const sub=upcoming&&upcoming.subscribed&&perm()==='granted';
  $('#sRem').textContent=!standalone?'open from the Home Screen icon':!window.pushManager?'needs iOS 18.4+':sub?'on':'off';
  $('#sRem').className=sub?'ok':'';
  const p=perm();
  $('#sNotif').textContent=p==='granted'?'allowed':p==='denied'?'blocked':'not asked yet';
  $('#sNotif').className=p==='granted'?'ok':p==='denied'?'bad':'';
  $('#sRemToggle').textContent=sub?'Turn reminders off':'Turn reminders on';
  $('#sTest').disabled=!sub;
  let loc='unknown';
  try{if(navigator.permissions)loc=(await navigator.permissions.query({name:'geolocation'})).state;}catch(e){}
  $('#sLoc').textContent=loc==='granted'?'allowed':loc==='denied'?'blocked':loc==='prompt'?'asks when used':loc;
  $('#sLoc').className=loc==='granted'?'ok':loc==='denied'?'bad':'';
  const here=fix&&Date.now()-fix.at<300000?placeAt(fix.lat,fix.lon):null;
  const pl=places();
  $('#sPlaces').innerHTML=pl.length?pl.map(p=>'<div class="kv"><span>'+esc(p.name)+'</span><b'+(p===here?' class="ok">here':'>'+
    Math.round(p.r)+' m')+'</b></div>').join(''):'<p>None yet.</p>';
  $('#spotSave').disabled=!slug($('#spotName').value);
}
/* Names the spot the phone is at: a "[set-place:Name]" jot with a fresh fix, which Loom's triage
   writes into the known-places list. Shown at once from this phone's own copy. */
async function saveSpot(){
  const name=$('#spotName').value.replace(/[\[\]\n]/g,'').trim().slice(0,40);
  if(!slug(name))return;
  $('#spotSave').disabled=true;state('locating…');
  const f=await locate(true);
  if(!f){state('no location - see Permission','warn');renderSheet();return;}
  queue.push({ts:stamp(new Date()),entry:'[set-place:'+name+']',loc:{lat:f.lat,lon:f.lon,acc:f.acc}});
  save('queue',queue);
  myPlaces=myPlaces.filter(p=>slug(p.name)!==slug(name)&&Date.now()-p.at<600000);
  myPlaces.push({name,lat:f.lat,lon:f.lon,r:150,at:Date.now()});
  save('places',myPlaces);
  $('#spotName').value='';
  await flush(true);
  if(!queue.length)state('saved '+name+(f.acc>75?' (±'+f.acc+' m)':''),'');
  showFix();renderSheet();
}
async function remindersOff(){
  try{
    await fetch('/jot/push',{method:'DELETE',cache:'no-store',headers:auth});
    const s=window.pushManager&&await window.pushManager.getSubscription();
    if(s)await s.unsubscribe();
    if(upcoming)upcoming.subscribed=false;
    state('reminders off','');
  }catch(e){state('could not turn reminders off','warn');}
}
$('#gear').onclick=()=>{$('#sheet').hidden=false;renderSheet();};
$('#sheetClose').onclick=()=>{$('#sheet').hidden=true;};
$('#sheet').addEventListener('click',e=>{if(e.target.id==='sheet')$('#sheet').hidden=true;});
$('#sRemToggle').onclick=async()=>{
  if(upcoming&&upcoming.subscribed&&perm()==='granted')await remindersOff();else await enableReminders(true);
  renderRem();renderSheet();
};
$('#sTest').onclick=async()=>{
  try{
    const r=await fetch('/jot/remind',{method:'POST',cache:'no-store',
      headers:Object.assign({'Content-Type':'application/json'},auth),body:JSON.stringify({action:'test'})});
    if(!(r.status===204||r.ok))throw new Error();
    state('test on its way - within a minute','');
  }catch(e){state('could not send a test','warn');}
};
$('#clr').onclick=()=>{
  if(clrArmed&&Date.now()-clrArmed<3000){sent=[];save('sent',sent);renderLists();return;}
  const at=clrArmed=Date.now();$('#clr').textContent='Tap to clear';$('#clr').classList.add('arm');
  setTimeout(()=>{if(clrArmed===at)disarmClear();},3000);
};
$('#spotName').addEventListener('input',()=>{$('#spotSave').disabled=!slug($('#spotName').value);});
$('#spotName').addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();saveSpot();}});
$('#spotSave').onclick=saveSpot;

window.addEventListener('online',()=>flush(false));
document.addEventListener('visibilitychange',()=>{if(!document.hidden){flush(false);loadUpcoming();
  if($('#loc').checked)locate();}});
if($('#loc').checked)locate();
if(P.get('v')==='today'||openId)setTimeout(()=>$('#remH').scrollIntoView({block:'start'}),300);
render();flush(false);loadUpcoming();
</script>
</body>
</html>
)HTML";
}

// The page with its icon spliced in as a data: URI and the texture's URL filled in, built once on
// first request. The URL carries the token like the page's own does - the texture route is gated the
// same way - and the page is only ever served to a caller who presented that token.
inline const std::string& JotCapturePage(const std::string& sToken)
{
    static const std::string sPage = [&sToken]
    {
        std::string s = JotCapturePageHtml();
        const auto Splice = [&s](const std::string& sKey, const std::string& sValue)
        {
            for (size_t n = s.find(sKey); n != std::string::npos; n = s.find(sKey, n + sValue.size()))
                s.replace(n, sKey.size(), sValue);
        };
        Splice("@@ICON_PNG@@", JotIconPngBase64());
        Splice("@@GRAPHITE_URL@@", "\"/jot/a/graphite.jpg?k=" + sToken + "\"");
        return s;
    }();
    return sPage;
}

// The texture's bytes, decoded once.
inline const std::string& JotGraphiteJpg()
{
    static const std::string sBytes = []
    {
        static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        uint32_t nAcc = 0;
        int nBits = 0;
        for (const char* p = JotGraphiteJpgBase64(); *p && *p != '='; ++p)
        {
            const char* pAt = std::strchr(kAlphabet, *p);
            if (!pAt)
                continue;
            nAcc = (nAcc << 6) | static_cast<uint32_t>(pAt - kAlphabet);
            nBits += 6;
            if (nBits >= 8)
            {
                nBits -= 8;
                out.push_back(static_cast<char>((nAcc >> nBits) & 0xFF));
            }
        }
        return out;
    }();
    return sBytes;
}
