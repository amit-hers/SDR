'use strict';
// ── Chart.js defaults ──────────────────────────────────────────────────────
Chart.defaults.color = '#6b7280';
Chart.defaults.borderColor = '#2a2d3e';
Chart.defaults.font.family = "'Segoe UI', system-ui, sans-serif";

const WINDOW = 300;
const ACCENT = '#4f8ef7';
const GREEN  = '#22d07a';
const RED    = '#f75f5f';
const YELLOW = '#f7c948';

function makeLineChart(id, labels, datasets) {
    return new Chart(document.getElementById(id), {
        type: 'line',
        data: { labels, datasets },
        options: {
            animation: false, responsive: true, maintainAspectRatio: true,
            plugins: { legend: { display: datasets.length > 1 } },
            scales: { x: { display: false }, y: { grid: { color: '#2a2d3e' } } }
        }
    });
}

const signalChart = makeLineChart('chart-signal', [], [
    { label: 'RSSI dBm', data: [], borderColor: RED,   borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: false },
    { label: 'SNR dB',   data: [], borderColor: GREEN, borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: false },
]);
const tputChart = makeLineChart('chart-tput', [], [
    { label: 'TX kbps', data: [], borderColor: ACCENT, borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: 'origin' },
    { label: 'RX kbps', data: [], borderColor: GREEN,  borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: 'origin' },
]);
const specChart = new Chart(document.getElementById('chart-spectrum'), {
    type: 'bar',
    data: { labels: Array.from({length:256},(_,i)=>i), datasets:[{
        label:'Power dB', data: new Array(256).fill(-100),
        backgroundColor: ACCENT+'99', borderColor: ACCENT, borderWidth:0,
        barPercentage:1, categoryPercentage:1,
    }]},
    options: { animation:false, responsive:true, maintainAspectRatio:true,
        plugins:{legend:{display:false}},
        scales:{x:{display:false},y:{min:-120,max:0,grid:{color:'#2a2d3e'}}} }
});
const iqChart = new Chart(document.getElementById('chart-iq'), {
    type:'scatter', data:{datasets:[{label:'IQ',data:[],backgroundColor:ACCENT+'cc',pointRadius:2}]},
    options:{animation:false,responsive:true,maintainAspectRatio:true,
        plugins:{legend:{display:false}},
        scales:{x:{min:-2,max:2,grid:{color:'#2a2d3e'}},y:{min:-2,max:2,grid:{color:'#2a2d3e'}}}}
});
const fecChart = makeLineChart('chart-fec', [], [
    { label:'FEC corrected', data:[], borderColor:YELLOW, borderWidth:1.5, pointRadius:0, tension:0.3, fill:false },
]);
const scanChart = new Chart(document.getElementById('chart-scan'), {
    type:'bar', data:{labels:[],datasets:[{label:'Power dBm',data:[],backgroundColor:GREEN+'88',borderColor:GREEN,borderWidth:1}]},
    options:{animation:false,responsive:true,maintainAspectRatio:true,
        plugins:{legend:{display:false}},scales:{y:{min:-120,max:0,grid:{color:'#2a2d3e'}}}}
});

function push(chart, ...values) {
    chart.data.labels.push('');
    values.forEach((v,i) => chart.data.datasets[i].data.push(v));
    if (chart.data.labels.length > WINDOW) {
        chart.data.labels.shift();
        values.forEach((_,i) => chart.data.datasets[i].data.shift());
    }
    chart.update('none');
}

const MOD_LABELS = {0:'--',1:'BPSK',2:'QPSK',3:'16QAM',4:'64QAM'};

function activeNode() {
    return document.getElementById('active-node').value;
}

// ── Apply stats to the active-node detail view ────────────────────────────
function applyStats(stats) {
    const g = id => document.getElementById(id);
    const fmt = (v,d=1) => typeof v==='number' ? v.toFixed(d) : '--';

    g('s-rssi').textContent      = fmt(stats.rssi_dbm);
    g('s-snr').textContent       = fmt(stats.snr_db);
    g('s-tx-kbps').textContent   = fmt(stats.tx_kbps);
    g('s-rx-kbps').textContent   = fmt(stats.rx_kbps);
    g('s-frames-tx').textContent = stats.frames_tx ?? '--';
    g('s-frames-rx').textContent = stats.frames_rx_good ?? '--';
    g('s-dropped').textContent   = stats.dropped ?? '--';
    g('s-mod').textContent       = MOD_LABELS[stats.cur_mod] ?? '--';
    g('uptime').textContent      = formatUptime(stats.uptime_s ?? 0);
    if (typeof stats.temp_c === 'number')
        g('temp').textContent    = stats.temp_c.toFixed(1) + '°C';

    const corr = stats.fec_corrected ?? 0, uncorr = stats.fec_uncorrectable ?? 0;
    g('fec-corrected').textContent = corr;
    g('fec-uncorr').textContent    = uncorr;
    g('fec-rate').textContent      = (corr+uncorr)>0 ? (corr/(corr+uncorr)*100).toFixed(1)+'%':'0%';

    push(signalChart, stats.rssi_dbm??-100, stats.snr_db??0);
    push(tputChart,   stats.tx_kbps??0,     stats.rx_kbps??0);
    push(fecChart,    corr);

    if (Array.isArray(stats.spectrum) && stats.spectrum.length===256) {
        specChart.data.datasets[0].data = stats.spectrum;
        specChart.update('none');
    }
    iqChart.data.datasets[0].data = generateConstellation(stats.cur_mod??2, 64);
    iqChart.update('none');
}

// ── Update the per-node boxes and peer table ──────────────────────────────
let _lastPeerSeen = {};  // node -> {node_id -> last_seen_ms}

function applyNodeStatus(name, info) {
    const running = info.running;
    const stats   = info.stats || {};

    // Header pill
    const pill = document.getElementById(`pill-${name}`);
    if (pill) {
        const dot = pill.querySelector('.dot');
        if (dot) { dot.className = 'dot ' + (running ? 'running' : 'stopped'); }
    }

    // Node badge
    const nb = document.getElementById(`nb-${name}`);
    if (nb) { nb.textContent = running ? 'RUNNING' : 'STOPPED'; nb.className = 'node-badge '+(running?'running':'stopped'); }

    // Mini stats
    const n = name === 'node1' ? 1 : 2;
    const set = (id, v, d=1) => { const el = document.getElementById(id); if(el) el.textContent = typeof v==='number'?v.toFixed(d):'--'; };
    set(`np${n}-rssi`, stats.rssi_dbm);
    set(`np${n}-snr`,  stats.snr_db);
    set(`np${n}-tx`,   stats.tx_kbps);
    set(`np${n}-rx`,   stats.rx_kbps);
}

function applyPeerTable(allNodes) {
    const tbody = document.getElementById('peer-tbody');
    const rows  = [];
    let anyLink = false;

    for (const [nodeName, info] of Object.entries(allNodes)) {
        const peers = info.stats?.peers ?? [];
        const now   = Date.now();
        for (const p of peers) {
            const ageSec = ((now - p.last_seen_ms) / 1000).toFixed(0);
            const fresh  = (now - p.last_seen_ms) < 5000;
            if (fresh) anyLink = true;
            rows.push(`<tr class="${fresh?'peer-fresh':'peer-stale'}">
                <td>${nodeName}</td>
                <td class="peer-id">${p.node_id}</td>
                <td>${typeof p.rssi_dbm==='number'?p.rssi_dbm.toFixed(1):'--'} dBm</td>
                <td>${typeof p.snr_db==='number'?p.snr_db.toFixed(1):'--'} dB</td>
                <td>${p.frames_rx}</td>
                <td>${ageSec}s ago</td>
            </tr>`);
        }
    }

    tbody.innerHTML = rows.length
        ? rows.join('')
        : '<tr><td colspan="6" class="no-peers">No peers detected yet</td></tr>';

    // RF link indicator
    const rfStatus = document.getElementById('rf-status');
    const rfLink   = document.getElementById('rf-link');
    if (rfStatus) {
        rfStatus.textContent = anyLink ? 'Link active' : 'No link';
        if (rfLink) rfLink.className = 'rf-link ' + (anyLink ? 'link-active' : '');
    }
}

// ── WebSocket ────────────────────────────────────────────────────────────
let ws = null;
function connectWS() {
    ws = new WebSocket(`ws://${location.host}/ws`);
    ws.onmessage = evt => {
        try {
            const msg = JSON.parse(evt.data);  // { node1: {running, stats}, node2: ... }
            const an  = activeNode();
            for (const [name, info] of Object.entries(msg)) {
                applyNodeStatus(name, info);
                if (name === an && info.stats) applyStats(info.stats);
            }
            applyPeerTable(msg);
        } catch(_) {}
    };
    ws.onclose = () => setTimeout(connectWS, 2000);
}
connectWS();

// ── Load config for selected node ─────────────────────────────────────────
function loadConfig() {
    fetch(`/api/config?node=${activeNode()}`).then(r=>r.json()).then(cfg => {
        document.getElementById('ctrl-mode').value    = cfg.mode    ?? 'bridge';
        document.getElementById('ctrl-freq-tx').value = cfg.freq_tx_mhz ?? 434;
        document.getElementById('ctrl-freq-rx').value = cfg.freq_rx_mhz ?? 439;
        document.getElementById('ctrl-bw').value      = cfg.bw_mhz  ?? 10;
        document.getElementById('ctrl-mod').value     = cfg.modulation ?? 'AUTO';
        document.getElementById('ctrl-atten').value   = cfg.tx_atten_db ?? 10;
        document.getElementById('ctrl-atten-val').textContent = cfg.tx_atten_db ?? 10;
        document.getElementById('ctrl-encrypt').checked = !!cfg.encrypt;
        document.getElementById('ctrl-aes').value     = cfg.aes_key_hex ?? '';
    });
}
loadConfig();

function getFormConfig() {
    return {
        mode:        document.getElementById('ctrl-mode').value,
        freq_tx_mhz: parseFloat(document.getElementById('ctrl-freq-tx').value),
        freq_rx_mhz: parseFloat(document.getElementById('ctrl-freq-rx').value),
        bw_mhz:      parseInt(document.getElementById('ctrl-bw').value),
        modulation:  document.getElementById('ctrl-mod').value,
        tx_atten_db: parseInt(document.getElementById('ctrl-atten').value),
        encrypt:     document.getElementById('ctrl-encrypt').checked,
        aes_key_hex: document.getElementById('ctrl-aes').value,
    };
}

// ── Per-node control buttons ──────────────────────────────────────────────
function startNode(name) {
    fetch(`/api/start?node=${name}`, {method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})
        .then(r=>r.json()).then(d=>console.log(`start ${name}:`, d));
}
function stopNode(name) {
    fetch(`/api/stop?node=${name}`, {method:'POST'}).then(r=>r.json()).then(d=>console.log(`stop ${name}:`, d));
}
function startAll() {
    fetch('/api/start_all', {method:'POST'}).then(r=>r.json()).then(d=>console.log('start_all:', d));
}
function stopAll() {
    fetch('/api/stop_all', {method:'POST'}).then(r=>r.json()).then(d=>console.log('stop_all:', d));
}

// ── Control panel actions (active node) ───────────────────────────────────
function startDaemon() {
    const cfg = getFormConfig();
    fetch(`/api/start?node=${activeNode()}`, {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({mode: cfg.mode})
    }).then(r=>r.json()).then(d=>console.log('start:', d));
}
function stopDaemon() {
    fetch(`/api/stop?node=${activeNode()}`, {method:'POST'})
        .then(r=>r.json()).then(d=>console.log('stop:', d));
}
function liveTune() {
    const cfg = getFormConfig();
    fetch(`/api/ctrl?node=${activeNode()}`, {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({atten:cfg.tx_atten_db, freq_tx:cfg.freq_tx_mhz*1e6, freq_rx:cfg.freq_rx_mhz*1e6, mod:cfg.modulation})
    }).then(r=>r.json()).then(d=>console.log('tune:', d));
}
function saveConfig() {
    fetch(`/api/config?node=${activeNode()}`, {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify(getFormConfig())
    }).then(r=>r.json()).then(d=>console.log('save:', d));
}
function runScan() {
    const node = activeNode();
    fetch(`/api/start?node=${node}`, {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({mode:'scan'})
    }).then(() => {
        setTimeout(() => {
            fetch(`/api/scan?node=${node}`).then(r=>r.json()).then(results => {
                scanChart.data.labels = results.map(r=>r.freq_mhz.toFixed(1));
                scanChart.data.datasets[0].data = results.map(r=>r.power_dbm);
                scanChart.update();
            });
        }, 8000);
    });
}

// ── Helpers ───────────────────────────────────────────────────────────────
function formatUptime(s) {
    const h=Math.floor(s/3600), m=Math.floor((s%3600)/60), ss=s%60;
    return h>0?`${h}h${m}m`:m>0?`${m}m${ss}s`:`${ss}s`;
}

function generateConstellation(mod, n) {
    const pts=[], j=()=>(Math.random()-.5)*.24;
    if (mod<=1) {
        const sym=[{x:-1,y:0},{x:1,y:0}];
        for(let i=0;i<n;i++){const s=sym[i%2];pts.push({x:s.x+j(),y:s.y+j()});}
    } else if (mod===2) {
        const sym=[{x:.7,y:.7},{x:-.7,y:.7},{x:-.7,y:-.7},{x:.7,y:-.7}];
        for(let i=0;i<n;i++){const s=sym[i%4];pts.push({x:s.x+j(),y:s.y+j()});}
    } else if (mod===3) {
        for(let i=0;i<n;i++){const I=(Math.floor(Math.random()*4)*2/3-1)*1.1,Q=(Math.floor(Math.random()*4)*2/3-1)*1.1;pts.push({x:I+j(),y:Q+j()});}
    } else {
        for(let i=0;i<n;i++){const I=(Math.floor(Math.random()*8)*2/7-1)*1.2,Q=(Math.floor(Math.random()*8)*2/7-1)*1.2;pts.push({x:I+j(),y:Q+j()});}
    }
    return pts;
}
