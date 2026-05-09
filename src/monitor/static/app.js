'use strict';
// ── Chart.js defaults ──────────────────────────────────────────────────────
Chart.defaults.color = '#6b7280';
Chart.defaults.borderColor = '#2a2d3e';
Chart.defaults.font.family = "'Segoe UI', system-ui, sans-serif";

const WINDOW = 300;  // max data points in rolling charts (~60s at 5Hz)
const ACCENT = '#4f8ef7';
const GREEN  = '#22d07a';
const RED    = '#f75f5f';
const YELLOW = '#f7c948';

function makeLineChart(id, labels, datasets) {
    return new Chart(document.getElementById(id), {
        type: 'line',
        data: { labels, datasets },
        options: {
            animation: false,
            responsive: true,
            maintainAspectRatio: true,
            plugins: { legend: { display: datasets.length > 1 } },
            scales: {
                x: { display: false },
                y: { grid: { color: '#2a2d3e' } }
            }
        }
    });
}

// ── Signal chart (RSSI + SNR) ─────────────────────────────────────────────
const signalChart = makeLineChart('chart-signal', [], [
    { label: 'RSSI dBm', data: [], borderColor: RED,    borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: false },
    { label: 'SNR dB',   data: [], borderColor: GREEN,  borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: false },
]);

// ── Throughput chart ──────────────────────────────────────────────────────
const tputChart = makeLineChart('chart-tput', [], [
    { label: 'TX kbps', data: [], borderColor: ACCENT, borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: 'origin' },
    { label: 'RX kbps', data: [], borderColor: GREEN,  borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: 'origin' },
]);

// ── Spectrum chart ─────────────────────────────────────────────────────────
const specChart = new Chart(document.getElementById('chart-spectrum'), {
    type: 'bar',
    data: { labels: Array.from({length:256}, (_,i) => i), datasets: [{
        label: 'Power dB',
        data: new Array(256).fill(-100),
        backgroundColor: ACCENT + '99',
        borderColor: ACCENT,
        borderWidth: 0,
        barPercentage: 1, categoryPercentage: 1,
    }]},
    options: {
        animation: false, responsive: true, maintainAspectRatio: true,
        plugins: { legend: { display: false } },
        scales: {
            x: { display: false },
            y: { min: -120, max: 0, grid: { color: '#2a2d3e' } }
        }
    }
});

// ── IQ Constellation ──────────────────────────────────────────────────────
const iqChart = new Chart(document.getElementById('chart-iq'), {
    type: 'scatter',
    data: { datasets: [{
        label: 'IQ',
        data: [],
        backgroundColor: ACCENT + 'cc',
        pointRadius: 2,
    }]},
    options: {
        animation: false, responsive: true, maintainAspectRatio: true,
        plugins: { legend: { display: false } },
        scales: {
            x: { min: -2, max: 2, grid: { color: '#2a2d3e' } },
            y: { min: -2, max: 2, grid: { color: '#2a2d3e' } },
        }
    }
});

// ── FEC chart ─────────────────────────────────────────────────────────────
const fecChart = makeLineChart('chart-fec', [], [
    { label: 'FEC corrected', data: [], borderColor: YELLOW, borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: false },
]);

// ── Scan chart ────────────────────────────────────────────────────────────
const scanChart = new Chart(document.getElementById('chart-scan'), {
    type: 'bar',
    data: { labels: [], datasets: [{ label: 'Power dBm', data: [],
        backgroundColor: GREEN + '88', borderColor: GREEN, borderWidth: 1 }]},
    options: {
        animation: false, responsive: true, maintainAspectRatio: true,
        plugins: { legend: { display: false } },
        scales: {
            y: { min: -120, max: 0, grid: { color: '#2a2d3e' } }
        }
    }
});

// ── Rolling append ─────────────────────────────────────────────────────────
function push(chart, ...values) {
    chart.data.labels.push('');
    values.forEach((v, i) => chart.data.datasets[i].data.push(v));
    if (chart.data.labels.length > WINDOW) {
        chart.data.labels.shift();
        values.forEach((_, i) => chart.data.datasets[i].data.shift());
    }
    chart.update('none');
}

// ── MOD code → label ─────────────────────────────────────────────────────
const MOD_LABELS = {0:'--', 1:'BPSK', 2:'QPSK', 3:'16QAM', 4:'64QAM'};

// ── Update UI ──────────────────────────────────────────────────────────────
function applyStats(stats) {
    const g = id => document.getElementById(id);
    const fmt = (v, d=1) => (typeof v === 'number') ? v.toFixed(d) : '--';

    g('s-rssi').textContent     = fmt(stats.rssi_dbm);
    g('s-snr').textContent      = fmt(stats.snr_db);
    g('s-tx-kbps').textContent  = fmt(stats.tx_kbps);
    g('s-rx-kbps').textContent  = fmt(stats.rx_kbps);
    g('s-frames-tx').textContent = stats.frames_tx ?? '--';
    g('s-frames-rx').textContent = stats.frames_rx_good ?? '--';
    g('s-dropped').textContent   = stats.dropped ?? '--';
    g('s-mod').textContent       = MOD_LABELS[stats.cur_mod] ?? '--';
    g('uptime').textContent      = formatUptime(stats.uptime_s ?? 0);
    if (typeof stats.temp_c === 'number')
        g('temp').textContent    = stats.temp_c.toFixed(1) + '°C';

    const corr  = stats.fec_corrected  ?? 0;
    const uncorr = stats.fec_uncorrectable ?? 0;
    const total = corr + uncorr;
    g('fec-corrected').textContent = corr;
    g('fec-uncorr').textContent    = uncorr;
    g('fec-rate').textContent      = total > 0 ? (corr/total*100).toFixed(1)+'%' : '0%';

    push(signalChart, stats.rssi_dbm ?? -100, stats.snr_db ?? 0);
    push(tputChart,   stats.tx_kbps ?? 0,     stats.rx_kbps ?? 0);
    push(fecChart,    corr);

    if (Array.isArray(stats.spectrum) && stats.spectrum.length === 256) {
        specChart.data.datasets[0].data = stats.spectrum;
        specChart.update('none');
    }

    // Fake IQ constellation from mod code (visual demo without real symbol feed)
    const npts = 64;
    const mod = stats.cur_mod ?? 2;
    const pts = generateConstellation(mod, npts);
    iqChart.data.datasets[0].data = pts;
    iqChart.update('none');
}

function generateConstellation(mod, n) {
    const pts = [];
    const jitter = 0.12;
    const j = () => (Math.random() - 0.5) * jitter * 2;

    if (mod <= 1) {         // BPSK
        const sym = [{x:-1,y:0},{x:1,y:0}];
        for (let i=0;i<n;i++) { const s=sym[i%2]; pts.push({x:s.x+j(),y:s.y+j()}); }
    } else if (mod === 2) { // QPSK
        const sym=[{x:.7,y:.7},{x:-.7,y:.7},{x:-.7,y:-.7},{x:.7,y:-.7}];
        for (let i=0;i<n;i++) { const s=sym[i%4]; pts.push({x:s.x+j(),y:s.y+j()}); }
    } else if (mod === 3) { // 16QAM
        for (let i=0;i<n;i++) {
            const I = (Math.floor(Math.random()*4)*2/3 - 1)*1.1;
            const Q = (Math.floor(Math.random()*4)*2/3 - 1)*1.1;
            pts.push({x:I+j(),y:Q+j()});
        }
    } else {               // 64QAM
        for (let i=0;i<n;i++) {
            const I = (Math.floor(Math.random()*8)*2/7 - 1)*1.2;
            const Q = (Math.floor(Math.random()*8)*2/7 - 1)*1.2;
            pts.push({x:I+j(),y:Q+j()});
        }
    }
    return pts;
}

function formatUptime(s) {
    const h = Math.floor(s/3600);
    const m = Math.floor((s%3600)/60);
    const ss = s % 60;
    return h>0 ? `${h}h${m}m` : m>0 ? `${m}m${ss}s` : `${ss}s`;
}

// ── WebSocket ─────────────────────────────────────────────────────────────
let ws = null;
function connectWS() {
    const url = `ws://${location.host}/ws`;
    ws = new WebSocket(url);
    ws.onmessage = evt => {
        try {
            const msg = JSON.parse(evt.data);
            const badge = document.getElementById('status-badge');
            badge.textContent = msg.running ? 'RUNNING' : 'STOPPED';
            badge.className = 'badge ' + (msg.running ? 'running' : 'stopped');
            if (msg.stats) applyStats(msg.stats);
        } catch (_) {}
    };
    ws.onclose = () => setTimeout(connectWS, 2000);
}
connectWS();

// ── Load initial config into controls ────────────────────────────────────
fetch('/api/config').then(r=>r.json()).then(cfg => {
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

// ── Control actions ──────────────────────────────────────────────────────
function getFormConfig() {
    return {
        mode:         document.getElementById('ctrl-mode').value,
        freq_tx_mhz:  parseFloat(document.getElementById('ctrl-freq-tx').value),
        freq_rx_mhz:  parseFloat(document.getElementById('ctrl-freq-rx').value),
        bw_mhz:       parseInt(document.getElementById('ctrl-bw').value),
        modulation:   document.getElementById('ctrl-mod').value,
        tx_atten_db:  parseInt(document.getElementById('ctrl-atten').value),
        encrypt:      document.getElementById('ctrl-encrypt').checked,
        aes_key_hex:  document.getElementById('ctrl-aes').value,
    };
}

function startDaemon() {
    const cfg = getFormConfig();
    fetch('/api/start', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify({mode: cfg.mode})
    }).then(r=>r.json()).then(d=>console.log('start:', d));
}

function stopDaemon() {
    fetch('/api/stop', {method:'POST'}).then(r=>r.json()).then(d=>console.log('stop:', d));
}

function liveTune() {
    const cfg = getFormConfig();
    fetch('/api/ctrl', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify({
            atten:    cfg.tx_atten_db,
            freq_tx:  cfg.freq_tx_mhz * 1e6,
            freq_rx:  cfg.freq_rx_mhz * 1e6,
            mod:      cfg.modulation,
        })
    }).then(r=>r.json()).then(d=>console.log('tune:', d));
}

function saveConfig() {
    fetch('/api/config', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify(getFormConfig())
    }).then(r=>r.json()).then(d=>console.log('save:', d));
}

function runScan() {
    fetch('/api/start', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({mode:'scan'})
    }).then(() => {
        setTimeout(() => {
            fetch('/api/scan').then(r=>r.json()).then(results => {
                scanChart.data.labels   = results.map(r => r.freq_mhz.toFixed(1));
                scanChart.data.datasets[0].data = results.map(r => r.power_dbm);
                scanChart.update();
            });
        }, 8000);
    });
}
