// ---------------------------------------------------------------------------
// tablero_html.h — Página del tablero de control, guardada en flash.
// No usa librerías externas ni CDN: la WLAN de la alcaldía puede no tener
// salida a internet. La gráfica se dibuja con SVG.
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>

const char TABLERO_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Monitor hídrico</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--txt:#1d2733;--sub:#5b6b7c;--borde:#dde3ea;
--verde:#1e8e4e;--ambar:#c77700;--rojo:#c62828;--azul:#1565c0}
@media (prefers-color-scheme:dark){:root{--bg:#11161c;--card:#1b232c;--txt:#e6edf3;
--sub:#9aa9b8;--borde:#2c3945;--azul:#64a8f0}}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--txt)}
header{display:flex;justify-content:space-between;align-items:center;padding:12px 16px;border-bottom:1px solid var(--borde);background:var(--card)}
h1{font-size:1.1rem;margin:0}
main{max-width:960px;margin:0 auto;padding:16px}
.con{font-size:.85rem;color:var(--sub)}
.con b{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--rojo);margin-right:6px}
.con.ok b{background:var(--verde)}
#banner{border-radius:10px;padding:14px 16px;margin-bottom:16px;display:flex;flex-wrap:wrap;gap:12px;justify-content:space-between;align-items:center;color:#fff;background:var(--verde)}
#banner.PRECAUCION{background:var(--ambar)}#banner.ALERTA,#banner.SILENCIADA{background:var(--rojo)}
#banner .t{font-size:1.2rem;font-weight:700}#banner .s{font-size:.9rem;opacity:.9}
button{font:inherit;border:0;border-radius:8px;padding:10px 14px;cursor:pointer;font-weight:600}
#btnAlarma{background:#fff;color:var(--rojo)}#btnAlarma[hidden]{display:none}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--borde);border-radius:10px;padding:12px}
.card .n{font-size:.8rem;color:var(--sub)}.card .v{font-size:1.5rem;font-weight:700;margin-top:4px}
.card .v small{font-size:.85rem;font-weight:400;color:var(--sub)}
.card.falla{border-color:var(--rojo)}
#causa{margin-bottom:16px}
.tabs{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:8px}
.tabs button{background:var(--bg);color:var(--txt);border:1px solid var(--borde);padding:6px 10px;font-weight:400}
.tabs button.on{background:var(--azul);color:#fff;border-color:var(--azul)}
svg{width:100%;height:220px;display:block}
svg text{fill:var(--sub);font-size:11px}
ul{list-style:none;padding:0;margin:0;font-size:.9rem}
li{padding:6px 0;border-bottom:1px solid var(--borde)}li span{color:var(--sub);margin-right:8px}
.fila{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:8px}
h2{font-size:1rem;margin:0}
#btnSonido{background:var(--bg);color:var(--txt);border:1px solid var(--borde);font-weight:400;padding:6px 10px}
</style>
</head>
<body>
<header><h1>Monitor hídrico · Sabana Centro</h1><div id="con" class="con"><b></b><span>Conectando…</span></div></header>
<main>
<section id="banner"><div><div class="t" id="bEstado">—</div><div class="s" id="bDetalle">Esperando datos</div></div>
<button id="btnAlarma" hidden></button></section>

<section class="grid">
<div class="card" id="cNivel"><div class="n">Nivel de agua</div><div class="v" id="vNivel">—</div></div>
<div class="card" id="cTemp"><div class="n">Temperatura</div><div class="v" id="vTemp">—</div></div>
<div class="card" id="cHum"><div class="n">Humedad relativa</div><div class="v" id="vHum">—</div></div>
<div class="card" id="cPres"><div class="n">Presión</div><div class="v" id="vPres">—</div></div>
<div class="card"><div class="n">Radiación (LDR)</div><div class="v" id="vLdr">—</div></div>
<div class="card"><div class="n">Evaporación potencial</div><div class="v" id="vEvap">—</div></div>
<div class="card"><div class="n">Tasa de descenso</div><div class="v" id="vTasa">—</div></div>
<div class="card"><div class="n">Riesgo</div><div class="v" id="vRiesgo">—</div></div>
</section>

<section class="card" id="causa" hidden></section>

<section class="card" style="margin-bottom:16px">
<div class="fila"><h2>Histórico reciente</h2><span class="con" id="rango"></span></div>
<div class="tabs" id="tabs"></div>
<svg id="graf" role="img" aria-label="Gráfica del histórico"></svg>
</section>

<section class="card">
<div class="fila"><h2>Registro de eventos</h2><button id="btnSonido">🔇 Sonido</button></div>
<ul id="eventos"><li>Sin eventos</li></ul>
</section>
</main>
<script>
const $=id=>document.getElementById(id);
const SERIES={riesgo:{n:'Riesgo',u:'',d:2,umbrales:[.40,.65]},nivel_cm:{n:'Nivel',u:'cm',d:1},
temp_c:{n:'Temperatura',u:'°C',d:1},hum_pct:{n:'Humedad',u:'%',d:1},presion_hpa:{n:'Presión',u:'hPa',d:1},
evaporacion:{n:'Evaporación',u:'',d:2}};
const MAX=360;let hist=[],serie='riesgo',offset=0,sonido=false,ctxAudio=null,ultimo=null,titulo=null;

const fmt=(v,d)=>v==null?'—':Number(v).toFixed(d);
const hora=t=>new Date(offset+t).toLocaleTimeString('es-CO',{hour:'2-digit',minute:'2-digit',second:'2-digit'});

function tabs(){const c=$('tabs');c.innerHTML='';for(const k in SERIES){const b=document.createElement('button');
b.textContent=SERIES[k].n;b.className=k===serie?'on':'';b.onclick=()=>{serie=k;tabs();dibujar()};c.appendChild(b)}}

function dibujar(){const s=$('graf'),W=Math.max(s.clientWidth,280),H=220,m={l:44,r:8,t:10,b:22};s.setAttribute('viewBox',`0 0 ${W} ${H}`);s.innerHTML='';
const p=hist.filter(r=>r[serie]!=null);if(p.length<2){s.innerHTML=`<text x="${W/2}" y="110" text-anchor="middle">Aún no hay suficientes datos</text>`;return}
const cfg=SERIES[serie];let lo=Math.min(...p.map(r=>r[serie])),hi=Math.max(...p.map(r=>r[serie]));
if(cfg.umbrales){lo=0;hi=1}if(hi-lo<1e-6){lo-=1;hi+=1}const pad=(hi-lo)*.08;if(!cfg.umbrales){lo-=pad;hi+=pad}
const t0=p[0].t,t1=p[p.length-1].t,X=t=>m.l+(t-t0)/(t1-t0||1)*(W-m.l-m.r),Y=v=>m.t+(1-(v-lo)/(hi-lo))*(H-m.t-m.b);
let g='';for(let i=0;i<=4;i++){const v=lo+(hi-lo)*i/4,y=Y(v);
g+=`<line x1="${m.l}" x2="${W-m.r}" y1="${y}" y2="${y}" stroke="currentColor" stroke-opacity=".12"/><text x="${m.l-6}" y="${y+4}" text-anchor="end">${v.toFixed(cfg.d)}</text>`}
(cfg.umbrales||[]).forEach((u,i)=>{const y=Y(u);g+=`<line x1="${m.l}" x2="${W-m.r}" y1="${y}" y2="${y}" stroke="${i?'var(--rojo)':'var(--ambar)'}" stroke-dasharray="5 4"/>`});
g+=`<polyline fill="none" stroke="var(--azul)" stroke-width="2" points="${p.map(r=>X(r.t)+','+Y(r[serie])).join(' ')}"/>`;
g+=`<text x="${m.l}" y="${H-4}">${hora(t0)}</text><text x="${W-m.r}" y="${H-4}" text-anchor="end">${hora(t1)}</text>`;
s.innerHTML=g;$('rango').textContent=`${p.length} muestras · ${cfg.u||'índice 0–1'}`}

function mostrar(e){ultimo=e;offset=Date.now()-e.uptime_ms;
$('vNivel').innerHTML=`${fmt(e.nivel_cm,1)} <small>cm · ${fmt(e.nivel_pct,0)} %</small>`;
$('vTemp').innerHTML=`${fmt(e.temp_c,1)} <small>°C</small>`;$('vHum').innerHTML=`${fmt(e.hum_pct,1)} <small>%</small>`;
$('vPres').innerHTML=`${fmt(e.presion_hpa,1)} <small>hPa</small>`;$('vLdr').innerHTML=`${e.ldr} <small>/ 4095</small>`;
$('vEvap').textContent=fmt(e.evaporacion,2);$('vTasa').innerHTML=`${fmt(e.tasa_cm_muestra,2)} <small>cm/muestra</small>`;
$('vRiesgo').textContent=fmt(e.riesgo,2);
$('cNivel').classList.toggle('falla',!e.sensores.nivel);$('cTemp').classList.toggle('falla',!e.sensores.dht22);
$('cHum').classList.toggle('falla',!e.sensores.dht22);$('cPres').classList.toggle('falla',!e.sensores.bmp280);
const b=$('banner');b.className=e.estado;
const txt={NORMAL:'Normal',PRECAUCION:'Precaución',ALERTA:'¡ALERTA de desabastecimiento!',SILENCIADA:'Alerta activa · alarma silenciada'};
$('bEstado').textContent=txt[e.estado];
const fallas=['nivel','dht22','bmp280'].filter(k=>!e.sensores[k]);
$('bDetalle').textContent=`Riesgo ${fmt(e.riesgo,2)} · última medición ${hora(e.t)}`+(e.ventana_llena?'':' · calentando ventana')+(fallas.length?` · falla: ${fallas.join(', ')}`:'');
const btn=$('btnAlarma');btn.hidden=!(e.estado==='ALERTA'||e.estado==='SILENCIADA');
btn.textContent=e.estado==='ALERTA'?'🔕 Silenciar alarma física':'🔔 Rearmar alarma';
const c=$('causa');const causas={FUGA:'⚠️ El nivel baja rápido sin que el clima lo explique: posible fuga o sobreconsumo.',EVAPORACION:'☀️ El descenso coincide con alta evaporación (calor, baja humedad, radiación).'};
c.hidden=!causas[e.causa];c.textContent=causas[e.causa]||'';
if(e.estado==='ALERTA')parpadear();else detenerParpadeo()}

function agregarHist(e){if(hist.length&&hist[hist.length-1].t===e.t)return;
hist.push({t:e.t,nivel_cm:e.nivel_cm,temp_c:e.temp_c,hum_pct:e.hum_pct,presion_hpa:e.presion_hpa,evaporacion:e.evaporacion,riesgo:e.riesgo});
if(hist.length>MAX)hist.shift();dibujar()}

async function cargarEventos(){try{const r=await fetch('/api/eventos');const ev=await r.json();const ul=$('eventos');ul.innerHTML='';
if(!ev.length){ul.innerHTML='<li>Sin eventos</li>';return}
ev.forEach(x=>{const li=document.createElement('li'),s=document.createElement('span');s.textContent=hora(x.t);li.append(s,x.texto);ul.appendChild(li)})}catch(_){}}

function pitar(){if(!sonido||!ctxAudio)return;for(let i=0;i<3;i++){const o=ctxAudio.createOscillator(),g=ctxAudio.createGain();
o.frequency.value=880;o.connect(g);g.connect(ctxAudio.destination);const t=ctxAudio.currentTime+i*.4;
g.gain.setValueAtTime(.2,t);g.gain.setValueAtTime(0,t+.25);o.start(t);o.stop(t+.3)}}
function parpadear(){if(titulo)return;let on=false;titulo=setInterval(()=>{document.title=(on=!on)?'⚠️ ALERTA':'Monitor hídrico'},800)}
function detenerParpadeo(){clearInterval(titulo);titulo=null;document.title='Monitor hídrico'}

$('btnSonido').onclick=()=>{sonido=!sonido;if(sonido&&!ctxAudio)ctxAudio=new (window.AudioContext||window.webkitAudioContext)();
$('btnSonido').textContent=sonido?'🔊 Sonido':'🔇 Sonido';if(sonido)pitar()};

$('btnAlarma').onclick=async()=>{const accion=ultimo&&ultimo.estado==='ALERTA'?'silenciar':'rearmar';
const r=await fetch('/api/alarma/'+accion,{method:'POST'});if(r.ok){mostrar(await r.json());cargarEventos()}
else{const e=await r.json().catch(()=>({}));alert(e.error||'No se pudo completar la acción')}};

async function iniciar(){tabs();
const e=await (await fetch('/api/estado')).json();offset=Date.now()-e.uptime_ms;
const h=await (await fetch('/api/historico')).json();
hist=h.datos.map(f=>Object.fromEntries(h.campos.map((c,i)=>[c,f[i]])));
if(e.muestras>0)mostrar(e);dibujar();cargarEventos();
const es=new EventSource('/events');
es.onopen=()=>{$('con').className='con ok';$('con').querySelector('span').textContent='En línea'};
es.onerror=()=>{$('con').className='con';$('con').querySelector('span').textContent='Sin conexión'};
es.addEventListener('lectura',ev=>{const d=JSON.parse(ev.data);mostrar(d);agregarHist(d);cargarEventos()});
es.addEventListener('alerta',ev=>{const d=JSON.parse(ev.data);mostrar(d);pitar();cargarEventos();
if(document.hidden)document.title='⚠️ ALERTA'})}
window.addEventListener('resize',dibujar);
iniciar().catch(()=>{$('bDetalle').textContent='No se pudo conectar con el nodo'});
</script>
</body>
</html>)rawliteral";
