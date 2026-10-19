#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

//////////////////////////////////////////////////////////////////////////////////////////////////
// MobilePage - a phone-sized front end, embedded in the binary and served at "/m".
//
// SEPARATE FROM THE DASHBOARD rather than a responsive mode of it. The dashboard is an operato
// console - tag hygiene, history, activity - and a phone wants three things: find a jot, read it,
// write one. Shrinking the console would ship 340KB of operator UI to do that.
//
// ADD TO HOME SCREEN turns it into a standalone app: that is what the manifest and the apple-*
// metas are for. iOS grants standalone mode only to a page served over HTTPS, so open it through a
// TLS front end rather than the bare port.
//
// Same REST routes and same origin as the dashboard, so no CORS. A bearer token, when the serve
// has one, is entered once on the page and kept in this browser's localStorage.
//
// Palette follows the system setting: Twilight in light mode, Twilight Dark in dark mode - the two
// dashboard palettes, with the same token names, so a colour change there is easy to carry here.
//////////////////////////////////////////////////////////////////////////////////////////////////

inline const char* LoomMobileHtml()
{
    return
R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="default">
<meta name="apple-mobile-web-app-title" content="Loom">
<meta name="theme-color" content="#efedfc" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#13111f" media="(prefers-color-scheme: dark)">
<title>Loom</title>
<link rel="manifest" href="/m/manifest.webmanifest">
<link rel="icon" type="image/png" href="/icon.png">
<link rel="apple-touch-icon" href="/icon-full.png">
<style>
:root{
  --bg:#efedfc; --panel:#ffffff; --sunk:#e7e4fa;
  --ink:#141030; --body:#2d2856; --dim:#534d7e; --faint:#6c6594;
  --line:#d8d4ef; --line-soft:#e5e2f6;
  --accent:#5040dc; --accent2:#7d4ae8; --accent-wash:#e9e7fe;
  --good:#0b7a63; --good2:#177a4a; --bad:#c42c46;
  --warn:#9c6208; --warn-wash:#fdf4e4;
  --hi-a:#5b53ea; --hi-b:#0f9d8e;
  --tag-bg:#e9e6fd; --tag-ink:#4236aa;
  --field-bg:#ffffff; --field-ink:#141030;
  --mark:#ffe9a3;
  --sh:28 26 34; --sh-k:1;
  --sans:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",sans-serif;
  --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
  --r:12px;
  color-scheme:light;
}
@media (prefers-color-scheme: dark){
  :root{
    --bg:#13111f; --panel:#1c1930; --sunk:#171429;
    --ink:#ede8f5; --body:#c4beda; --dim:#8f88ac; --faint:#7d76a0;
    --line:#3e2f63; --line-soft:#332852;
    --accent:#b39ddb; --accent2:#d2bdf0; --accent-wash:#332852;
    --good:#8fd9a8; --good2:#b0ecc4; --bad:#e0697a;
    --warn:#e8b968; --warn-wash:#3d3018;
    --hi-a:#7b5be6; --hi-b:#1aa594;
    --tag-bg:#2a2348; --tag-ink:#cbb8e8;
    --field-bg:#e0d9f0; --field-ink:#1b1730;
    --mark:#4a3c1c;
    --sh:5 3 14; --sh-k:3.2;
    color-scheme:dark;
  }
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;background:var(--bg);color:var(--body);font:16px/1.45 var(--sans)}
body{padding:0 0 calc(96px + env(safe-area-inset-bottom))}
button{font:inherit;color:inherit;border:0;background:none;cursor:pointer}

/* The bar stays put while the list scrolls under it; the gradient rule across its foot is the
   one place the page spends the palette's highlight ramp. */
header{position:sticky;top:0;z-index:5;background:var(--bg);
  padding:calc(10px + env(safe-area-inset-top)) 16px 10px}
header::after{content:"";position:absolute;left:0;right:0;bottom:0;height:3px;
  background:linear-gradient(90deg,var(--hi-a),var(--hi-b),var(--accent2))}
.brand{display:flex;align-items:center;gap:8px;margin-bottom:10px}
.brand img{width:24px;height:24px;border-radius:6px}
.brand b{font-size:19px;color:var(--ink);letter-spacing:-.01em}
.brand .n{margin-left:auto;font:12px var(--mono);color:var(--faint)}
.brand .gear{margin-left:8px;padding:6px;color:var(--dim);font-size:18px;line-height:1}
#q{width:100%;padding:11px 14px;border-radius:var(--r);border:1px solid var(--line);
  background:var(--field-bg);color:var(--field-ink);font:inherit;font-size:16px;
  box-shadow:inset 2px 3px 5px rgb(var(--sh) / calc(.10*var(--sh-k)))}
#q:focus{outline:2px solid var(--accent);outline-offset:1px}
.tabs{display:flex;gap:8px;margin-top:10px}
.tabs button{flex:1;padding:8px 0;border-radius:999px;font-size:14px;font-weight:600;
  color:var(--dim);background:var(--sunk);border:1px solid var(--line-soft)}
.tabs button.on{color:#fff;border-color:transparent;
  background:linear-gradient(110deg,var(--accent),var(--accent2))}
@media (prefers-color-scheme: dark){.tabs button.on{color:#15121f}}

main{padding:12px 16px}
.day{font:11px var(--mono);text-transform:uppercase;letter-spacing:.07em;color:var(--faint);
  margin:14px 2px 6px}
.card{display:block;width:100%;text-align:left;background:var(--panel);border-radius:var(--r);
  border:1px solid var(--line-soft);padding:12px 14px;margin-bottom:8px;
  box-shadow:1px 2px 3px rgb(var(--sh) / calc(.14*var(--sh-k))),3px 5px 12px -2px rgb(var(--sh) / calc(.10*var(--sh-k)))}
.card:active{transform:scale(.99)}
.card .t{color:var(--ink);font-weight:600;overflow-wrap:anywhere}
.card .s{color:var(--dim);font-size:14px;margin-top:2px;overflow:hidden;display:-webkit-box;
  -webkit-line-clamp:2;-webkit-box-orient:vertical;overflow-wrap:anywhere}
.card.todo{border-left:4px solid var(--warn)}
.foot{display:flex;flex-wrap:wrap;gap:5px;align-items:center;margin-top:8px}
.tag{font:11.5px var(--mono);padding:2px 7px;border-radius:999px;background:var(--tag-bg);color:var(--tag-ink)}
.when{font:11.5px var(--mono);color:var(--faint);margin-left:auto}
.empty{color:var(--faint);text-align:center;padding:40px 0}
mark{background:var(--mark);color:inherit;border-radius:2px}
.more{display:block;margin:6px auto 0;padding:10px 18px;color:var(--accent);font-weight:600}

/* Floating add button, clear of the home indicator. */
#fab{position:fixed;right:20px;bottom:calc(20px + env(safe-area-inset-bottom));z-index:6;
  width:58px;height:58px;border-radius:50%;color:#fff;font-size:30px;line-height:58px;
  background:linear-gradient(135deg,var(--accent),var(--hi-b));
  box-shadow:3px 5px 6px rgb(var(--sh) / calc(.30*var(--sh-k))),9px 16px 32px -5px rgb(var(--sh) / calc(.25*var(--sh-k)))}

/* Full-screen sheets for reading and writing. A sheet rather than a route: the list behind it
   keeps its scroll position and nothing has to be re-fetched on the way back. */
.sheet{position:fixed;inset:0;z-index:10;background:var(--bg);display:none;flex-direction:column;
  padding-top:env(safe-area-inset-top)}
.sheet.show{display:flex}
.sbar{display:flex;align-items:center;gap:8px;padding:10px 12px;border-bottom:1px solid var(--line)}
.sbar .ttl{flex:1;font-weight:600;color:var(--ink);text-align:center}
.sbar button{padding:8px 10px;color:var(--accent);font-weight:600}
.sbody{flex:1;overflow:auto;padding:16px 16px calc(24px + env(safe-area-inset-bottom))}
#vSummary{font-size:20px;font-weight:650;color:var(--ink);line-height:1.3;overflow-wrap:anywhere}
#vName{font:12.5px var(--mono);color:var(--faint);margin-top:4px}
#vText{white-space:pre-wrap;overflow-wrap:anywhere;margin-top:14px;color:var(--body)}
#vTags{margin-top:14px}
#vMeta{font:12px var(--mono);color:var(--faint);margin-top:14px}
.acts{display:flex;gap:10px;margin-top:22px}
.btn{flex:1;padding:12px 0;border-radius:var(--r);font-weight:650;color:#fff;
  background:linear-gradient(110deg,var(--accent),var(--accent2))}
.btn.good{background:linear-gradient(110deg,var(--good),var(--good2))}
.btn.ghost{background:var(--sunk);color:var(--bad);border:1px solid var(--line-soft)}
@media (prefers-color-scheme: dark){.btn{color:#15121f}.btn.ghost{color:var(--bad)}}
label{display:block;font:11px var(--mono);text-transform:uppercase;letter-spacing:.07em;
  color:var(--faint);margin:14px 2px 6px}
label:first-child{margin-top:0}
.f{width:100%;padding:11px 12px;border-radius:var(--r);border:1px solid var(--line);
  background:var(--field-bg);color:var(--field-ink);font:inherit;font-size:16px}
textarea.f{min-height:38vh;resize:vertical;line-height:1.45}
.f:focus{outline:2px solid var(--accent);outline-offset:1px}
.chk{display:flex;align-items:center;gap:8px;margin-top:12px;color:var(--dim);font-size:15px}
.chk input{width:20px;height:20px;accent-color:var(--accent)}

#toast{position:fixed;left:16px;right:16px;bottom:calc(92px + env(safe-area-inset-bottom));z-index:20;
  padding:12px 14px;border-radius:var(--r);background:var(--ink);color:var(--bg);font-size:14px;
  opacity:0;transform:translateY(8px);transition:.2s;pointer-events:none}
#toast.show{opacity:1;transform:none}
#toast.bad{background:var(--bad);color:#fff}
</style>
</head>
<body>
<header>
  <div class="brand"><img src="/icon.png" alt=""><b>Loom</b><span class="n" id="count"></span>
    <button class="gear" id="gear" aria-label="Settings">&#9881;</button></div>
  <input id="q" type="search" placeholder="Search jots" autocomplete="off" autocapitalize="off" enterkeyhint="search">
  <div class="tabs"><button data-v="recent" class="on">Recent</button><button data-v="todo">TODO</button></div>
</header>
<main id="list"></main>
<button id="fab" aria-label="New jot">+</button>

<section class="sheet" id="view">
  <div class="sbar"><button data-close>&#8249; Back</button><span class="ttl"></span><button id="vEdit">Edit</button></div>
  <div class="sbody">
    <div id="vSummary"></div><div id="vName"></div>
    <div id="vText"></div><div id="vTags" class="foot"></div><div id="vMeta"></div>
    <div class="acts"><button class="btn good" id="vDone">Mark done</button></div>
  </div>
</section>

<section class="sheet" id="edit">
  <div class="sbar"><button data-close>Cancel</button><span class="ttl" id="eTitle">New jot</span><button id="eSave">Save</button></div>
  <div class="sbody">
    <label for="eText">Text</label><textarea class="f" id="eText" placeholder="What do you want to remember?"></textarea>
    <label for="eSummary">Summary</label><input class="f" id="eSummary" placeholder="Optional one-liner">
    <label for="eTags">Tags</label><input class="f" id="eTags" placeholder="comma separated" autocapitalize="off" autocorrect="off">
    <div class="chk" id="eTodoRow"><input type="checkbox" id="eTodo"><span>TODO</span></div>
    <div class="acts" id="eDelRow"><button class="btn ghost" id="eDel">Delete</button></div>
  </div>
</section>

<section class="sheet" id="cfg">
  <div class="sbar"><button data-close>Close</button><span class="ttl">Settings</span><button id="cSave">Save</button></div>
  <div class="sbody">
    <label for="cToken">Bearer token</label>
    <input class="f" id="cToken" type="password" placeholder="Leave empty if the server has none" autocapitalize="off" autocorrect="off">
    <label for="cEditor">Write as editor</label>
    <input class="f" id="cEditor" placeholder="user" autocapitalize="off" autocorrect="off">
  </div>
</section>
<div id="toast"></div>

<script>
'use strict';
const $=s=>document.querySelector(s);
const store={
  get(k){try{return localStorage.getItem('loom.m.'+k)||'';}catch(e){return'';}},
  set(k,v){try{v?localStorage.setItem('loom.m.'+k,v):localStorage.removeItem('loom.m.'+k);}catch(e){}}
};
const PAGE=40;
const S={view:'recent',q:'',jots:[],offset:0,more:false,cur:null,editing:null};

async function api(path,opts){
  const o=Object.assign({},opts);
  o.headers=Object.assign({},o.headers||{});
  const tok=store.get('token');
  if(tok)o.headers['Authorization']='Bearer '+tok;
  if(o.body&&typeof o.body!=='string'){o.body=JSON.stringify(o.body);o.headers['Content-Type']='application/json';}
  const r=await fetch(path,o);const text=await r.text();
  let body=null;try{body=text?JSON.parse(text):null;}catch(e){body={error:text};}
  if(!r.ok)throw Object.assign(new Error(body&&body.error?body.error:r.statusText),{status:r.status});
  return body;
}
function toast(msg,bad){
  const t=$('#toast');t.textContent=msg;t.className='show'+(bad?' bad':'');
  clearTimeout(t._t);t._t=setTimeout(()=>t.className='',bad?4500:2200);
}
function fail(e){
  toast(e.status===401?'Token needed - set it in Settings':(e.message||'Request failed'),true);
}

const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function marked(s){
  const h=esc(s);
  const terms=S.q.split(/\s+/).filter(w=>w.length>1).map(w=>w.replace(/[.*+?^${}()|[\]\\]/g,'\\$&'));
  return terms.length?h.replace(new RegExp('('+terms.join('|')+')','gi'),'<mark>$1</mark>'):h;
}
/* Ids are UTC microseconds; shown in the phone's own zone. */
const D=us=>new Date(us/1000);
function ago(us){
  const s=(Date.now()-us/1000)/1000;
  if(s<45)return'now';if(s<3600)return Math.round(s/60)+'m';
  if(s<86400)return Math.round(s/3600)+'h';if(s<86400*7)return Math.round(s/86400)+'d';
  const d=D(us);
  return d.toLocaleDateString(undefined,{month:'short',day:'numeric',
    year:d.getFullYear()===new Date().getFullYear()?undefined:'numeric'});
}
function dayLabel(us){
  const d=D(us),t=new Date(),y=new Date(t-864e5);
  if(d.toDateString()===t.toDateString())return'Today';
  if(d.toDateString()===y.toDateString())return'Yesterday';
  return d.toLocaleDateString(undefined,{weekday:'short',month:'short',day:'numeric',
    year:d.getFullYear()===t.getFullYear()?undefined:'numeric'});
}
const isTodo=j=>(j.tags||[]).includes('todo')&&!(j.tags||[]).includes('status:done');
const titleOf=j=>j.summary||j.name||j.snippet||(j.text?j.text.split('\n')[0]:'')||'(untitled)';
/* Structural tags (type:, asserted:, ...) are noise at phone width; the full set is on the jot. */
const shownTags=j=>(j.tags||[]).filter(t=>t.indexOf(':')<0).slice(0,4);

function query(){
  const p=new URLSearchParams({brief:'1',limit:String(PAGE),offset:String(S.offset)});
  if(S.q)p.set('q',S.q);
  if(S.view==='todo'){p.set('tag','todo');p.set('notag','status:done');}
  return '/jots?'+p.toString();
}
async function load(append){
  if(!append)S.offset=0;
  try{
    const r=await api(query());
    S.jots=append?S.jots.concat(r.jots):r.jots;
    S.more=!!r.truncated||(r.matched>S.offset+r.returned);
    $('#count').textContent=r.matched+(r.matched===1?' jot':' jots');
    render();
  }catch(e){fail(e);}
}
function render(){
  const list=$('#list');
  if(!S.jots.length){list.innerHTML='<div class="empty">'+(S.q?'No matches':'Nothing here')+'</div>';return;}
  let h='',lastDay='';
  const byDay=!S.q;   // a ranked list has no meaningful day orde
  for(const j of S.jots){
    const day=j.updated?dayLabel(j.updated):'';
    if(byDay&&day!==lastDay){h+='<div class="day">'+esc(day)+'</div>';lastDay=day;}
    const sub=j.summary&&j.name?j.name:'';
    h+='<button class="card'+(isTodo(j)?' todo':'')+'" data-id="'+j.id+'">'+
       '<div class="t">'+marked(titleOf(j))+'</div>'+
       (sub?'<div class="s">'+esc(sub)+'</div>':'')+
       '<div class="foot">'+shownTags(j).map(t=>'<span class="tag">'+esc(t)+'</span>').join('')+
       '<span class="when">'+(j.updated?ago(j.updated):'')+'</span></div></button>';
  }
  if(S.more)h+='<button class="more" id="more">Load more</button>';
  list.innerHTML=h;
}

function open(id){$(id).classList.add('show');}
function close(el){el.classList.remove('show');}
document.querySelectorAll('[data-close]').forEach(b=>b.onclick=()=>close(b.closest('.sheet')));

async function showJot(id){
  try{
    const j=await api('/jots/'+id);
    S.cur=j;
    $('#vSummary').textContent=j.summary||j.name||'(no summary)';
    $('#vName').textContent=j.summary&&j.name?j.name:'';
    $('#vText').textContent=j.text||'';
    $('#vTags').innerHTML=(j.tags||[]).map(t=>'<span class="tag">'+esc(t)+'</span>').join('');
    $('#vMeta').textContent=(j.updated?D(j.updated).toLocaleString():'')+(j.editor?' · '+j.editor:'');
    $('#vDone').style.display=isTodo(j)?'':'none';
    open('#view');$('#view .sbody').scrollTop=0;
  }catch(e){fail(e);}
}

function startEdit(j){
  S.editing=j||null;
  $('#eTitle').textContent=j?'Edit jot':'New jot';
  $('#eText').value=j?(j.text||''):'';
  $('#eSummary').value=j?(j.summary||''):'';
  const tags=j?(j.tags||[]):(S.view==='todo'?['todo']:[]);
  $('#eTodo').checked=tags.includes('todo')&&!tags.includes('status:done');
  $('#eTags').value=tags.filter(t=>t!=='todo').join(', ');
  $('#eDelRow').style.display=j?'':'none';
  open('#edit');
  if(!j)setTimeout(()=>$('#eText').focus(),50);
}
function parseTags(){
  let tags=$('#eTags').value.split(',').map(t=>t.trim()).filter(Boolean);
  /* Unticking an open TODO closes it rather than erasing it: todo + status:done is the record. */
  if($('#eTodo').checked)tags=tags.filter(t=>t!=='status:done').concat('todo');
  else if(S.editing&&isTodo(S.editing))tags.push('todo','status:done');
  else if(S.editing&&(S.editing.tags||[]).includes('todo'))tags.push('todo');
  return[...new Set(tags)];
}
async function save(){
  const text=$('#eText').value.trim(),summary=$('#eSummary').value.trim();
  if(!text&&!summary){toast('Write something first',true);return;}
  const body={text,summary,tags:parseTags()};
  const ed=store.get('editor');if(ed)body.editor=ed;
  const btn=$('#eSave');btn.disabled=true;
  try{
    if(S.editing){
      await api('/jots/'+S.editing.id+'?expect_updated='+S.editing.updated,{method:'PATCH',body});
      toast('Saved');close($('#edit'));showJot(S.editing.id);
    }else{
      await api('/jots',{method:'POST',body});
      toast('Added');close($('#edit'));
    }
    load();
  }catch(e){
    if(e.status===409)toast('Changed elsewhere - reopen it and try again',true);else fail(e);
  }finally{btn.disabled=false;}
}
async function markDone(){
  const j=S.cur;if(!j)return;
  const tags=[...new Set([...(j.tags||[]),'status:done'])];   // todo stays: the pair is the record
  try{
    await api('/jots/'+j.id+'?expect_updated='+j.updated,{method:'PATCH',body:{tags}});
    toast('Done');close($('#view'));load();
  }catch(e){
    if(e.status===409)toast('Changed elsewhere - reopen it and try again',true);else fail(e);
  }
}
async function del(){
  const j=S.editing;if(!j||!confirm('Delete this jot?'))return;
  try{
    await api('/jots/'+j.id,{method:'DELETE'});
    toast('Deleted');close($('#edit'));close($('#view'));load();
  }catch(e){fail(e);}
}

$('#list').addEventListener('click',e=>{
  if(e.target.closest('#more')){S.offset+=PAGE;load(true);return;}
  const c=e.target.closest('.card');if(c)showJot(c.dataset.id);
});
document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('.tabs button').forEach(x=>x.classList.toggle('on',x===b));
  S.view=b.dataset.v;window.scrollTo(0,0);load();
});
let qt;
$('#q').addEventListener('input',e=>{clearTimeout(qt);qt=setTimeout(()=>{S.q=e.target.value.trim();load();},250);});
$('#fab').onclick=()=>startEdit(null);
$('#vEdit').onclick=()=>startEdit(S.cur);
$('#vDone').onclick=markDone;
$('#eSave').onclick=save;
$('#eDel').onclick=del;
$('#gear').onclick=()=>{$('#cToken').value=store.get('token');$('#cEditor').value=store.get('editor');open('#cfg');};
$('#cSave').onclick=()=>{store.set('token',$('#cToken').value.trim());store.set('editor',$('#cEditor').value.trim());
  close($('#cfg'));toast('Saved');load();};
/* Coming back to the app after a while should show what agents wrote in the meantime. */
document.addEventListener('visibilitychange',()=>{if(!document.hidden&&!document.querySelector('.sheet.show'))load();});
load();
</script>
</body>
</html>
)HTML";
}

// The web app manifest. start_url and scope pin the installed app to /m, so a link out of it to
// the dashboard opens in the browser rather than inside the phone app's frame.
inline const char* LoomMobileManifest()
{
    return
R"JSON({
  "name": "Loom",
  "short_name": "Loom",
  "start_url": "/m",
  "scope": "/m",
  "display": "standalone",
  "background_color": "#13111f",
  "theme_color": "#5040dc",
  "icons": [
    { "src": "/icon-full.png", "sizes": "any", "type": "image/png", "purpose": "any" }
  ]
})JSON";
}
