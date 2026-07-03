// Embedded configuration page. Served from PROGMEM at "/".
// The browser (not the ESP32) queries the EA API for station search,
// so the device never handles large responses.
#pragma once
#include <pgmspace.h>

static const char CONFIG_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EA HOF Monitor</title>
<style>
:root{
  --ink:#152228; --paper:#eef1ee; --card:#ffffff; --line:#c9d2cf;
  --river:#1d5f6e; --river-deep:#123f4a; --alert:#b3261e; --warn:#9a6a00;
  --ok:#2f6b3a; --mut:#5c6b6f;
}
*{box-sizing:border-box}
body{margin:0;background:var(--paper);color:var(--ink);
  font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:680px;margin:0 auto;padding:16px 16px 64px}
header{border-left:6px solid var(--river);padding:6px 0 6px 14px;margin:18px 0 22px}
header h1{margin:0;font-size:1.35rem;letter-spacing:.01em}
header p{margin:4px 0 0;color:var(--mut);font-size:.9rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;
  padding:16px;margin-bottom:16px}
.card h2{margin:0 0 10px;font-size:.8rem;text-transform:uppercase;
  letter-spacing:.08em;color:var(--river-deep)}
label{display:block;font-size:.85rem;color:var(--mut);margin:10px 0 4px}
input,select,button{font:inherit}
input,select{width:100%;padding:9px 10px;border:1px solid var(--line);
  border-radius:8px;background:#fff}
input:focus,select:focus,button:focus{outline:3px solid #9cc4cd;outline-offset:1px}
.row{display:flex;gap:10px}.row>*{flex:1}
button{padding:10px 16px;border-radius:8px;border:1px solid var(--river-deep);
  background:var(--river);color:#fff;cursor:pointer}
button.ghost{background:#fff;color:var(--river-deep)}
button:disabled{opacity:.5;cursor:default}
.results{margin:8px 0 0;padding:0;list-style:none;max-height:230px;overflow:auto;
  border:1px solid var(--line);border-radius:8px}
.results li{padding:9px 12px;border-bottom:1px solid var(--line);cursor:pointer}
.results li:last-child{border-bottom:0}
.results li:hover{background:#e7efee}
.results .sub{display:block;font-size:.8rem;color:var(--mut)}
.pick{background:#eff5f4;border:1px solid var(--line);border-radius:8px;
  padding:10px 12px;margin-top:10px;font-size:.92rem}
.pick strong{color:var(--river-deep)}
.hint{font-size:.8rem;color:var(--mut);margin-top:6px}
.msg{font-size:.9rem;margin-top:10px;min-height:1.2em}
.msg.ok{color:var(--ok)}.msg.err{color:var(--alert)}
/* status board — styled like a staff gauge stripe */
.board{display:flex;align-items:stretch;gap:0;border:1px solid var(--line);
  border-radius:8px;overflow:hidden;margin-top:8px}
.board .stripe{width:14px;background:
  repeating-linear-gradient(to bottom,var(--river-deep) 0 12px,#fff 12px 24px)}
.board .body{flex:1;padding:10px 14px}
.board .state{font-weight:700;letter-spacing:.03em}
.state.armed{color:var(--ok)}.state.warned{color:var(--warn)}
.state.TRIGGERED{color:var(--alert)}
.state.good{color:var(--ok)}.state.warn{color:var(--warn)}.state.bad{color:var(--alert)}
.badge{display:inline-flex;align-items:center;gap:6px;margin-left:8px;padding:2px 8px;
  border-radius:999px;border:1px solid var(--line);font-size:.78rem;font-weight:600;
  text-transform:uppercase;letter-spacing:.06em;vertical-align:middle}
.dot{width:8px;height:8px;border-radius:50%}
.badge.good{background:#edf7ef;color:var(--ok)} .badge.good .dot{background:var(--ok)}
.badge.warn{background:#fff6dd;color:var(--warn)} .badge.warn .dot{background:var(--warn)}
.badge.bad{background:#fdeceb;color:var(--alert)} .badge.bad .dot{background:var(--alert)}
.badge.neutral{background:#edf2f1;color:var(--mut)} .badge.neutral .dot{background:var(--mut)}
.linkish{background:none;border:none;color:var(--river-deep);cursor:pointer;
  padding:0;text-decoration:underline;font-size:.9rem}
footer{color:var(--mut);font-size:.78rem;margin-top:24px}
</style>
</head>
<body>
<div class="wrap">
<header>
  <h1>EA HOF Monitor</h1>
  <p>Alerts when flow or level at your licensed gauging station crosses a hands-off condition.</p>
</header>

<div class="card" id="statusCard" hidden>
  <h2>Device status</h2>
  <div id="boards"></div>
  <p class="hint" id="statusMeta"></p>
</div>

<div class="card">
  <h2>Notifications</h2>
  <label for="topic">ntfy.sh topic (subscribe to this in the ntfy app)</label>
  <input id="topic" placeholder="e.g. my-abstraction-alerts-x7q2">
  <p class="hint">Pick something unguessable — anyone who knows the topic name can read it.</p>
  <div class="row" style="margin-top:10px">
    <button class="ghost" type="button" onclick="testAlert()">Send test alert</button>
  </div>
  <p class="msg" id="topicMsg"></p>
</div>

<div id="siteBlocks"></div>
<p><button class="linkish" id="addSecond" type="button" onclick="showSecond()">+ Add a second station (rare — only if your licence names two)</button></p>

<div class="card">
  <div class="row">
    <button type="button" onclick="saveAll()">Save &amp; start monitoring</button>
  </div>
  <p class="msg" id="saveMsg"></p>
</div>

<footer>Data: Environment Agency real-time API (provisional, unvalidated).
This device is an early-warning aid — your licence and the EA's own
determination remain the authority on when abstraction must stop.</footer>
</div>

<script>
const EA='https://environment.data.gov.uk/flood-monitoring';
const N=2;                       // firmware supports two slots
const cfg={ntfyTopic:'',sites:[null,null]};

function siteHtml(i){return `
<div class="card" id="site${i}">
  <h2>Station ${i+1}</h2>
  <label>Search by station name, river, or town (as named in your licence)</label>
  <div class="row">
    <input id="q${i}" placeholder="e.g. Great Somerford">
    <select id="p${i}" style="max-width:130px">
      <option value="">flow + level</option>
      <option value="flow">flow only</option>
      <option value="level">level only</option>
    </select>
    <button class="ghost" type="button" style="max-width:110px" onclick="search(${i})">Search</button>
  </div>
  <ul class="results" id="res${i}" hidden></ul>
  <div class="pick" id="pick${i}" hidden></div>

  <div id="thr${i}" hidden>
    <label>Alert when the value…</label>
    <select id="dir${i}">
      <option value="below">falls below the trigger (hands-off flow / level)</option>
      <option value="above">rises above the trigger</option>
    </select>
    <div class="row">
      <div>
        <label>Licence trigger value</label>
        <input id="trig${i}" type="number" step="any" inputmode="decimal">
      </div>
      <div>
        <label>Units (as written in the licence)</label>
        <select id="unit${i}"></select>
      </div>
      <div>
        <label>Warning margin</label>
        <select id="marg${i}">
          <option value="5">5%</option>
          <option value="10" selected>10%</option>
          <option value="15">15%</option>
          <option value="20">20%</option>
        </select>
      </div>
    </div>
    <p class="hint" id="conv${i}"></p>
  </div>
  <p class="msg" id="msg${i}"></p>
</div>`;}

document.getElementById('siteBlocks').innerHTML=siteHtml(0)+siteHtml(1);
document.getElementById('site1').hidden=true;

function showSecond(){document.getElementById('site1').hidden=false;
  document.getElementById('addSecond').hidden=true;}

async function search(i){
  const q=document.getElementById('q'+i).value.trim();
  const p=document.getElementById('p'+i).value;
  const msg=document.getElementById('msg'+i); msg.textContent='Searching…';msg.className='msg';
  if(!q){msg.textContent='Type a station or river name first.';msg.className='msg err';return}
  try{
    let url=`${EA}/id/stations?search=${encodeURIComponent(q)}&_limit=15`;
    if(p)url+=`&parameter=${p}`;
    const r=await fetch(url); const d=await r.json();
    const ul=document.getElementById('res'+i); ul.innerHTML=''; ul.hidden=false;
    if(!d.items.length){msg.textContent='No stations found — try a shorter search term.';msg.className='msg err';return}
    msg.textContent='';
    d.items.forEach(st=>{
      const li=document.createElement('li');
      li.innerHTML=`<strong>${st.label??'(unnamed)'}</strong>
        <span class="sub">${st.riverName??''} · ${st.catchmentName??''} · ref ${st.stationReference??st.notation}</span>`;
      li.onclick=()=>pickStation(i,st);
      ul.appendChild(li);
    });
  }catch(e){msg.textContent='Search failed — check this device has internet access.';msg.className='msg err'}
}

async function pickStation(i,st){
  document.getElementById('res'+i).hidden=true;
  const pick=document.getElementById('pick'+i);
  pick.hidden=false; pick.innerHTML=`<strong>${st.label}</strong> — loading measures…`;
  const r=await fetch(`${EA}/id/stations/${st.notation}/measures`); const d=await r.json();
  if(!d.items.length){pick.innerHTML='This station publishes no live measures.';return}
  let html=`<strong>${st.label}</strong> (${st.riverName??'—'})<br>Choose the measure your licence refers to:<br>`;
  html+=`<select id="mp${i}" style="margin-top:6px">`;
  d.items.forEach((m,ix)=>{
    html+=`<option value="${ix}">${m.parameter} · ${m.qualifier??''} · ${m.unitName??''}</option>`;
  });
  html+=`</select><div class="hint" id="latest${i}" style="margin-top:6px"></div>`;
  pick.innerHTML=html;
  const sel=document.getElementById('mp'+i);
  const apply=()=>chooseMeasure(i,st,d.items[+sel.value]);
  sel.onchange=apply; apply();
}

async function chooseMeasure(i,st,m){
  const s={label:st.label,stationRef:st.stationReference??st.notation,
    measureId:m.notation??m['@id'].split('/').pop(),
    param:m.parameter,nativeUnit:m.unitName??(m.parameter==='flow'?'m3/s':'m')};
  cfg.sites[i]=Object.assign(cfg.sites[i]||{},s);
  // unit choices for the licence value
  const u=document.getElementById('unit'+i); u.innerHTML='';
  (m.parameter==='flow'?['l/s','m3/s']:['m']).forEach(x=>{
    u.appendChild(new Option(x==='m3/s'?'m³/s':x,x));});
  document.getElementById('thr'+i).hidden=false;
  updateConv(i);
  ['trig','unit','marg','dir'].forEach(id=>
    document.getElementById(id+i).oninput=()=>updateConv(i));
  // sanity check: show the live reading
  try{
    const r=await fetch(`${EA}/id/measures/${s.measureId}/readings?latest&_limit=1`);
    const d=await r.json(); const it=d.items[0];
    document.getElementById('latest'+i).textContent=
      `Latest reading: ${it.value} ${s.nativeUnit} at ${new Date(it.dateTime).toLocaleString()} — confirm this looks right for your licence point.`;
  }catch(e){}
}

function toNative(i,val){
  const unit=document.getElementById('unit'+i).value;
  const s=cfg.sites[i];
  if(unit==='l/s'&&s.param==='flow')return val/1000;
  return val;
}
function updateConv(i){
  const s=cfg.sites[i]; if(!s)return;
  const val=parseFloat(document.getElementById('trig'+i).value);
  const el=document.getElementById('conv'+i);
  if(isNaN(val)){el.textContent='';return}
  const below=document.getElementById('dir'+i).value==='below';
  const marg=parseFloat(document.getElementById('marg'+i).value)/100;
  const trig=toNative(i,val);
  const warn=below?trig*(1+marg):trig*(1-marg);
  el.textContent=`Trigger = ${trig.toPrecision(4)} ${s.nativeUnit} (EA units). `+
    `Warning fires at ${warn.toPrecision(4)} ${s.nativeUnit}; the same margin re-arms alerts after recovery.`;
}

async function testAlert(){
  const t=document.getElementById('topic').value.trim();
  const msg=document.getElementById('topicMsg');
  if(!t){msg.textContent='Enter a topic first.';msg.className='msg err';return}
  msg.textContent='Saving topic and sending…';msg.className='msg';
  // topic must reach the device before it can push
  await fetch('/save',{method:'POST',body:JSON.stringify(buildConfig(true))});
  const r=await fetch('/test',{method:'POST'});
  msg.textContent=r.ok?'Test sent — check your phone.':'Push failed — check the topic and internet access.';
  msg.className=r.ok?'msg ok':'msg err';
}

function buildConfig(draft){
  const out={ntfyTopic:document.getElementById('topic').value.trim(),sites:[]};
  for(let i=0;i<N;i++){
    const s=cfg.sites[i];
    if(!s||!s.measureId){out.sites.push(null);continue}
    const val=parseFloat(document.getElementById('trig'+i).value);
    const below=document.getElementById('dir'+i).value==='below';
    const marg=parseFloat(document.getElementById('marg'+i).value)/100;
    if(isNaN(val)){if(!draft)throw new Error(`Enter a trigger value for station ${i+1}.`);out.sites.push(null);continue}
    const trig=toNative(i,val);
    out.sites.push({label:s.label,stationRef:s.stationRef,measureId:s.measureId,
      param:s.param,nativeUnit:s.nativeUnit,
      licenceUnit:document.getElementById('unit'+i).value,
      below:below,trigger:trig,
      warnAt:below?trig*(1+marg):trig*(1-marg)});
  }
  return out;
}

async function saveAll(){
  const msg=document.getElementById('saveMsg');
  let c; try{c=buildConfig(false)}catch(e){msg.textContent=e.message;msg.className='msg err';return}
  if(!c.ntfyTopic){msg.textContent='Enter an ntfy topic.';msg.className='msg err';return}
  if(!c.sites[0]){msg.textContent='Choose a station and measure first.';msg.className='msg err';return}
  const r=await fetch('/save',{method:'POST',body:JSON.stringify(c)});
  msg.textContent=r.ok?'Saved. Monitoring has started — first check runs now.':'Save failed.';
  msg.className=r.ok?'msg ok':'msg err';
  if(r.ok)setTimeout(refreshStatus,3000);
}

async function refreshStatus(){
  try{
    const r=await fetch('/status'); const d=await r.json();
    if(!d.haveConfig)return;
    document.getElementById('statusCard').hidden=false;
    const b=document.getElementById('boards'); b.innerHTML='';
    d.sites.forEach(s=>{
      const div=document.createElement('div'); div.className='board';
      const age=s.readingAge!=null?`${Math.round(s.readingAge/60)} min ago`:'no reading yet';
      const val=s.licenceVal!=null?`${(+s.licenceVal).toPrecision(4)} ${s.licenceUnit}`:'—';
      const prev=s.prevLicenceVal!=null?`${(+s.prevLicenceVal).toPrecision(4)} ${s.licenceUnit}`:'—';
      const delta=(s.licenceVal!=null&&s.prevLicenceVal!=null&&s.readingAge!=null)
        ? ((+s.licenceVal)-(+s.prevLicenceVal)).toPrecision(4):null;
      const deltaText=delta==null?'':(delta>0?`+${delta}`:delta);
      const trendClass=s.trendState||'neutral';
      const bandClass=s.bandState||'neutral';
      const trendLabel=s.trend||'steady';
      const trendDetail=s.trendDetail||'no trend yet';
      const bandLabel=s.band||'no reading yet';
      div.innerHTML=`<div class="stripe"></div><div class="body">
        <div><strong>${s.label}</strong> — <span class="state ${s.state}">${s.stateLabel??s.state}${s.stale?' · DATA STALE':''}</span></div>
        <div style="margin-top:6px">
          <span class="badge ${bandClass}"><span class="dot"></span>${bandLabel}</span>
          <span class="badge ${trendClass}"><span class="dot"></span>${trendLabel}</span>
        </div>
        <div class="hint">Current: ${val} (${age}) · Prior: ${prev}${deltaText?` · Δ ${deltaText}`:''}</div>
        <div class="hint">Band: ${s.band??'—'} · Trend: ${trendDetail}</div></div>`;
      b.appendChild(div);
    });
    document.getElementById('statusMeta').textContent=
      `Up ${d.uptimeMin} min · next check in ${Math.max(0,Math.round(d.nextPollS/60))} min`;
  }catch(e){}
}

async function loadExisting(){
  try{
    const r=await fetch('/config'); const d=await r.json();
    if(d.ntfyTopic)document.getElementById('topic').value=d.ntfyTopic;
    // Existing sites are shown in the status board; re-run search to change them.
  }catch(e){}
  refreshStatus(); setInterval(refreshStatus,30000);
}
loadExisting();
</script>
</body>
</html>)HTML";
