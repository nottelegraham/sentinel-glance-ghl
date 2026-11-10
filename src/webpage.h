// Setup page served at http://<glance-ip>/ (and at 192.168.8.1 on the setup hotspot)
#pragma once
#include <pgmspace.h>

static const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sentinel Glance setup</title>
<style>
:root{--bg:#f2f4f7;--card:#fff;--ink:#1d2433;--mute:#667085;--line:#d0d5dd;--acc:#2f6fed;--red:#d93025}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 -apple-system,system-ui,sans-serif}
main{max-width:720px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 14px}h2{font-size:15px;margin:0 0 10px}h3{font-size:14px;margin:12px 0 4px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:12px}
label{display:block;font-size:12px;color:var(--mute);margin:8px 0 3px}
input,select{width:100%;padding:8px;border:1px solid var(--line);border-radius:6px;font:inherit;background:#fff}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}.row4{display:grid;grid-template-columns:2fr 1fr 1fr 1fr;gap:10px}
@media(max-width:520px){.row3,.row4{grid-template-columns:1fr 1fr}}
.chk{display:flex;align-items:center;gap:8px;margin-top:10px}.chk input{width:auto}
.tile{border-top:1px solid var(--line);padding-top:10px;margin-top:10px}.tile:first-of-type{border:0;margin:0;padding:0}
.head{display:flex;justify-content:space-between;align-items:center}
button{padding:10px 16px;border:0;border-radius:6px;background:var(--acc);color:#fff;font:inherit;font-weight:600;cursor:pointer}
button.ghost{background:#fff;color:var(--acc);border:1px solid var(--acc)}button.small{padding:5px 10px;font-size:13px}
button.danger{background:#fff;color:var(--red);border:1px solid var(--red)}button:disabled{opacity:.5}
.bar{display:flex;gap:10px;align-items:center;position:sticky;bottom:0;background:var(--bg);padding:10px 0}
#msg,#otaMsg{color:var(--mute);font-size:13px}.hide{display:none}small{color:var(--mute)}
</style></head><body><main>
<h1>Sentinel Glance setup</h1>

<section><h2>Wi-Fi</h2>
<label>Network</label><div class="row"><input id="ssid" list="nets"><button class="ghost" id="scan" type="button">Scan</button></div>
<datalist id="nets"></datalist>
<label>Password</label><input id="pass" type="password">
<label>Time zone (POSIX TZ string)</label><input id="tz"><small>Eastern US: EST5EDT,M3.2.0,M11.1.0 · Central: CST6CDT,M3.2.0,M11.1.0 · UK: GMT0BST,M3.5.0/1,M10.5.0 · Central Europe: CET-1CEST,M3.5.0,M10.5.0/3</small>
</section>

<section><h2>Controllers</h2><small>Up to six. ProfiLux covers the ProfiLux Mini, ProfiLux Light Wi-Fi and ProfiLux 4. Give each one a fixed IP address in your router. One controller is read every 30 s, in turn.</small>
<div id="devs"></div><p><button class="ghost small" id="addDev" type="button">Add controller</button></p></section>

<section><h2>Doser pumps</h2><small>Shown pumps appear on the Doser page, which alternates with the tiles (up to six rows across all dosers). Percent full = remaining volume ÷ container size. Container 0 shows mL instead. Low % 0 = no alarm.</small>
<div id="pumps"></div>
<label>Alternate tiles and Doser page every (seconds, 0 = off)</label><input id="rotateSec" type="number" min="0" max="120"></section>

<section><h2>Tiles</h2><small>Tiles 1 and 2 are on the home page with their graph. Tiles 3 and 4 are not drawn: they are recorded to the history CSV and can raise alarms. Doser pumps show on the Doser page directly. Sensor N reads the ProfiLux's sensor N live value; set its scale under Divide raw by (pH 100, temperature 10, most others 10). Alarm sensors: the sensor numbers whose controller alarm turns this tile red, e.g. "1" or "1,3". Doser tiles turn red at their pump's low %.</small><div id="tiles"></div></section>

<section><h2>Alarm light</h2>
<div class="row"><div><label>NeoPixel output</label><select id="glowPort"><option value="2">Inside, M-Bus pin 8 (G25): glow back or M5GO Bottom2</option><option value="0">Port A, white wire (G32)</option><option value="1">Port A, yellow wire (G33)</option></select></div>
<div><label>Number of NeoPixels</label><input id="glowCount" type="number" min="1" max="30"></div></div>
<label>Power input</label><select id="powerFromBack"><option value="0">USB-C on the side (Core2 supplies 5 V to the back)</option><option value="1">5 V into the back (M-Bus pin 28)</option></select>
<small>Set this BEFORE connecting power to the back. In USB-C mode the Core2 drives its own 5 V onto pin 28. The Glance restarts when this changes.</small></section>

<section><h2>Night</h2><div class="row3"><div><label>Night mode</label><select id="nightMode"><option value="0">Off</option><option value="1">Dim the screen</option><option value="2">Screen off</option></select></div><div><label>From (hour, 0–23)</label><input id="nightStart" type="number" min="0" max="23"></div><div><label>Until (hour, 0–23)</label><input id="nightEnd" type="number" min="0" max="23"></div></div><small>A touch wakes the screen for a minute. Any alarm brings it to full brightness. The glow is not affected.</small></section>

<section><h2>Firmware updates</h2>
<p id="otaInfo">Version …</p>
<div class="row"><button class="ghost" id="otaCheck" type="button">Check GitHub</button><button id="otaInstall" type="button" disabled>Install update</button></div>
<label>Or install a firmware .bin from your computer</label><div class="row"><input id="otaFile" type="file" accept=".bin"><button class="ghost" id="otaUpload" type="button">Upload and install</button></div>
<span class="chk"><input type="checkbox" id="otaDaily">Check GitHub once a day and show new versions under About</span>
<p id="otaMsg"></p><small>The Glance keeps its settings and history through an update and restarts when it's done. Installs only happen when you press a button here.</small></section>

<section><h2>History</h2><small>The Glance keeps 7 days at 1-minute detail and 90 days of 15-minute averages, saved in its flash. Delete it from Display settings on the device.</small>
<p><a href="/api/history.csv?r=7d">Download 7 days (1-minute) CSV</a><br><a href="/api/history.csv?r=90d">Download 90 days (15-minute) CSV</a></p></section>

<div class="bar"><button id="save">Save</button><span id="msg"></span></div>
</main>
<script>
const $=s=>document.querySelector(s);let cfg;
const esc=v=>String(v).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const PRESETS={0:"pH (Mini sensor 1)",1:"Temperature °F (Mini sensor 2)",2:"Temperature °C (Mini sensor 2)",8:"Sensor N (any ProfiLux)",3:"Wi-Fi signal (dBm)",4:"Custom register",7:"Doser pump % full"};
const bits=m=>{const a=[];for(let b=0;b<32;b++)if(m&(1<<b))a.push(b+1);return a.join(",")};
const mask=s=>String(s).split(/[ ,]+/).map(Number).filter(n=>n>=1&&n<=32).reduce((m,n)=>m|(1<<(n-1)),0)>>>0;
function devHtml(d,i){return `<div class="tile"><div class="head"><b>Controller ${i+1}</b><button class="danger small" type="button" data-rm="${i}">Remove</button></div><div class="row3">
<div><label>Type</label><select data-d="${i}" data-k="type" data-n><option value="0" ${d.type==0?"selected":""}>ProfiLux (Mini / Light / 4)</option><option value="1" ${d.type==1?"selected":""}>Doser 2.1 (port 10001)</option></select></div>
<div><label>Name</label><input data-d="${i}" data-k="name" value="${esc(d.name)}" maxlength="15"></div>
<div><label>IP address</label><input data-d="${i}" data-k="host" value="${esc(d.host)}" placeholder="192.168.1.50"></div></div>
<span class="chk"><input type="checkbox" data-d="${i}" data-k="enabled" ${d.enabled?"checked":""}>Enabled</span></div>`}
function pumpHtml(p,i,j){return `<div class="row4 tile"><div><label>Pump ${j+1} name</label><input data-dp="${i}" data-p="${j}" data-k="name" value="${esc(p.name)}" maxlength="15"></div><div><label>Container (mL)</label><input data-dp="${i}" data-p="${j}" data-k="cap" data-n value="${p.cap}"></div><div><label>Low alarm (%)</label><input data-dp="${i}" data-p="${j}" data-k="low" data-n value="${p.low}"></div><div><label>&nbsp;</label><span class="chk"><input type="checkbox" data-dp="${i}" data-p="${j}" data-k="show" ${p.show?"checked":""}>Show</span></div></div>`}
function tileHtml(t,i){const devOpts=cfg.devices.map((d,j)=>d._shown?`<option value="${j}" ${t.device==j?"selected":""}>${esc(d.name||("Controller "+(j+1)))}</option>`:"").join("");
const pre=Object.entries(PRESETS).map(([v,p])=>`<option value="${v}" ${t.preset==v?"selected":""}>${p}</option>`).join("");
const pumpNames=(cfg.devices[t.device]||{}).pumps||[];
return `<div class="tile"><b>Tile ${i+1}</b><div class="row3">
<div><label>Label</label><input data-t="${i}" data-k="label" value="${esc(t.label)}" maxlength="15"></div>
<div><label>Controller</label><select data-t="${i}" data-k="device" data-n>${devOpts}</select></div>
<div><label>Reading</label><select data-t="${i}" data-k="preset" data-n>${pre}</select></div></div>
<div class="row ${t.preset==7?"":"hide"}" id="p${i}"><div><label>Doser pump</label><select data-t="${i}" data-k="pump" data-n>${[1,2,3,4,5,6].map(j=>`<option value="${j}" ${t.reg==j?"selected":""}>${j} ${esc((pumpNames[j-1]||{}).name||"")}</option>`).join("")}</select></div></div>
<div class="row3 ${t.preset==4||t.preset==8?"":"hide"}" id="c${i}">
<div><label id="rl${i}">${t.preset==8?"Sensor number (1–32)":"Register (decimal)"}</label><input data-t="${i}" data-k="reg" data-n value="${t.reg}"></div>
<div><label>Divide raw by</label><input data-t="${i}" data-k="divisor" data-n value="${t.divisor}"></div>
<div><label>Decimals / unit</label><div class="row"><input data-t="${i}" data-k="decimals" data-n value="${t.decimals}"><input data-t="${i}" data-k="unit" value="${esc(t.unit)}" maxlength="7"></div></div></div>
<div class="row3"><div><label>Alarm sensors</label><input data-t="${i}" data-k="alarmBits" value="${bits(t.alarmMask)}" placeholder="none"></div>
<div><label>Accent colour</label><input type="color" data-t="${i}" data-k="color" value="${t.color}"></div>
<div><label>&nbsp;</label><span class="chk"><input type="checkbox" data-t="${i}" data-k="useLimits" ${t.useLimits?"checked":""}>Local limits</span></div></div>
<div class="row"><div><label>Low limit</label><input data-t="${i}" data-k="low" data-n value="${t.low}"></div>
<div><label>High limit</label><input data-t="${i}" data-k="high" data-n value="${t.high}"></div></div></div>`}
function render(){
cfg.devices.forEach(d=>{if(d._shown===undefined)d._shown=!!d.host});
if(!cfg.devices.some(d=>d._shown))cfg.devices[0]._shown=true;
$("#devs").innerHTML=cfg.devices.map((d,i)=>d._shown?devHtml(d,i):"").join("");
$("#addDev").disabled=cfg.devices.every(d=>d._shown);
$("#pumps").innerHTML=cfg.devices.map((d,i)=>d._shown&&d.type==1?`<h3>${esc(d.name||("Controller "+(i+1)))}</h3>`+d.pumps.map((p,j)=>pumpHtml(p,i,j)).join(""):"").join("")||"<p><small>No doser added.</small></p>";
$("#tiles").innerHTML=cfg.tiles.map(tileHtml).join("");
$("#rotateSec").value=cfg.rotateSec;$("#nightMode").value=cfg.nightMode;$("#nightStart").value=cfg.nightStart;$("#nightEnd").value=cfg.nightEnd;$("#glowPort").value=cfg.glowPort;$("#glowCount").value=cfg.glowCount;$("#powerFromBack").value=cfg.powerFromBack?"1":"0";$("#ssid").value=cfg.ssid;$("#pass").value=cfg.pass;$("#tz").value=cfg.tz;$("#otaDaily").checked=cfg.otaCheck;
document.querySelectorAll("[data-t][data-k=preset],[data-t][data-k=device],[data-d][data-k=type],[data-d][data-k=name]").forEach(s=>s.onchange=()=>{collect();render()});
document.querySelectorAll("[data-rm]").forEach(b=>b.onclick=()=>{collect();const d=cfg.devices[b.dataset.rm];d.host="";d.name="";d.enabled=false;d._shown=false;render()})}
function collect(){cfg.rotateSec=Number($("#rotateSec").value)||0;cfg.nightMode=Number($("#nightMode").value);cfg.nightStart=Number($("#nightStart").value)||0;cfg.nightEnd=Number($("#nightEnd").value)||0;cfg.glowPort=Number($("#glowPort").value);cfg.glowCount=Number($("#glowCount").value)||1;cfg.powerFromBack=$("#powerFromBack").value=="1";cfg.ssid=$("#ssid").value;cfg.pass=$("#pass").value;cfg.tz=$("#tz").value;cfg.otaCheck=$("#otaDaily").checked;
document.querySelectorAll("[data-k]").forEach(e=>{const o=e.dataset.dp!==undefined?cfg.devices[e.dataset.dp].pumps[e.dataset.p]:e.dataset.d!==undefined?cfg.devices[e.dataset.d]:e.dataset.t!==undefined?cfg.tiles[e.dataset.t]:null;if(!o)return;
o[e.dataset.k]=e.type=="checkbox"?e.checked:e.dataset.n!==undefined?Number(e.value):e.value});
cfg.tiles.forEach(t=>{if(t.preset==7&&t.pump)t.reg=t.pump;delete t.pump;if(t.alarmBits!==undefined){t.alarmMask=mask(t.alarmBits);delete t.alarmBits}})}
fetch("/api/config").then(r=>r.json()).then(j=>{cfg=j;render()});
$("#addDev").onclick=()=>{collect();const d=cfg.devices.find(d=>!d._shown);if(d){d._shown=true;d.enabled=true;if(!d.name)d.name="Controller";}render()};
$("#scan").onclick=async()=>{$("#msg").textContent="Scanning…";const l=await (await fetch("/api/scan")).json();
$("#nets").innerHTML=l.map(n=>`<option value="${esc(n)}">`).join("");$("#msg").textContent=l.length+" networks found"};
$("#save").onclick=async()=>{collect();$("#msg").textContent="Saving…";
const out=JSON.parse(JSON.stringify(cfg));out.devices.forEach(d=>delete d._shown);
const r=await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(out)});
const j=await r.json();$("#msg").textContent=j.reboot?"Saved. Glance is restarting to join "+cfg.ssid+".":"Saved."};
async function ota(){const j=await (await fetch("/api/ota")).json();
$("#otaInfo").textContent="Installed: v"+j.version+(j.latest?"  ·  Latest on GitHub: "+j.latest:"");
$("#otaInstall").disabled=!j.available;$("#otaMsg").textContent=j.progress>=0?"Installing: "+j.progress+"%":j.status;return j}
ota();setInterval(ota,3000);
$("#otaCheck").onclick=()=>fetch("/api/ota/check",{method:"POST"});
$("#otaInstall").onclick=()=>{if(confirm("Install the update from GitHub? The Glance restarts when it's done."))fetch("/api/ota/install",{method:"POST"})};
$("#otaUpload").onclick=()=>{const f=$("#otaFile").files[0];if(!f){$("#otaMsg").textContent="Choose a .bin file first.";return}
if(!confirm("Install "+f.name+"? The Glance restarts when it's done."))return;
const fd=new FormData();fd.append("firmware",f,f.name);const x=new XMLHttpRequest();x.open("POST","/api/ota/upload");
x.upload.onprogress=e=>{if(e.lengthComputable)$("#otaMsg").textContent="Uploading: "+Math.round(e.loaded*100/e.total)+"%"};
x.onload=()=>{$("#otaMsg").textContent=x.status==200?"Installed. The Glance is restarting.":"Update failed. The old firmware is still running."};
x.onerror=()=>{$("#otaMsg").textContent="Upload interrupted. The old firmware is still running."};x.send(fd)};
</script></body></html>)HTML";
