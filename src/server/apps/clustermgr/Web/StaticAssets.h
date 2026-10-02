/*
 * This file is part of the C9Core Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef StaticAssets_h__
#define StaticAssets_h__

#include <string_view>

// ─── Embedded HTML ────────────────────────────────────────────────────────────
// Single-page application served at GET /.
// Leaflet.js + Chart.js loaded from CDN (ops workstation needs internet or LAN mirror).

inline constexpr std::string_view HTML_INDEX = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>C9Core Cluster Manager</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',Arial,sans-serif;background:#0d1117;color:#c9d1d9;display:flex;flex-direction:column;height:100vh;overflow:hidden}
header{background:#161b22;border-bottom:1px solid #30363d;padding:0 16px;height:44px;display:flex;align-items:center;gap:12px;flex-shrink:0}
header h1{font-size:16px;font-weight:600;color:#e6edf3;letter-spacing:.4px}
.badge-live{background:#238636;color:#fff;border-radius:10px;padding:2px 8px;font-size:11px;font-weight:600;animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
.badge-dis{background:#8b0000;color:#fff;border-radius:10px;padding:2px 8px;font-size:11px;font-weight:600}
.spacer{flex:1}
.btn{background:#21262d;border:1px solid #30363d;color:#c9d1d9;padding:4px 12px;border-radius:6px;cursor:pointer;font-size:13px}
.btn:hover{background:#30363d}
.btn-primary{background:#238636;border-color:#2ea043;color:#fff}
.btn-primary:hover{background:#2ea043}
.layout{display:flex;flex:1;overflow:hidden}
.sidebar{width:220px;background:#161b22;border-right:1px solid #30363d;overflow-y:auto;flex-shrink:0;padding:8px 0}
.sidebar-node{padding:4px 12px;cursor:pointer;border-left:3px solid transparent}
.sidebar-node:hover{background:#21262d}
.sidebar-node.active{background:#1f2937;border-left-color:#58a6ff}
.sidebar-node .node-name{font-size:13px;font-weight:600;display:flex;align-items:center;gap:6px}
.sidebar-node .map-list{margin:4px 0 4px 16px}
.sidebar-node .map-item{font-size:12px;color:#8b949e;padding:2px 0;cursor:pointer}
.sidebar-node .map-item:hover{color:#58a6ff}
.sidebar-footer{padding:12px;border-top:1px solid #30363d;margin-top:8px}
.dot{width:9px;height:9px;border-radius:50%;display:inline-block;flex-shrink:0}
.dot-green{background:#3fb950}.dot-yellow{background:#d29922}.dot-red{background:#f85149}.dot-gray{background:#484f58}
.main{flex:1;overflow-y:auto;padding:16px}

/* Cards */
.card-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px}
.card-header{display:flex;align-items:center;gap:8px;margin-bottom:12px}
.card-title{font-size:15px;font-weight:600;color:#e6edf3;flex:1;cursor:pointer}
.card-title:hover{color:#58a6ff;text-decoration:underline}
.state-badge{font-size:11px;font-weight:700;padding:2px 8px;border-radius:10px;text-transform:uppercase}
.state-running{background:#0d4a1e;color:#3fb950}.state-stopped{background:#21262d;color:#8b949e}
.state-starting{background:#3b2e00;color:#d29922}.state-crashed{background:#3b0000;color:#f85149}
.metrics{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}
.metric{background:#0d1117;border-radius:6px;padding:8px}
.metric-label{font-size:11px;color:#8b949e;margin-bottom:2px}
.metric-value{font-size:18px;font-weight:700;color:#e6edf3}
.metric-sub{font-size:11px;color:#8b949e}
.mem-bar{background:#21262d;border-radius:4px;height:6px;margin:4px 0 2px}
.mem-fill{background:#58a6ff;height:100%;border-radius:4px;transition:width .5s}
.sparkline-wrap{height:60px;margin-top:8px}
.crash-row{font-size:12px;color:#f85149;padding:8px 0;border-top:1px solid #30363d;margin-top:8px}

/* Node detail */
.section-title{font-size:14px;font-weight:600;color:#8b949e;text-transform:uppercase;letter-spacing:.8px;margin:16px 0 8px}
.detail-header{display:flex;align-items:center;gap:12px;margin-bottom:16px;flex-wrap:wrap}
.detail-stat{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:8px 16px;text-align:center}
.detail-stat-val{font-size:22px;font-weight:700;color:#e6edf3}
.detail-stat-lbl{font-size:11px;color:#8b949e}
.chart-wrap{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin-bottom:16px}
.chart-wrap canvas{max-height:160px}
.crash-table{width:100%;border-collapse:collapse;font-size:13px}
.crash-table th{text-align:left;color:#8b949e;padding:6px 8px;border-bottom:1px solid #30363d;font-weight:500}
.crash-table td{padding:6px 8px;border-bottom:1px solid #21262d;color:#c9d1d9}

/* Map viewer */
#map-container{height:calc(100vh - 44px - 32px);position:relative}
#leaflet-map{width:100%;height:100%;border-radius:8px;overflow:hidden;border:1px solid #30363d}
.map-tabs{display:flex;gap:8px;margin-bottom:8px}
.map-tab{padding:4px 12px;background:#21262d;border:1px solid #30363d;border-radius:6px;cursor:pointer;font-size:13px}
.map-tab.active{background:#1f2937;border-color:#58a6ff;color:#58a6ff}
.map-tab.no-tiles{color:#8b949e;border-style:dashed}

/* Deploy modal */
.modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:1000;align-items:center;justify-content:center}
.modal-overlay.open{display:flex}
.modal{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:24px;width:480px;max-width:95vw}
.modal-title{font-size:16px;font-weight:600;margin-bottom:16px;color:#e6edf3}
.form-group{margin-bottom:12px}
.form-group label{display:block;font-size:13px;color:#8b949e;margin-bottom:4px}
.form-group input,.form-group select{width:100%;background:#0d1117;border:1px solid #30363d;border-radius:6px;color:#c9d1d9;padding:6px 10px;font-size:13px}
.form-group input:focus,.form-group select:focus{outline:none;border-color:#58a6ff}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.modal-actions{display:flex;gap:8px;justify-content:flex-end;margin-top:16px}
.deploy-log{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:8px;font-family:monospace;font-size:12px;height:120px;overflow-y:auto;margin-top:12px;color:#3fb950}
.back-btn{display:flex;align-items:center;gap:6px;color:#58a6ff;cursor:pointer;font-size:14px;margin-bottom:12px}
.back-btn:hover{text-decoration:underline}
</style>
</head>
<body>
<header>
  <h1>&#11014; C9Core Cluster Manager</h1>
  <span id="conn-badge" class="badge-dis">CONNECTING</span>
  <div class="spacer"></div>
  <button class="btn btn-primary" onclick="openDeploy()">+ Deploy</button>
</header>
<div class="layout">
  <nav class="sidebar" id="sidebar"></nav>
  <main class="main" id="main-content"></main>
</div>

<!-- Deploy modal -->
<div class="modal-overlay" id="deploy-modal">
  <div class="modal">
    <div class="modal-title">Deploy Wizard</div>
    <div class="form-row">
      <div class="form-group"><label>SSH Host</label><input id="d-host" placeholder="192.0.2.71"></div>
      <div class="form-group"><label>SSH User</label><input id="d-user" value="wow"></div>
    </div>
    <div class="form-row">
      <div class="form-group"><label>SSH Port</label><input id="d-port" type="number" value="22"></div>
      <div class="form-group"><label>Node ID</label><input id="d-nodeid" type="number" value="2"></div>
    </div>
    <div class="form-group"><label>SSH Key Path</label><input id="d-key" value="~/.ssh/id_rsa"></div>
    <div class="form-group"><label>Remote Path</label><input id="d-path" value="/home/wow/wowcluster"></div>
    <div class="form-group"><label>Local Binary</label><input id="d-bin" value="./bin/worldserver"></div>
    <div id="deploy-log" class="deploy-log" style="display:none"></div>
    <div class="modal-actions">
      <button class="btn" onclick="closeDeploy()">Cancel</button>
      <button class="btn btn-primary" onclick="runDeploy()">Deploy</button>
    </div>
  </div>
</div>

<script>
// ── State ────────────────────────────────────────────────────────────────────
const state = { nodes: {}, players: [], ws: null, view: 'dashboard', selectedNode: null, selectedMap: null };
const sparkCharts = {};
const detailCharts = {};

// ── HTML escaping ────────────────────────────────────────────────────────────
// Every server-supplied string (node address, player name, map name from the
// tile index) goes through this before it is interpolated into innerHTML.
// The status/player feeds are authenticated on the bus, but the browser must
// not be the last line of defence against markup in a character name.
function esc(v) {
  return String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

// ── Tile index ───────────────────────────────────────────────────────────────
// Written by tools/minimap-tiles/minimap_tiles.py as {TilesPath}/maps.json and
// served through the same /tiles/ handler as the PNGs. Describes which maps
// have tiles and the pyramid geometry, so nothing about the map set or the
// projection is hand-typed here.
//
//   TILES.native_zoom  zoom at which one 256-px tile == one ADT cell (6)
//   TILES.grid         ADT cells per map edge (64)
//   TILES.adt_yards    yards per ADT cell (533.33333)
//   TILES.maps[]       {id, name, tiles, cols:[min,max], rows:[min,max]}
//
// Geometry: at zoom 0 the whole 64×64 grid is one 256-px tile, so in Leaflet's
// CRS.Simple one ADT is (256/64) = 4 units. Tile (x,y) at zoom 6 is ADT column
// x (west→east) and row y (north→south). WoW +X points north and +Y points
// west, so:
//   lng = (32 - worldY / 533.333) * 4
//   lat = -(32 - worldX / 533.333) * 4
const TILES = { native_zoom: 6, tile_size: 256, grid: 64, adt_yards: 533.33333, maps: [], byId: {} };

function loadTileIndex() {
  return fetch('/tiles/maps.json').then(r => r.json()).then(idx => {
    if (!idx || !Array.isArray(idx.maps)) return;
    Object.assign(TILES, idx);
    TILES.byId = {};
    for (const m of TILES.maps) TILES.byId[m.id] = m;
    if (state.view === 'map' && state.selectedNode !== null) showMap(state.selectedNode, state.selectedMap);
    else renderSidebar();
  }).catch(() => {}); // no tiles configured — map view degrades to a player list
}

function mapName(m) {
  const t = TILES.byId[m];
  return (t && t.name) || MAP_NAMES[m] || `Map ${m}`;
}

// Maps worth listing for a node: what it announced (continent/zone nodes) plus
// wherever its players actually are (the instance node announces no maps).
function mapsForNode(n) {
  const ids = new Set(n.mapIds || []);
  for (const p of state.players) if (p.nodeId == n.nodeId) ids.add(p.mapId);
  return [...ids].sort((a, b) => a - b);
}

const MAP_NAMES = {
  0:"Eastern Kingdoms",1:"Kalimdor",530:"Outland",571:"Northrend",
  33:"Shadowfang Keep",34:"Stormwind Stockade",36:"Deadmines",
  43:"Wailing Caverns",44:"Monastery",47:"Razorfen Kraul",
  48:"Blackfathom Deeps",70:"Uldaman",90:"Gnomeregan",
  109:"Sunken Temple",129:"Razorfen Downs",189:"Scarlet Monastery",
  209:"Zul Farrak",229:"Blackrock Spire",230:"Blackrock Depths",
  249:"Onyxia",269:"Opening of the Dark Portal",289:"Scholomance",
  309:"Zul Gurub",329:"Stratholme",349:"Maraudon",
  369:"Deeprun Tram",389:"Ragefire Chasm",429:"Dire Maul",
  469:"Blackwing Lair",489:"Warsong Gulch",509:"Ruins of Ahn Qiraj",
  529:"Arathi Basin",531:"Ahn Qiraj Temple",532:"Karazhan",
  533:"Naxxramas",534:"Hyjal Summit",540:"Hellfire Ramparts",
  542:"Blood Furnace",543:"Ramparts",544:"Magtheridon",
  545:"Steamvault",546:"Underbog",547:"Slave Pens",
  548:"Serpentshrine Cavern",550:"Tempest Keep",552:"Arcatraz",
  553:"Botanica",554:"Mechanar",555:"Shadow Labyrinth",
  556:"Sethekk Halls",557:"Mana-Tombs",558:"Auchenai Crypts",
  559:"Nagrand Arena",560:"Old Hillsbrad",564:"Black Temple",
  565:"Gruul",566:"Eye of the Storm",568:"Zul Aman",
  572:"Ruins of Lordaeron",574:"Utgarde Keep",575:"Utgarde Pinnacle",
  576:"Nexus",578:"Oculus",580:"Sunwell Plateau",
  585:"Magisters Terrace",595:"Culling of Stratholme",
  598:"Sunwell Fix",599:"Halls of Stone",600:"Drak Tharon Keep",
  601:"Azjol-Nerub",602:"Halls of Lightning",603:"Ulduar",
  604:"Gundrak",608:"Violet Hold",615:"Obsidian Sanctum",
  616:"Eye of Eternity",617:"Dalaran Sewers",619:"Ahn kahet",
  624:"Vault of Archavon",631:"Icecrown Citadel",
  632:"Forge of Souls",649:"Trial of the Crusader",
  650:"Trial of the Champion",658:"Pit of Saron",
  668:"Halls of Reflection",724:"Ruby Sanctum",
  30:"Alterac Valley",559:"Nagrand Arena",562:"Blade Edge Arena",
  572:"Ruins of Lordaeron",617:"Dalaran Sewers",618:"Ring of Valor",
  628:"Isle of Conquest",726:"Twin Peaks",761:"Battle for Gilneas"
};

const CLASS_NAMES = ['','Warrior','Paladin','Hunter','Rogue','Priest','DK','Shaman','Mage','Warlock','','Druid'];
const RACE_NAMES  = ['','Human','Orc','Dwarf','NE','Undead','Tauren','Gnome','Troll','','BE','Draenei'];

// ── WebSocket ────────────────────────────────────────────────────────────────
function connect() {
  const wsUrl = `ws://${location.host}/ws`;
  state.ws = new WebSocket(wsUrl);
  state.ws.onopen = () => {
    document.getElementById('conn-badge').className = 'badge-live';
    document.getElementById('conn-badge').textContent = 'LIVE';
  };
  state.ws.onclose = () => {
    document.getElementById('conn-badge').className = 'badge-dis';
    document.getElementById('conn-badge').textContent = 'RECONNECTING';
    setTimeout(connect, 3000);
  };
  state.ws.onerror = () => {};
  state.ws.onmessage = e => handleMsg(JSON.parse(e.data));
}

function handleMsg(msg) {
  if (msg.type === 'status') {
    msg.nodes.forEach(n => { state.nodes[n.nodeId] = n; });
    renderSidebar();
    if (state.view === 'dashboard') renderDashboard();
    else if (state.view === 'node' && state.selectedNode) renderNodeDetail(state.selectedNode);
  } else if (msg.type === 'players') {
    state.players = msg.players;
    renderSidebar();                       // per-map player counts + instance-node map list
    if (state.view === 'map') updateMapMarkers();
  } else if (msg.type === 'crash') {
    if (state.view === 'node' && state.selectedNode == msg.nodeId) renderNodeDetail(msg.nodeId);
  } else if (msg.type === 'deploy_log') {
    const log = document.getElementById('deploy-log');
    if (log) { log.textContent += msg.line + '\n'; log.scrollTop = log.scrollHeight; }
  }
}

// ── Sidebar ──────────────────────────────────────────────────────────────────
function renderSidebar() {
  const sb = document.getElementById('sidebar');
  const nodes = Object.values(state.nodes).sort((a,b)=>a.nodeId-b.nodeId);
  sb.innerHTML = nodes.map(n => {
    const dot = stateDot(n.state);
    const mapsHtml = mapsForNode(n).map(m => {
      const pc = state.players.filter(p=>p.mapId==m && p.nodeId==n.nodeId).length;
      return `<div class="map-item" onclick="event.stopPropagation();showMap(${n.nodeId},${m})">${esc(mapName(m))} (${pc})</div>`;
    }).join('');
    const active = (state.view==='node' && state.selectedNode==n.nodeId) ||
                   (state.view==='map'  && state.selectedNode==n.nodeId) ? ' active' : '';
    return `<div class="sidebar-node${active}" onclick="showNode(${n.nodeId})">
      <div class="node-name"><span class="dot ${dot}"></span>Node ${n.nodeId} <small style="color:#8b949e;font-weight:400">(${n.playerCount||0})</small></div>
      <div class="map-list">${mapsHtml}</div>
    </div>`;
  }).join('') + `<div class="sidebar-footer"><button class="btn" style="width:100%" onclick="openDeploy()">+ Deploy</button></div>`;
}

function stateDot(s) {
  return s==3?'dot-green':s==2?'dot-yellow':s==5?'dot-red':'dot-gray';
}

function stateLabel(s) {
  return ['UNKNOWN','STOPPED','STARTING','RUNNING','STOPPING','CRASHED'][s]||'?';
}

function stateBadgeClass(s) {
  return s==3?'state-running':s==2||s==4?'state-starting':s==5?'state-crashed':'state-stopped';
}

// ── Dashboard ────────────────────────────────────────────────────────────────
function showDashboard() {
  state.view='dashboard'; state.selectedNode=null; state.selectedMap=null;
  if (state._leafletMap) { state._leafletMap.remove(); state._leafletMap=null; }
  renderDashboard();
}

function renderDashboard() {
  const nodes = Object.values(state.nodes).sort((a,b)=>a.nodeId-b.nodeId);
  const main = document.getElementById('main-content');
  if (!nodes.length) { main.innerHTML='<p style="color:#8b949e;padding:32px">Waiting for nodes to publish status…</p>'; return; }

  main.innerHTML = `<div class="card-grid">${nodes.map(n => renderCard(n)).join('')}</div>`;

  nodes.forEach(n => {
    const history = n._history || [];
    const ctx = document.getElementById(`spark-${n.nodeId}`);
    if (!ctx) return;
    if (sparkCharts[n.nodeId]) { sparkCharts[n.nodeId].destroy(); delete sparkCharts[n.nodeId]; }
    sparkCharts[n.nodeId] = new Chart(ctx, {
      type:'line',
      data:{ labels: history.map((_,i)=>''), datasets:[{data:history.map(h=>h.playerCount),fill:true,borderColor:'#58a6ff',backgroundColor:'rgba(88,166,255,.15)',tension:.4,pointRadius:0,borderWidth:1.5}]},
      options:{ animation:false,responsive:true,maintainAspectRatio:false,plugins:{legend:{display:false}},scales:{x:{display:false},y:{display:false,min:0}}}
    });
  });
}

function renderCard(n) {
  const maxMem = 4096;
  const memPct = Math.min(100, Math.round((n.memUsageMB||0)/maxMem*100));
  const crash = n._lastCrash ? `<div class="crash-row">&#9888; Last crash: ${timeAgo(n._lastCrash.timestampSec)} · was up ${fmtUptime(n._lastCrash.uptimeSecs)} · ${n._lastCrash.playerCount} players</div>` : '';
  return `<div class="card">
    <div class="card-header">
      <div class="dot ${stateDot(n.state)}"></div>
      <div class="card-title" onclick="showNode(${n.nodeId})">Node ${n.nodeId}</div>
      <span class="state-badge ${stateBadgeClass(n.state)}">${stateLabel(n.state)}</span>
    </div>
    <div class="metrics">
      <div class="metric"><div class="metric-label">Players</div><div class="metric-value">${n.playerCount||0}</div><div class="metric-sub">/ ${n.maxPlayers||500}</div></div>
      <div class="metric"><div class="metric-label">Memory</div><div class="metric-value">${n.memUsageMB||0}</div><div class="metric-sub">MB</div>
        <div class="mem-bar"><div class="mem-fill" style="width:${memPct}%"></div></div></div>
      <div class="metric"><div class="metric-label">CPU</div><div class="metric-value">${n.cpuPercent||0}%</div></div>
      <div class="metric"><div class="metric-label">Uptime</div><div class="metric-value">${fmtUptimeShort(n.uptimeSecs||0)}</div></div>
    </div>
    <div class="sparkline-wrap"><canvas id="spark-${n.nodeId}"></canvas></div>
    ${crash}
  </div>`;
}

// ── Node Detail ──────────────────────────────────────────────────────────────
function showNode(nodeId) {
  state.view='node'; state.selectedNode=nodeId;
  if (state._leafletMap) { state._leafletMap.remove(); state._leafletMap=null; }
  Object.values(detailCharts).forEach(c=>c.destroy());
  for (const k in detailCharts) delete detailCharts[k];
  renderSidebar();
  renderNodeDetail(nodeId);
}

function renderNodeDetail(nodeId) {
  const n = state.nodes[nodeId];
  const main = document.getElementById('main-content');
  if (!n) { main.innerHTML='<p style="color:#8b949e">Node not found.</p>'; return; }

  const crashes = n._crashes || [];
  const history = n._history || [];

  main.innerHTML = `
    <div class="back-btn" onclick="showDashboard()">&#8592; Dashboard</div>
    <div class="detail-header">
      <div class="dot ${stateDot(n.state)}" style="width:14px;height:14px"></div>
      <h2 style="font-size:20px;color:#e6edf3">Node ${n.nodeId}</h2>
      <span class="state-badge ${stateBadgeClass(n.state)}" style="font-size:13px">${stateLabel(n.state)}</span>
    </div>
    <div class="detail-header">
      <div class="detail-stat"><div class="detail-stat-val">${n.playerCount||0}</div><div class="detail-stat-lbl">Players</div></div>
      <div class="detail-stat"><div class="detail-stat-val">${n.memUsageMB||0} MB</div><div class="detail-stat-lbl">Memory</div></div>
      <div class="detail-stat"><div class="detail-stat-val">${n.cpuPercent||0}%</div><div class="detail-stat-lbl">CPU</div></div>
      <div class="detail-stat"><div class="detail-stat-val">${fmtUptime(n.uptimeSecs||0)}</div><div class="detail-stat-lbl">Uptime</div></div>
      <div class="detail-stat"><div class="detail-stat-val">${n.pid||'-'}</div><div class="detail-stat-lbl">PID</div></div>
      <div class="detail-stat"><div class="detail-stat-val">${esc(n.address||'?')}:${parseInt(n.port)||8086}</div><div class="detail-stat-lbl">Address</div></div>
    </div>
    <div class="section-title">1-Hour History</div>
    <div class="chart-wrap"><canvas id="ch-players-${nodeId}"></canvas></div>
    <div class="chart-wrap"><canvas id="ch-mem-${nodeId}"></canvas></div>
    <div class="chart-wrap"><canvas id="ch-cpu-${nodeId}"></canvas></div>
    <div class="section-title">Maps</div>
    <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px">
      ${(n.mapIds||[]).map(m=>`<button class="btn" onclick="showMap(${nodeId},${m})">${MAP_NAMES[m]||('Map '+m)}</button>`).join('')}
    </div>
    <div class="section-title">Crash Log (${crashes.length})</div>
    <table class="crash-table">
      <thead><tr><th>Time</th><th>Was Up</th><th>Players at Crash</th></tr></thead>
      <tbody>${crashes.length ? crashes.slice().reverse().map(c=>`<tr><td>${timeAgoSec(c.timestampSec)}</td><td>${fmtUptime(c.uptimeSecs)}</td><td>${c.playerCount}</td></tr>`).join('') : '<tr><td colspan="3" style="color:#8b949e">No crashes recorded.</td></tr>'}</tbody>
    </table>`;

  const labels = history.map(h => timeAgoSec(h.timestampSec));
  makeDetailChart(`ch-players-${nodeId}`, 'Players', labels, history.map(h=>h.playerCount), '#58a6ff', detailCharts);
  makeDetailChart(`ch-mem-${nodeId}`,     'Memory MB', labels, history.map(h=>h.memUsageMB), '#f0883e', detailCharts);
  makeDetailChart(`ch-cpu-${nodeId}`,     'CPU %', labels, history.map(h=>h.cpuPercent), '#3fb950', detailCharts);
}

function makeDetailChart(id, label, labels, data, color, charts) {
  const ctx = document.getElementById(id);
  if (!ctx) return;
  charts[id] = new Chart(ctx, {
    type:'line',
    data:{ labels, datasets:[{label, data, borderColor:color, backgroundColor: color+'22', fill:true, tension:.3, pointRadius:0, borderWidth:1.5}]},
    options:{ animation:false,responsive:true,maintainAspectRatio:false,plugins:{legend:{display:false}},scales:{x:{display:false},y:{min:0,ticks:{color:'#8b949e'},grid:{color:'#21262d'}}}}
  });
}

// ── Map Viewer ───────────────────────────────────────────────────────────────
let _leafletMarkers = {};

function showMap(nodeId, mapId) {
  mapId = parseInt(mapId); nodeId = parseInt(nodeId);
  state.view='map'; state.selectedNode=nodeId; state.selectedMap=mapId;
  renderSidebar();

  const tileInfo = TILES.byId[mapId];
  const main = document.getElementById('main-content');
  const n = state.nodes[nodeId];

  const tabs = (n ? mapsForNode(n) : [mapId]).map(m =>
    `<div class="map-tab${m==mapId?' active':''}${TILES.byId[m]?'':' no-tiles'}" onclick="showMap(${nodeId},${m})">${esc(mapName(m))}</div>`
  ).join('');

  main.innerHTML = `
    <div class="back-btn" onclick="showNode(${nodeId})">&#8592; Node ${nodeId}</div>
    <div class="map-tabs">${tabs}</div>
    <div id="map-container"><div id="leaflet-map"></div></div>`;

  if (state._leafletMap) { state._leafletMap.remove(); state._leafletMap=null; _leafletMarkers={}; }

  if (!tileInfo) {
    // No tiles for this map (WMO-only interior, or generator not run): still
    // show who is here so the view is useful.
    const here = state.players.filter(p => p.mapId == mapId);
    document.getElementById('leaflet-map').innerHTML =
      `<p style="padding:32px 32px 8px;color:#8b949e">No tile data for ${esc(mapName(mapId))} (map ${mapId}).</p>` +
      `<ul style="padding:0 32px 32px 48px;color:#c9d1d9">` +
      here.map(p => `<li>${esc(p.name)} — L${parseInt(p.level)} ${esc(CLASS_NAMES[p.classId]||'?')} (node ${parseInt(p.nodeId)}) @ ${Number(p.x).toFixed(0)}, ${Number(p.y).toFixed(0)}</li>`).join('') +
      `</ul>`;
    return;
  }

  // Leaflet CRS.Simple over the tile pyramid: zoom 0 = whole grid in one tile.
  const U = TILES.tile_size / TILES.grid;             // CRS units per ADT (4)
  const bounds = L.latLngBounds(
    [-(tileInfo.rows[1] + 1) * U, tileInfo.cols[0] * U],    // south-west
    [-tileInfo.rows[0] * U,       (tileInfo.cols[1] + 1) * U]); // north-east
  const map = L.map('leaflet-map', {
    crs: L.CRS.Simple, minZoom: 0, maxZoom: TILES.native_zoom + 1,
    maxBounds: bounds.pad(0.25), attributionControl: false
  });
  state._leafletMap = map;

  L.tileLayer(`/tiles/${mapId}/{z}/{x}/{y}.png`, {
    noWrap: true, tileSize: TILES.tile_size,
    minNativeZoom: 0, maxNativeZoom: TILES.native_zoom,
    bounds: L.latLngBounds([-TILES.grid * U, 0], [0, TILES.grid * U])
  }).addTo(map);

  map.fitBounds(bounds);
  updateMapMarkers();
}

function updateMapMarkers() {
  const map = state._leafletMap;
  if (!map || state.view !== 'map') return;
  const mapId = state.selectedMap;

  const playersOnMap = state.players.filter(p => p.mapId == mapId);
  const guids = new Set(playersOnMap.map(p=>p.guid));

  // Remove stale markers
  for (const guid of Object.keys(_leafletMarkers)) {
    if (!guids.has(guid)) { _leafletMarkers[guid].remove(); delete _leafletMarkers[guid]; }
  }

  // Add/update markers
  for (const p of playersOnMap) {
    const latlng = wowToLatLng(p.x, p.y);
    const color = p.teamId==0 ? '#4488ff' : '#ff4444';
    const tooltipHtml = `<b>${esc(p.name)}</b><br>Level ${parseInt(p.level)} ${esc(CLASS_NAMES[p.classId]||'?')} ${esc(RACE_NAMES[p.raceId]||'?')}<br>${p.teamId==0?'Alliance':'Horde'} · node ${parseInt(p.nodeId)}<br><span style="color:#8b949e">${Number(p.x).toFixed(0)}, ${Number(p.y).toFixed(0)}, ${Number(p.z).toFixed(0)}</span>`;

    if (_leafletMarkers[p.guid]) {
      _leafletMarkers[p.guid].setLatLng(latlng);
      _leafletMarkers[p.guid].setTooltipContent(tooltipHtml);
    } else {
      const marker = L.circleMarker(latlng, {radius:6,fillColor:color,color:'#fff',fillOpacity:.9,weight:1.5}).addTo(map);
      marker.bindTooltip(tooltipHtml, {permanent:false,direction:'top'});
      _leafletMarkers[p.guid] = marker;
    }
  }
}

function wowToLatLng(x, y) {
  // WoW world space: +X is north, +Y is west. The tile grid's origin is the
  // north-west corner (ADT column 0 / row 0 == world Y = X = +32 cells), one
  // ADT is U CRS units. CRS.Simple maps lat to screen-up, so north is -row.
  const U = TILES.tile_size / TILES.grid;
  const half = TILES.grid / 2;
  const col = half - y / TILES.adt_yards;   // west → east
  const row = half - x / TILES.adt_yards;   // north → south
  return [-row * U, col * U];
}

// ── Deploy Wizard ────────────────────────────────────────────────────────────
function openDeploy() { document.getElementById('deploy-modal').classList.add('open'); document.getElementById('deploy-log').style.display='none'; document.getElementById('deploy-log').textContent=''; }
function closeDeploy() { document.getElementById('deploy-modal').classList.remove('open'); }

function runDeploy() {
  const body = JSON.stringify({
    host:    document.getElementById('d-host').value,
    user:    document.getElementById('d-user').value,
    port:    parseInt(document.getElementById('d-port').value)||22,
    nodeId:  parseInt(document.getElementById('d-nodeid').value)||2,
    key:     document.getElementById('d-key').value,
    path:    document.getElementById('d-path').value,
    binary:  document.getElementById('d-bin').value,
  });
  const log = document.getElementById('deploy-log');
  log.style.display='block'; log.textContent='Starting deploy…\n';
  fetch('/api/deploy',{method:'POST',headers:{'Content-Type':'application/json'},body})
    .then(r=>r.json()).then(d=>{ log.textContent += d.message + '\n'; }).catch(e=>{ log.textContent += 'Error: '+e+'\n'; });
}

// ── Helpers ──────────────────────────────────────────────────────────────────
function fmtUptime(secs) {
  if (!secs) return '-';
  const h=Math.floor(secs/3600), m=Math.floor((secs%3600)/60), s=secs%60;
  if (h>0) return `${h}h ${m}m`;
  if (m>0) return `${m}m ${s}s`;
  return `${s}s`;
}
function fmtUptimeShort(secs) {
  if (!secs) return '-';
  const h=Math.floor(secs/3600), m=Math.floor((secs%3600)/60);
  if (h>0) return `${h}h`;
  if (m>0) return `${m}m`;
  return `${secs}s`;
}
function timeAgo(ts) {
  const diff = Math.floor(Date.now()/1000) - ts;
  if (diff < 60) return diff+'s ago';
  if (diff < 3600) return Math.floor(diff/60)+'m ago';
  return Math.floor(diff/3600)+'h ago';
}
function timeAgoSec(ts) { return timeAgo(ts); }

// ── Fetch initial data ────────────────────────────────────────────────────────
function fetchStatus() {
  fetch('/api/status').then(r=>r.json()).then(d=>{
    d.nodes.forEach(n => {
      state.nodes[n.nodeId] = {...(state.nodes[n.nodeId]||{}), ...n};
    });
    renderSidebar();
    renderDashboard();
  }).catch(()=>{});
}

connect();
fetchStatus();
loadTileIndex();
showDashboard();
</script>
</body>
</html>
)HTML";

#endif // StaticAssets_h__
