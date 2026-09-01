#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

//////////////////////////////////////////////////////////////////////////////////////////////////
// Dashboard - the operator UI, embedded in the binary and served at "/".
//
// EMBEDDED rather than read from disk: Loom is meant to be one file you drop on the Linux box and
// run under systemd. An asset directory is one more thing to get wrong in a unit file, and a
// dashboard that 404s because the working directory moved is worse than no dashboard.
//
// SERVED BY LOOM rather than opened from disk: same origin means the page calls the API with no
// CORS preflight and no configuration. It also means this cannot be a hosted page - a sandboxed
// remote page cannot reach 127.0.0.1.
//
// Vanilla everything. No framework, no build step, no CDN - consistent with a project that vendors
// its dependencies, and it keeps this file editable in place.
//
//------------------------------------------------------------------------------------------------
// DESIGN NOTES, because the reasoning is not recoverable from the CSS:
//
//   RANKING IS THE POINT, SO IT IS VISIBLE. Every result carries a left accent rail whose opacity
//   scales with its score relative to the best hit, and matched terms are marked in the text. The
//   question this tool exists to answer is "is Loom's ranking good enough to replace the
//   hand-maintained cluster index files", and that is unanswerable from a list that hides why
//   things ranked where they did.
//
//   MEMORIES AND JOTS LOOK DIFFERENT. A named jot with a summary is a durable memory; a bare one is
//   a passing thought. Same record type, different kind of thing - and flattening them into
//   identical rows was the worst failure of the first version.
//
//   TIME IS SHOWN LOCALLY. Ids are UTC microseconds, but the imported entries were written in local
//   wall-clock time. Formatting happens in the browser from the id, so an entry written at 13:09
//   reads 13:09 rather than the 20:09 a raw UTC field shows. Recent groups by day, which is what
//   makes two years of imported history legible instead of one flat scroll.
//
//   CHROME RECEDES. Metadata is small, monospaced and faint; content is the only thing carrying
//   full contrast. Everything that is not a memory gets out of the way of the ones that are.
//////////////////////////////////////////////////////////////////////////////////////////////////

inline const char* LoomDashboardHtml()
{
    return
R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Loom</title>
<style>
:root{
  --bg:#faf9f6; --panel:#fff; --sunk:#f3f1ec;
  --ink:#1c1a17; --body:#3a3630; --dim:#6f6a61; --faint:#a49d92;
  --line:#e7e3da; --line-soft:#f0ece4;
  --accent:#6d5bd0; --accent-ink:#4e3fae; --accent-wash:#f0edfd;
  --warn:#a1620c; --warn-wash:#fdf6ec; --warn-line:#e8c99a;
  --bad:#a32c22; --bad-wash:#fdf1f0; --bad-line:#e7b3ad;
  --good:#2c6b45; --good-wash:#f0f8f3; --good-line:#a9d4bd;
  --mark:#fdf0a8;
  --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
  --sans:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",sans-serif;
  --r:7px;
}
@media (prefers-color-scheme:dark){
:root{
  --bg:#141317; --panel:#1b1a20; --sunk:#232128;
  --ink:#eceae5; --body:#cfcbc3; --dim:#948e85; --faint:#6b665e;
  --line:#2c2a32; --line-soft:#25232a;
  --accent:#a493f5; --accent-ink:#c0b3ff; --accent-wash:#272243;
  --warn:#e0a458; --warn-wash:#2a2114; --warn-line:#5c4520;
  --bad:#e08279; --bad-wash:#2b1917; --bad-line:#5e2f2a;
  --good:#6cc294; --good-wash:#152720; --good-line:#2c5340;
  --mark:#5c5220;
}}
*{box-sizing:border-box}
html,body{height:100%}
body{margin:0;background:var(--bg);color:var(--body);font:14px/1.55 var(--sans);
  -webkit-font-smoothing:antialiased}
::selection{background:var(--accent-wash)}

/* ---------- header ---------- */
header{display:flex;align-items:center;gap:20px;padding:0 20px;height:50px;
  border-bottom:1px solid var(--line);background:var(--panel);position:sticky;top:0;z-index:30}
.brand{font-size:16px;font-weight:600;letter-spacing:-.01em;color:var(--ink);
  display:flex;align-items:center;gap:9px}
.brand .mk{width:15px;height:15px;flex:none;opacity:.9}
.stats{margin-left:auto;display:flex;gap:20px;align-items:center}
.stat{display:flex;flex-direction:column;line-height:1.15}
.stat b{font:12px/1.2 var(--mono);color:var(--ink);font-weight:600}
.stat span{font-size:9.5px;text-transform:uppercase;letter-spacing:.09em;color:var(--faint)}
.live{display:flex;align-items:center;gap:6px;font-size:11px;color:var(--dim)}
.dot{width:6px;height:6px;border-radius:50%;background:var(--good);flex:none}
.dot.off{background:var(--bad)}

/* ---------- nav ---------- */
nav{display:flex;gap:1px;padding:0 20px;background:var(--panel);
  border-bottom:1px solid var(--line);position:sticky;top:50px;z-index:29}
nav button{background:none;border:0;padding:0 13px;height:38px;cursor:pointer;
  font:13px var(--sans);color:var(--dim);position:relative}
nav button:hover{color:var(--ink)}
nav button.on{color:var(--ink);font-weight:600}
nav button.on::after{content:"";position:absolute;left:9px;right:9px;bottom:-1px;height:2px;
  background:var(--accent);border-radius:2px 2px 0 0}
nav .pill{margin-left:6px;font:10px var(--mono);background:var(--sunk);color:var(--dim);
  padding:1px 5px;border-radius:20px;vertical-align:1px}
nav button.on .pill{background:var(--accent-wash);color:var(--accent-ink)}

/* search pinned in the nav on every view - it stays put across tab switches */
.navsearch{position:relative;display:flex;align-items:center;margin-left:auto}
.navsearch svg{position:absolute;left:9px;width:12px;height:12px;color:var(--faint)}
.navsearch input{width:190px;font:12.5px var(--sans);color:var(--ink);background:var(--sunk);
  border:1px solid var(--line);border-radius:20px;height:27px;padding:0 26px}
.navsearch input::placeholder{color:var(--faint)}
.navsearch input:focus{outline:0;border-color:var(--accent)}
.navsearch .kbd{position:absolute;right:8px;font:9.5px var(--mono);color:var(--faint);
  border:1px solid var(--line);border-radius:4px;padding:0 4px;background:var(--panel);
  pointer-events:none}

/* ---------- layout ---------- */
main{display:grid;grid-template-columns:minmax(0,1fr) 400px;height:calc(100vh - 89px)}
@media(max-width:1000px){main{grid-template-columns:1fr}#detail{display:none}
  body.editing #list{display:none}body.editing #detail{display:block}}
.pane{overflow-y:auto;overscroll-behavior:contain}
#list{padding:18px 20px 60px}
#detail{border-left:1px solid var(--line);background:var(--panel);padding:18px 20px 60px}

/* ---------- controls ---------- */
.search{position:relative;margin-bottom:12px}
.search input{width:100%;font:15px var(--sans);color:var(--ink);background:var(--panel);
  border:1px solid var(--line);border-radius:var(--r);padding:10px 12px 10px 34px}
.search svg{position:absolute;left:11px;top:11px;width:14px;height:14px;color:var(--faint)}
.search input:focus{outline:0;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-wash)}
.search input::placeholder{color:var(--faint)}
.kbd{position:absolute;right:10px;top:9px;font:10px var(--mono);color:var(--faint);
  border:1px solid var(--line);border-radius:4px;padding:1px 5px;background:var(--sunk)}

/* ---------- filter bar (Search view: sort, time range, memories-only) ---------- */
.filterbar{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:12px}
.fgroup{display:flex;align-items:center;gap:7px}
.flabel{font:10px var(--mono);text-transform:uppercase;letter-spacing:.06em;color:var(--faint)}
.togglewrap{display:flex;align-items:center;gap:8px;margin-left:auto}
.togglewrap span{font-size:12.5px;color:var(--dim)}
.toggle{width:30px;height:17px;border-radius:20px;background:var(--sunk);border:1px solid var(--line);
  position:relative;flex:none;cursor:pointer}
.toggle i{position:absolute;top:1px;left:1px;width:13px;height:13px;border-radius:50%;
  background:var(--faint);transition:left .12s}
.toggle.on{background:var(--accent-wash);border-color:var(--accent)}
.toggle.on i{left:14px;background:var(--accent)}

input,select,textarea{font:14px var(--sans);color:var(--ink);background:var(--panel);
  border:1px solid var(--line);border-radius:6px;padding:7px 9px;width:100%}
textarea{resize:vertical;line-height:1.55}
input:focus,select:focus,textarea:focus{outline:0;border-color:var(--accent);
  box-shadow:0 0 0 3px var(--accent-wash)}

button.btn{font:13px var(--sans);color:var(--ink);background:var(--panel);cursor:pointer;
  border:1px solid var(--line);border-radius:6px;padding:7px 12px;width:auto}
button.btn:hover{border-color:var(--dim)}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff;font-weight:600}
button.primary:hover{filter:brightness(1.07);border-color:var(--accent)}
button.ghost{border-color:transparent;color:var(--dim)}
button.ghost:hover{color:var(--ink);border-color:var(--line)}
button.danger:hover{border-color:var(--bad);color:var(--bad)}
button.tiny{font-size:12px;padding:4px 9px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}

/* ---------- filter chips ---------- */
.chips{display:flex;gap:5px;flex-wrap:wrap;margin-bottom:14px}
.chip{border:1px solid var(--line);border-radius:20px;padding:3px 10px;font-size:12px;
  cursor:pointer;color:var(--dim);background:var(--panel);white-space:nowrap;user-select:none}
.chip:hover{border-color:var(--dim);color:var(--ink)}
.chip.on{background:var(--accent);border-color:var(--accent);color:#fff;font-weight:500}
.chip b{font:10px var(--mono);opacity:.65;margin-left:5px;font-weight:400}

/* ---------- result meta ---------- */
.rmeta{display:flex;align-items:baseline;gap:8px;margin:2px 0 12px;
  font:11px var(--mono);color:var(--faint);text-transform:uppercase;letter-spacing:.06em}
.rmeta em{font-style:normal;color:var(--dim)}
.daybar{position:sticky;top:0;z-index:5;background:var(--bg);padding:10px 0 6px;
  font:11px var(--mono);color:var(--faint);text-transform:uppercase;letter-spacing:.08em;
  border-bottom:1px solid var(--line-soft);margin-bottom:8px}

/* ---------- memory / jot cards ----------
   Cards, not rows: ranking is still visible (the accent top-edge + score badge carry what the
   rail used to), but a jot no longer gets to sprawl into a wall of text - headline and preview
   both clamp to two lines. The top-edge color is the tag-derived "category" - see catColorOf(). */
mark{background:var(--mark);color:inherit;border-radius:2px;padding:0 1px}
.tag{background:var(--sunk);color:var(--dim);border-radius:4px;padding:1px 6px;font-size:11px}
.tag.res{background:transparent;border:1px dashed var(--line);color:var(--faint)}

.cardgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:11px}
.mcard{position:relative;background:var(--panel);border:1px solid var(--line);border-radius:var(--r);
  border-top:3px solid var(--cat,var(--accent));padding:11px 12px 10px;cursor:pointer;
  display:flex;flex-direction:column;gap:6px;min-height:128px}
.mcard:hover{border-color:var(--dim)}
.mcard.sel{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-wash)}
.cathead{display:flex;align-items:center;gap:7px}
.catdot{width:6px;height:6px;border-radius:50%;background:var(--cat,var(--accent));flex:none}
.catlabel{font:10px var(--mono);text-transform:uppercase;letter-spacing:.06em;color:var(--dim)}
.cathead .when{margin-left:auto;font:10.5px var(--mono);color:var(--faint)}
.slug{font:12px var(--mono);color:var(--accent-ink);font-weight:600;letter-spacing:-.01em}
.headline{color:var(--ink);font-size:13.5px;font-weight:500;line-height:1.4;
  display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
.preview{color:var(--dim);font-size:12.5px;line-height:1.5;
  display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
.mfoot{margin-top:auto;display:flex;align-items:center;gap:6px;flex-wrap:wrap;
  font:10.5px var(--mono);color:var(--faint)}
.scbadge{font:10.5px var(--mono);color:var(--accent-ink);font-weight:600}

/* ---------- dashboard: reminders banner + section heads ---------- */
.note.reminders{display:flex;align-items:center;gap:13px;padding:12px 14px}
.note.reminders .ic{width:30px;height:30px;border-radius:8px;background:var(--sunk);
  display:flex;align-items:center;justify-content:center;flex:none}
.note.reminders .ic svg{width:15px;height:15px;color:var(--faint)}
.note.reminders b{display:block;font-size:13px}
.note.reminders .soon{margin-left:auto;font:10px var(--mono);text-transform:uppercase;
  letter-spacing:.07em;color:var(--accent-ink);background:var(--accent-wash);padding:3px 8px;
  border-radius:20px;flex:none}
.colhead{display:flex;align-items:baseline;gap:8px;margin:22px 0 11px}
.colhead:first-child{margin-top:0}
.colhead h2{margin:0;font-size:13px;font-weight:600;color:var(--ink)}
.colhead span{font-size:11.5px;color:var(--faint)}

/* ---------- notes / empty ---------- */
.empty{text-align:center;padding:44px 20px;color:var(--faint)}
.empty b{display:block;color:var(--dim);font-size:14px;font-weight:500;margin-bottom:5px}
.note{border:1px solid var(--line);border-left:3px solid var(--dim);background:var(--panel);
  padding:9px 12px;border-radius:6px;margin-bottom:10px;font-size:13px;color:var(--body)}
.note.warn{border-left-color:var(--warn);background:var(--warn-wash);border-color:var(--warn-line);
  color:var(--warn)}
.note.good{border-left-color:var(--good);background:var(--good-wash);border-color:var(--good-line);
  color:var(--good)}
.note.bad{border-left-color:var(--bad);background:var(--bad-wash);border-color:var(--bad-line);
  color:var(--bad)}

/* ---------- tags view ---------- */
.sect{font:11px var(--mono);color:var(--faint);text-transform:uppercase;letter-spacing:.08em;
  margin:22px 0 9px}
.sect:first-child{margin-top:2px}
.cluster{border:1px solid var(--warn-line);background:var(--warn-wash);border-radius:var(--r);
  padding:12px 13px;margin-bottom:8px}
.cluster .lead{font-size:12.5px;color:var(--warn);margin-bottom:9px}
.vrow{display:flex;align-items:center;gap:10px;padding:6px 9px;border-radius:5px;
  cursor:pointer;position:relative;overflow:hidden}
.vrow:hover{background:var(--sunk)}
.vrow .fill{position:absolute;left:0;top:0;bottom:0;background:var(--accent-wash);z-index:0}
.vrow > *{position:relative;z-index:1}
.vrow .n{font:11px var(--mono);color:var(--dim);margin-left:auto}
.vrow .when{font:10px var(--mono);color:var(--faint);width:70px;text-align:right}

/* ---------- detail ---------- */
.dhead{display:flex;align-items:flex-start;gap:10px;margin-bottom:4px}
.dhead h3{margin:0;font-size:15px;color:var(--ink);font-weight:600}
.dmeta{font:11px var(--mono);color:var(--faint);line-height:1.7;margin-bottom:16px;
  padding-bottom:14px;border-bottom:1px solid var(--line-soft)}
.dmeta div{display:flex;gap:8px}
.dmeta i{font-style:normal;color:var(--dim);width:62px;flex:none}
label{display:block;font:10px var(--mono);text-transform:uppercase;letter-spacing:.08em;
  color:var(--faint);margin:14px 0 5px}
label u{text-decoration:none;color:var(--accent-ink);text-transform:none;letter-spacing:0;
  font-family:var(--sans);font-size:11px;margin-left:6px}
.actions{display:flex;gap:8px;margin-top:20px;padding-top:14px;
  border-top:1px solid var(--line-soft);flex-wrap:wrap}

/* ---------- toast ---------- */
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(8px);z-index:60;
  padding:10px 16px;border-radius:8px;font-size:13px;max-width:min(520px,90vw);
  box-shadow:0 6px 24px rgba(0,0,0,.16);opacity:0;pointer-events:none;
  transition:opacity .16s,transform .16s}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
#toast.ok{background:var(--good-wash);color:var(--good);border:1px solid var(--good-line)}
#toast.err{background:var(--bad-wash);color:var(--bad);border:1px solid var(--bad-line)}
#toast.warn{background:var(--warn-wash);color:var(--warn);border:1px solid var(--warn-line)}
</style>
</head>
<body>

<header>
  <div class="brand">
    <svg class="mk" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3">
      <path d="M1 4h14M1 8h14M1 12h14" opacity=".4"/>
      <path d="M4 1v14M8 1v14M12 1v14"/>
    </svg>
    Loom
  </div>
  <div class="stats" id="stats"></div>
</header>

<nav id="nav"></nav>

<main>
  <div class="pane" id="list"></div>
  <div class="pane" id="detail"></div>
</main>

<div id="toast"></div>

<script>
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;
                   if(x!==undefined)e.textContent=x;return e;};
const escHtml=s=>(s??'').toString().replace(/[&<>"]/g,c=>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

/* sortOrder/sinceWhen/memOnly live here, not inside viewSearch(), for the same reason activeTags
   and lastQ do: viewSearch() is torn down and rebuilt on every render() - including the render()
   that just opening a result triggers - so filter state kept local to it would silently reset the
   instant a jot is clicked. */
let view='dashboard',sel=null,activeTags=new Set(),allTags=[],lastQ='',stats={},
    sortOrder='',sinceWhen='',memOnly=false;

function toast(msg,kind){const t=$('#toast');t.textContent=msg;t.className='show '+(kind||'ok');
  clearTimeout(t._t);t._t=setTimeout(()=>t.className='',3200);}

async function api(path,opts){
  const r=await fetch(path,opts);const text=await r.text();
  let body=null;try{body=text?JSON.parse(text):null;}catch(e){body={error:text};}
  if(!r.ok)throw Object.assign(new Error(body&&body.error?body.error:r.statusText),
                               {status:r.status,body});
  return body;
}

/* Ids are UTC microseconds; the browser renders them in the reader's own zone, which is the zone
   these notes were written in. */
const D=us=>new Date(us/1000);
function ago(us){
  const s=(Date.now()-us/1000)/1000;
  if(s<45)return'just now';
  if(s<3600)return Math.round(s/60)+'m ago';
  if(s<86400)return Math.round(s/3600)+'h ago';
  if(s<86400*7)return Math.round(s/86400)+'d ago';
  const d=D(us);
  return d.toLocaleDateString(undefined,{month:'short',day:'numeric',
    year:d.getFullYear()===new Date().getFullYear()?undefined:'numeric'});
}
const dayKey=us=>D(us).toDateString();
function dayLabel(us){
  const d=D(us),t=new Date();
  if(d.toDateString()===t.toDateString())return'Today';
  const y=new Date(t.getTime()-864e5);
  if(d.toDateString()===y.toDateString())return'Yesterday';
  return d.toLocaleDateString(undefined,{weekday:'long',month:'long',day:'numeric',
    year:d.getFullYear()===t.getFullYear()?undefined:'numeric'});
}
const stamp=us=>D(us).toLocaleString(undefined,
  {year:'numeric',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'});

/* ---------- header ---------- */
async function refreshStats(){
  const S=$('#stats');
  try{
    stats=await api('/stats');const p=stats.persistence||{};
    S.innerHTML='';
    const add=(v,k)=>{const w=el('div','stat');w.append(el('b',null,v));
                      w.append(el('span',null,k));S.append(w);};
    add(stats.jots,'jots');add(stats.named,'memories');add(stats.tags,'tags');
    if(p.enabled)add((p.wal_bytes/1024).toFixed(0)+'k','wal');
    const L=el('div','live');
    const d=el('i','dot'+(p.enabled?'':' off'));
    L.append(d);L.append(el('span',null,p.enabled?'persistent':'RAM only'));
    S.append(L);
    drawNav();
  }catch(e){
    S.innerHTML='';
    const L=el('div','live');L.append(el('i','dot off'));
    L.append(el('span',null,'unreachable'));S.append(L);
  }
}

const VIEWS=[['dashboard','Dashboard'],['search','Search'],['tags','Tags'],['health','Health']];
let navTabEls=[];
function drawNav(){
  const N=$('#nav');N.innerHTML='';
  navTabEls=[];
  VIEWS.forEach(function(v){
    const b=el('button',v[0]===view?'on':'');
    b.append(document.createTextNode(v[1]));
    if(v[0]==='tags'&&stats.tags!==undefined)b.append(el('span','pill',stats.tags));
    if(v[0]==='dashboard'&&stats.jots!==undefined)b.append(el('span','pill',stats.jots));
    b.onclick=function(){view=v[0];drawNav();render();};
    b.dataset.view=v[0];
    navTabEls.push(b);
    N.append(b);
  });

  /* search is pinned here, not just inside the Search tab, so it is reachable from anywhere -
     typing here switches to Search and runs the query; drawNav() itself is never called from its
     own oninput, or the input (and the user's cursor position in it) would be destroyed mid-type */
  const ns=el('div','navsearch');
  ns.innerHTML='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">'+
    '<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5L14 14"/></svg>';
  const ni=el('input');ni.id='navsearch-input';ni.placeholder='Search everything…';ni.value=lastQ;
  ns.append(ni);ns.append(el('span','kbd','/'));
  let nt;
  ni.oninput=function(){
    clearTimeout(nt);
    nt=setTimeout(function(){
      lastQ=ni.value;
      view='search';
      navTabEls.forEach(b=>b.classList.toggle('on',b.dataset.view==='search'));
      render();
    },160);
  };
  N.append(ns);
}

/* ---------- cards ---------- */
function highlight(text,terms){
  const safe=escHtml(text);
  if(!terms.length)return safe;
  const esc=terms.map(t=>t.replace(/[.*+?^${}()|[\]\\]/g,'\\$&'));
  return safe.replace(new RegExp('('+esc.join('|')+')','gi'),'<mark>$1</mark>');
}

/* Category = the first non-structural tag (":"-tags are structure, not vocabulary - same rule
   the server's drift/vocabulary logic uses). No dedicated field for it; deterministically hashed
   onto a small fixed set of colors already in the palette so the same tag always reads the same
   way, without inventing a new one-color-per-tag system. */
const CAT_VARS=['--accent','--warn','--good','--dim'];
function catColorOf(tags){
  const t=(tags||[]).find(x=>x.indexOf(':')<0);
  if(!t)return{cssVar:'--dim',name:'untagged'};
  let h=0;for(let i=0;i<t.length;i++)h=(h*31+t.charCodeAt(i))>>>0;
  return{cssVar:CAT_VARS[h%CAT_VARS.length],name:t};
}

function jotCard(j,maxScore,terms){
  const isMem=!!j.name;
  const cat=catColorOf(j.tags);
  const c=el('div','mcard'+(sel&&sel.id===j.id?' sel':''));
  c.style.setProperty('--cat','var('+cat.cssVar+')');

  const head=el('div','cathead');
  head.append(el('i','catdot'));
  head.append(el('span','catlabel',cat.name));
  const when=el('span','when',ago(j.id));when.title=stamp(j.id);head.append(when);
  c.append(head);

  if(isMem)c.append(el('div','slug',j.name));
  /* headline vs. preview is keyed on "does a summary exist", not on isMem: a terse jot may be
     summary-only (weighted highest in search on purpose - see Jot.h), with nothing in text at
     all, named or not. Rendering that as headline+empty-second-line was the bug this replaced. */
  if(j.summary){
    const s=el('div','headline');s.innerHTML=highlight(j.summary,terms);c.append(s);
    if(j.text){const t=el('div','preview');t.innerHTML=highlight(j.text,terms);c.append(t);}
  }else{
    const t=el('div','headline');t.innerHTML=highlight(j.text||'',terms);c.append(t);
  }

  const f=el('div','mfoot');
  (j.tags||[]).slice(0,3).forEach(x=>f.append(el('span','tag'+(x.indexOf(':')>=0?' res':''),x)));
  const trail=el('span');trail.style.cssText='margin-left:auto;display:flex;gap:6px;align-items:center';
  if(j.editor)trail.append(el('span',null,'@'+j.editor));
  if(j.score!==undefined)trail.append(el('span','scbadge',j.score.toFixed(1)));
  f.append(trail);
  c.append(f);

  c.onclick=function(){sel=j;document.body.classList.add('editing');render();};
  return c;
}

const queryTerms=q=>(q||'').toLowerCase().split(/[^a-z0-9']+/i).filter(t=>t.length>1);

/* ---------- search ----------
   Filters (sort, time range, tags, memories-only) all live above the results, and combine - this
   is the "search is a first-class citizen" pass: previously it was a text box plus a tag-chip row
   with no time filter and no way to narrow to memories. Sort/tag/time all map straight onto
   Query.h's existing order/tag/since params; only "memories only" has no server-side filter (no
   boolean for "has a name"), so it's applied client-side after the fetch. */
async function viewSearch(){
  const L=$('#list');

  const box=el('div','search');
  box.innerHTML='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">'+
    '<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5L14 14"/></svg>';
  const q=el('input');q.placeholder='Search memories and jots…';q.value=lastQ;
  box.append(q);box.append(el('span','kbd','/'));
  L.append(box);

  const bar=el('div','filterbar');

  const sortG=el('div','fgroup');sortG.append(el('span','flabel','Sort'));
  const order=el('select');order.style.width='auto';
  [['','Best match'],['newest','Newest first'],['oldest','Oldest first']]
    .forEach(function(o){const x=el('option',null,o[1]);x.value=o[0];order.append(x);});
  order.value=sortOrder;
  sortG.append(order);bar.append(sortG);

  const whenG=el('div','fgroup');whenG.append(el('span','flabel','When'));
  const when=el('select');when.style.width='auto';
  [['','All time'],['1d','Today'],['7d','This week'],['30d','This month']]
    .forEach(function(o){const x=el('option',null,o[1]);x.value=o[0];when.append(x);});
  when.value=sinceWhen;
  whenG.append(when);bar.append(whenG);

  const memWrap=el('div','togglewrap');
  memWrap.append(el('span',null,'Memories only'));
  const memToggle=el('div','toggle'+(memOnly?' on':''));memToggle.append(el('i'));
  memToggle.onclick=function(){memOnly=!memOnly;memToggle.classList.toggle('on',memOnly);run();};
  memWrap.append(memToggle);bar.append(memWrap);

  const nw=el('button','btn tiny primary','New jot');
  nw.onclick=function(){sel={__new:true};document.body.classList.add('editing');render();};
  bar.append(nw);
  L.append(bar);

  if(allTags.length){
    const chips=el('div','chips');
    allTags.slice(0,18).forEach(function(t){
      const c=el('span','chip'+(activeTags.has(t.tag)?' on':''));
      c.append(document.createTextNode(t.tag));c.append(el('b',null,t.count));
      c.onclick=function(){
        activeTags.has(t.tag)?activeTags.delete(t.tag):activeTags.add(t.tag);
        run();
      };
      chips.append(c);
    });
    L.append(chips);
  }

  const meta=el('div','rmeta');L.append(meta);
  const grid=el('div','cardgrid');L.append(grid);

  async function run(){
    lastQ=q.value;
    const p=new URLSearchParams();
    if(q.value){p.set('q',q.value);p.set('prefix','1');}
    activeTags.forEach(t=>p.append('tag',t));
    if(order.value)p.set('order',order.value);
    if(when.value)p.set('since',when.value);
    p.set('limit','60');
    try{
      const r=await api('/jots?'+p);
      const jots=memOnly?r.jots.filter(j=>j.name):r.jots;

      meta.innerHTML='';
      meta.append(el('em',null,jots.length+(jots.length===1?' match':' matches')));
      if(r.truncated)meta.append(el('span',null,'showing '+r.returned));
      if(activeTags.size)meta.append(el('span',null,'tag-filtered'));
      if(memOnly)meta.append(el('span',null,'memories only'));

      grid.innerHTML='';
      if(!jots.length){
        const e=el('div','empty');
        e.append(el('b',null,'Nothing matched'));
        e.append(el('div',null,q.value?'Try fewer words — summaries are weighted highest.'
                                      :'Clear the filters, or write something.'));
        grid.append(e);return;
      }
      let max=0;jots.forEach(j=>{if((j.score||0)>max)max=j.score||0;});
      const terms=queryTerms(q.value);
      jots.forEach(j=>grid.append(jotCard(j,max,terms)));
    }catch(e){grid.innerHTML='';grid.append(el('div','note bad',e.message));}
  }

  let t;q.oninput=function(){clearTimeout(t);t=setTimeout(run,130);};
  order.onchange=function(){sortOrder=order.value;run();};
  when.onchange=function(){sinceWhen=when.value;run();};
  await run();
  /* don't steal focus from the nav search box mid-keystroke - it's what re-renders this view
     on every debounced input when a search started there rather than in this panel */
  if(!sel&&document.activeElement!==$('#navsearch-input')){
    q.focus();q.setSelectionRange(q.value.length,q.value.length);
  }
}

/* ---------- dashboard: at-a-glance home view ---------- */
async function viewDashboard(){
  const L=$('#list');

  const rem=el('div','note reminders');
  const ic=el('div','ic');
  ic.innerHTML='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">'+
    '<path d="M12 5v6l4 2"/><circle cx="12" cy="13" r="8"/><path d="M9 2h6"/></svg>';
  rem.append(ic);
  const copy=el('div');
  copy.append(el('b',null,'No reminders yet'));
  copy.append(el('div',null,'Time-based nudges will surface here once reminder scheduling ships.'));
  rem.append(copy);
  rem.append(el('span','soon','Coming soon'));
  L.append(rem);

  const rHead=el('div','colhead');
  rHead.append(el('h2',null,'Recent'));
  rHead.append(el('span',null,'newest first'));
  L.append(rHead);
  const grid=el('div','cardgrid');L.append(grid);
  async function run(){
    try{
      const r=await api('/jots?order=newest&limit=30');
      grid.innerHTML='';
      if(!r.jots.length){
        const e=el('div','empty');
        e.append(el('b',null,'No jots yet'));
        e.append(el('div',null,'Write one from Search, or import a jots.log.'));
        grid.append(e);
      }else{
        r.jots.forEach(j=>grid.append(jotCard(j,0,[])));
      }
    }catch(e){grid.innerHTML='';grid.append(el('div','note bad',e.message));}
  }
  await run();
  clearInterval(window.__rt);
  window.__rt=setInterval(function(){if(view==='dashboard'&&!sel)run();},5000);

  const uHead=el('div','colhead');
  uHead.append(el('h2',null,'Upcoming'));
  uHead.append(el('span',null,'due soon'));
  L.append(uHead);
  const uEmpty=el('div','empty');
  uEmpty.append(el('b',null,'Nothing scheduled'));
  uEmpty.append(el('div',null,"Due-date tracking isn't a feature yet - this is reserving its place."));
  L.append(uEmpty);
}

/* ---------- tags ---------- */
async function viewTags(){
  const L=$('#list');
  try{
    const both=await Promise.all([api('/tags?reserved=1'),api('/tags/similar')]);
    const tags=both[0],sim=both[1];

    L.append(el('div','sect','Drift — tags that look like variants of each other'));
    if(!sim.clusters.length){
      L.append(el('div','note good','No drift detected. The vocabulary is coherent.'));
    }else{
      sim.clusters.forEach(function(members){
        const c=el('div','cluster');
        c.append(el('div','lead',
          'These '+members.length+' tags are probably the same idea written differently.'));
        const r=el('div','row');
        members.forEach(m=>r.append(el('span','tag',m)));
        c.append(r);
        const act=el('div','row');act.style.marginTop='10px';
        const keep=el('select');keep.style.width='auto';
        members.forEach(function(m){const o=el('option',null,'keep “'+m+'”');
                                    o.value=m;keep.append(o);});
        const b=el('button','btn tiny primary','Merge');
        b.onclick=async function(){
          const to=keep.value,from=members.filter(m=>m!==to);
          if(!confirm('Merge '+from.join(', ')+' into “'+to+'”?\n\n'+
                      'This rewrites every jot carrying them. It cannot be undone.'))return;
          try{
            const res=await api('/tags/merge',{method:'POST',
              headers:{'Content-Type':'application/json'},
              body:JSON.stringify({from:from,to:to})});
            toast('Merged — '+res.changed+' jots rewritten');
            allTags=(await api('/tags')).tags;await refreshStats();render();
          }catch(e){toast(e.message,'err');}
        };
        act.append(keep,b);c.append(act);L.append(c);
      });
    }

    L.append(el('div','sect','Vocabulary — '+tags.count+' tags'));
    if(!tags.tags.length){
      L.append(el('div','empty','Nothing tagged yet.'));
    }else{
      let max=1;tags.tags.forEach(t=>{if(t.count>max)max=t.count;});
      tags.tags.forEach(function(s){
        const row=el('div','vrow');
        const fill=el('i','fill');fill.style.width=(100*s.count/max)+'%';row.append(fill);
        row.append(el('span','tag'+(s.reserved?' res':''),s.tag));
        row.append(el('span','n',s.count));
        const w=el('span','when',ago(s.last));w.title='last used '+stamp(s.last);row.append(w);
        row.onclick=function(){view='search';activeTags=new Set([s.tag]);drawNav();render();};
        L.append(row);
      });
    }
  }catch(e){L.append(el('div','note bad',e.message));}
}

/* ---------- health ---------- */
async function viewHealth(){
  const L=$('#list');
  try{
    const s=await api('/stats');const p=s.persistence||{};
    const group=function(title,rows){
      L.append(el('div','sect',title));
      rows.forEach(function(kv){
        const r=el('div','vrow');r.style.cursor='default';
        r.append(el('span',null,kv[0]));r.append(el('span','n',kv[1]));L.append(r);
      });
    };
    group('Store',[['Jots',s.jots],['Named memories',s.named],['Tag vocabulary',s.tags],
      ['Distinct terms',s.terms],['Pending links',s.pending_links],['Editors',s.editors],
      ['Mutations this run',s.mutations]]);
    if(s.oldest)group('Span',[['Oldest',stamp(s.oldest)],['Newest',stamp(s.newest)]]);

    if(!p.enabled){
      L.append(el('div','sect','Durability'));
      L.append(el('div','note bad',
        'Running in RAM only — everything is lost on restart. Start without --no-persist.'));
    }else{
      group('Durability',[['WAL bytes',p.wal_bytes],['Lines appended',p.appended],
        ['Fsyncs',p.synced],['Queue depth',p.queued],['Snapshots',p.snapshots]]);
      const row=el('div','row');row.style.marginTop='16px';
      const snap=el('button','btn tiny','Snapshot now');
      snap.onclick=async function(){try{const r=await api('/admin/snapshot',{method:'POST'});
        toast('Snapshot wrote '+r.records+' records');render();}
        catch(e){toast(e.message,'err');}};
      const fl=el('button','btn tiny','Flush WAL');
      fl.onclick=async function(){try{await api('/admin/flush',{method:'POST'});toast('Flushed');}
        catch(e){toast(e.message,'err');}};
      row.append(snap,fl);L.append(row);
    }

    L.append(el('div','sect','Endpoints'));
    L.append(el('div','note','REST at /jots · MCP at /mcp — connect an agent with: '+
      'claude mcp add --transport http loom '+location.origin+'/mcp'));
  }catch(e){L.append(el('div','note bad',e.message));}
}

/* ---------- detail / editor ---------- */
function renderDetail(){
  const P=$('#detail');P.innerHTML='';
  if(!sel){
    const e=el('div','empty');
    e.append(el('b',null,'Nothing selected'));
    e.append(el('div',null,'Pick a jot to read or edit it.'));
    P.append(e);return;
  }
  const isNew=!!sel.__new;

  const h=el('div','dhead');
  h.append(el('h3',null,isNew?'New jot':(sel.name||'Edit jot')));
  P.append(h);

  if(!isNew){
    const m=el('div','dmeta');
    const line=function(k,v){const d=el('div');d.append(el('i',null,k));
      d.append(el('span',null,v));m.append(d);};
    line('id',String(sel.id));
    line('created',stamp(sel.id));
    if(sel.updated)line('edited',stamp(sel.updated));
    line('editor',sel.editor||'user');
    P.append(m);
  }

  const f={};
  const field=function(key,label,tag,hint){
    const l=el('label');l.append(document.createTextNode(label));
    if(hint)l.append(el('u',null,hint));
    P.append(l);
    const e=el(tag||'input');e.value=(sel[key]===undefined||sel[key]===null)?'':sel[key];
    if(tag==='textarea')e.rows=(key==='text')?9:2;
    P.append(e);f[key]=e;return e;
  };
  field('name','slug',null,'durable memories only');
  field('summary','summary','textarea','weighted highest in search');
  field('text','text','textarea');
  const tg=field('tags','tags',null,'comma separated');
  tg.value=(sel.tags||[]).join(', ');
  const lk=field('links','links',null,'ids or slugs');
  lk.value=(sel.links||[]).concat(sel.pending||[]).join(', ');
  field('editor','editor');

  if(!isNew&&(sel.pending||[]).length)
    P.append(el('div','note','Unresolved: '+sel.pending.join(', ')+
      ' — these connect themselves when a jot takes that slug.'));

  const act=el('div','actions');
  const save=el('button','btn primary',isNew?'Create':'Save');
  save.onclick=async function(){
    const body={text:f.text.value,name:f.name.value,summary:f.summary.value,
      editor:f.editor.value,
      tags:tg.value.split(',').map(s=>s.trim()).filter(Boolean),
      links:lk.value.split(',').map(s=>s.trim()).filter(Boolean)};
    try{
      let r;
      if(isNew){
        r=await api('/jots',{method:'POST',headers:{'Content-Type':'application/json'},
                             body:JSON.stringify(body)});
      }else{
        /* expect_updated is the multi-agent guard: if anything changed since this panel
           loaded, the server answers 409 instead of losing the other write. */
        const exp=sel.updated||sel.id;
        r=await api('/jots/'+sel.id+'?expect_updated='+exp,
          {method:'PATCH',headers:{'Content-Type':'application/json'},
           body:JSON.stringify(body)});
      }
      sel=r;
      if(r.warnings&&r.warnings.length)toast(r.warnings[0],'warn');
      else toast(isNew?'Created':'Saved');
      allTags=(await api('/tags')).tags;await refreshStats();render();
    }catch(e){
      if(e.status===409)
        toast('Conflict — someone else changed this jot. Close and reopen before saving.','err');
      else toast(e.message,'err');
    }
  };
  act.append(save);

  if(!isNew){
    const rel=el('button','btn tiny','Linked');
    rel.onclick=async function(){
      try{
        const r=await api('/jots/'+sel.id+'/links?depth=2');
        P.append(el('div','sect','Linked — 2 hops, both directions'));
        if(!r.jots.length)P.append(el('div','empty','Nothing links here.'));
        r.jots.forEach(j=>P.append(jotCard(j,0,[])));
      }catch(e){toast(e.message,'err');}
    };
    const del=el('button','btn tiny danger','Delete');
    del.onclick=async function(){
      if(!confirm('Delete this jot permanently? There is no undo.'))return;
      try{await api('/jots/'+sel.id,{method:'DELETE'});toast('Deleted');
        sel=null;document.body.classList.remove('editing');
        await refreshStats();render();}
      catch(e){toast(e.message,'err');}
    };
    act.append(rel,del);
  }
  const close=el('button','btn tiny ghost','Close');
  close.onclick=function(){sel=null;document.body.classList.remove('editing');render();};
  act.append(close);
  P.append(act);

  P.onkeydown=function(e){
    if((e.metaKey||e.ctrlKey)&&e.key==='Enter'){e.preventDefault();save.click();}
  };
}

/* ---------- shell ---------- */
async function render(){
  $('#list').innerHTML='';
  if(view==='dashboard')await viewDashboard();
  else if(view==='search')await viewSearch();
  else if(view==='tags')await viewTags();
  else await viewHealth();
  renderDetail();
}

document.addEventListener('keydown',function(e){
  const typing=/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName);
  if(e.key==='/'&&!typing){e.preventDefault();$('#navsearch-input').focus();}
  if(e.key==='Escape'){
    if(sel){sel=null;document.body.classList.remove('editing');render();}
    else if(typing)document.activeElement.blur();
  }
});

(async function(){
  drawNav();
  try{allTags=(await api('/tags')).tags;}catch(e){}
  await refreshStats();
  await render();
  setInterval(refreshStats,4000);
})();
</script>
</body>
</html>
)HTML";
}
