/* RF editor: preserve the full config; only replace fields shown in the form. */
const RF_FIELDS = [
  ['FREQUENCY', 'TX frequency (MHz)', 325, 3800, 0.000001, 1e6],
  ['RX_FREQUENCY', 'RX frequency (MHz)', 325, 3800, 0.000001, 1e6],
  ['SAMPLE_RATE', 'Sample rate (MS/s)', 2.083333, 17.28, 0.000001, 1e6],
  ['TX_RF_BANDWIDTH', 'TX bandwidth (MHz)', 0.2, 20, 0.000001, 1e6],
  ['RX_RF_BANDWIDTH', 'RX bandwidth (MHz)', 0.2, 20, 0.000001, 1e6],
  ['TX_ATTENUATION_DB', 'TX attenuation (dB; higher means less power)', 0, 89.75, 0.25, 1],
  ['RX_GAIN_DB', 'Manual RX gain (dB)', 0, 73, 1, 1],
  ['STATS_S', 'Device telemetry interval (seconds)', 1, 3600, 1, 1],
  ['PROBE_INTERVAL_S', 'Probe interval (seconds; 0 disables)', 0, 3600, 1, 1],
];
const configDrafts = {};
function parseConfig(text) {
  const result = {};
  for (const line of text.split('\n')) {
    const match = /^([A-Z_0-9]+)=(.*)$/.exec(line);
    if (match) result[match[1]] = match[2];
  }
  return result;
}
function patchConfig(original, changes) {
  const remaining = {...changes};
  const lines = original.trimEnd().split('\n').map(line => {
    const match = /^([A-Z_0-9]+)=/.exec(line);
    if (!match || !Object.hasOwn(changes, match[1])) return line;
    const name = match[1]; delete remaining[name];
    return changes[name] == null ? null : `${name}=${changes[name]}`;
  }).filter(line => line !== null);
  for (const [name, value] of Object.entries(remaining)) if (value != null) lines.push(`${name}=${value}`);
  return lines.join('\n') + '\n';
}
function validateRF(values) {
  for (const [name, label, min, max, step, scale] of RF_FIELDS) {
    if (name === 'RX_GAIN_DB' && values.RX_GAIN_MODE !== 'manual') continue;
    if ((name === 'STATS_S' || name === 'PROBE_INTERVAL_S') && values[name] == null) continue;
    const raw = values[name], value = Number(raw) / scale;
    if (raw == null || raw === '' || !Number.isFinite(value) || value < min || value > max || Math.abs(value / step - Math.round(value / step)) > 0.0001) throw Error(`${label}: use ${min}–${max}, step ${step}.`);
  }
  if (!['manual', 'slow_attack', 'fast_attack', 'hybrid'].includes(values.RX_GAIN_MODE)) throw Error('Invalid gain mode.');
  if (!['0', '1'].includes(values.DIFF_MODE)) throw Error('Invalid differential encoding mode.');
  if (Number(values.FREQUENCY) === Number(values.RX_FREQUENCY)) throw Error('TX and RX frequencies must differ to avoid self-interference.');
  for (const name of ['TX_RF_BANDWIDTH', 'RX_RF_BANDWIDTH']) if (Number(values[name]) > Number(values.SAMPLE_RATE)) throw Error('RF bandwidth must not exceed sample rate.');
}
function makeEditor(key) {
  const container = document.getElementById(`${key}-config-editor`);
  container.innerHTML = `<details open><summary>Radio configuration — editable controls</summary>
    <p class="sub">Production modem: QPSK. 16-QAM / 64-QAM selection requires a supported modem bitstream and is not available in this firmware.</p>
    <button type="button" class="primary" id="${key}-load-config">Load settings to edit</button>
    <form id="${key}-rf-form" hidden>
      <div class="kv">${RF_FIELDS.map(([name, label, min, max, step]) => `<label for="${key}-${name}">${label}</label><input id="${key}-${name}" type="number" min="${min}" max="${max}" step="${step}">`).join('')}
      <label for="${key}-RX_GAIN_MODE">RX gain mode</label><select id="${key}-RX_GAIN_MODE"><option>slow_attack</option><option>fast_attack</option><option>hybrid</option><option>manual</option></select>
      <label for="${key}-DIFF_MODE">Differential encoding (match peer)</label><select id="${key}-DIFF_MODE"><option value="1">Enabled</option><option value="0">Disabled</option></select></div>
      <p class="sub">Device telemetry interval controls how often the SDR writes its counters; faster browser refresh cannot create newer device samples. Changing it requires Apply. Keep TX frequency matched to the peer's RX frequency, and match sample rate and encoding on both units. Apply briefly restarts this unit's link. Driver limits can depend on frequency.</p>
      <button type="button" id="${key}-preview-config">Validate & preview changes</button>
      <pre id="${key}-config-preview"></pre>
      <button type="button" id="${key}-apply-config" disabled>Apply reviewed changes to UNIT-${key.toUpperCase()}</button>
    </form><pre id="${key}-config-result" role="status"></pre></details>`;
  const el = suffix => document.getElementById(`${key}-${suffix}`);
  const invalidate = () => { if (configDrafts[key]) configDrafts[key].candidate = null; el('apply-config').disabled = true; el('config-preview').textContent = ''; };
  const gainMode = () => { el('RX_GAIN_DB').disabled = el('RX_GAIN_MODE').value !== 'manual'; };
  el('rf-form').addEventListener('submit', e => e.preventDefault());
  el('rf-form').addEventListener('input', () => { invalidate(); gainMode(); });
  el('load-config').addEventListener('click', async () => {
    invalidate(); el('rf-form').hidden = true; el('config-result').textContent = 'Loading…';
    try {
      const unit = {...settings[key]};
      const r = await getJson(unit.url, unit.token, '/api/v1/config/raw', 15000);
      if (!r.ok || typeof r.body?.config !== 'string') throw Error(`Cannot load config (HTTP ${r.status}). Check connection and authentication.`);
      configDrafts[key] = {original: r.body.config, unit, candidate: null};
      const values = parseConfig(r.body.config);
      for (const [name, , , , , scale] of RF_FIELDS) el(name).value = values[name] == null ? '' : Number(values[name]) / scale;
      el('RX_GAIN_MODE').value = values.RX_GAIN_MODE || 'slow_attack';
      el('DIFF_MODE').value = values.DIFF_MODE || '1';
      gainMode(); el('rf-form').hidden = false;
      el('config-result').textContent = 'Configuration loaded. Other device settings will be preserved.';
    } catch (error) { el('config-result').textContent = error.message; }
  });
  el('preview-config').addEventListener('click', () => {
    invalidate();
    try {
      const draft = configDrafts[key], changes = {};
      for (const [name, , , , , scale] of RF_FIELDS) {
        const raw = el(name).value;
        changes[name] = raw === '' ? null : String(scale === 1e6 ? Math.round(Number(raw) * scale) : Number(raw));
      }
      changes.RX_GAIN_MODE = el('RX_GAIN_MODE').value;
      changes.DIFF_MODE = el('DIFF_MODE').value;
      if (changes.RX_GAIN_MODE !== 'manual') changes.RX_GAIN_DB = parseConfig(draft.original).RX_GAIN_DB ?? null;
      validateRF(changes);
      const candidate = patchConfig(draft.original, changes);
      if (new TextEncoder().encode(candidate).length > 4096) throw Error('Configuration exceeds device limit (4096 bytes).');
      const before = parseConfig(draft.original);
      const diff = Object.entries(changes).filter(([name,value]) => (before[name] ?? null) !== value).map(([name,value]) => `${name}: ${before[name] ?? '(unset)'} → ${value ?? '(unset)'}`);
      if (!diff.length) throw Error('No changes to apply.');
      const peer = snapshots[key === 'a' ? 'b' : 'a'];
      const warnings = [];
      if (!peer || Date.now() - Date.parse(peer.captured_at) > 15000) warnings.push('Peer data unavailable or stale; frequency and sample-rate compatibility unverified.');
      else {
        if (Number(changes.FREQUENCY) !== Number(peer.status.config?.rx_hz)) warnings.push('New TX frequency differs from peer RX.');
        if (Number(changes.RX_FREQUENCY) !== Number(peer.status.config?.tx_hz)) warnings.push('New RX frequency differs from peer TX.');
        if (Number(changes.SAMPLE_RATE) !== Number(peer.status.config?.sample_rate)) warnings.push('New sample rate differs from peer.');
      }
      el('config-preview').textContent = diff.join('\n') + (warnings.length ? '\n\nPeer checks:\n' + warnings.join('\n') : '') + '\n\nFull candidate:\n' + candidate;
      draft.candidate = candidate; el('apply-config').disabled = false;
      el('config-result').textContent = 'Local validation passed. Device validation runs when you apply.';
    } catch (error) { el('config-result').textContent = error.message; }
  });
  el('apply-config').addEventListener('click', async () => {
    const draft = configDrafts[key];
    if (!draft?.candidate) return;
    const candidate = draft.candidate;
    el('rf-form').querySelectorAll('input, select').forEach(e => e.disabled = true);
    el('apply-config').disabled = true;
    el('load-config').disabled = true;
    el('preview-config').disabled = true;
    try {
      if (draft.unit.url !== settings[key].url || draft.unit.token !== settings[key].token) throw Error('Connection changed; reload configuration.');
      const current = await getJson(draft.unit.url, draft.unit.token, '/api/v1/config/raw', 15000);
      if (!current.ok || current.body?.config !== draft.original) throw Error('Device configuration changed or cannot be checked. Reload before applying.');
      if (!confirm(`Apply the reviewed RF settings to ${draft.unit.name}? This restarts its link and may require matching changes on the peer.`)) return;
      el('config-result').textContent = 'Applying and verifying on device… This can take about a minute.';
      const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 75000);
      try {
        const headers = {'Content-Type': 'text/plain'};
        if (draft.unit.url.startsWith('/devices/')) headers['X-SDR-Apply'] = 'yes';
        else headers.Authorization = 'Bearer ' + draft.unit.token;
        const response = await fetch(draft.unit.url + '/api/v1/config?confirm=yes', {method: 'POST', headers, body: candidate, signal: controller.signal});
        const result = await response.json();
        const outcome = result.verified ? 'Configuration applied and verified.' : result.rolled_back ? 'Verification failed; device reports rollback. Reload and check status.' : 'Configuration was not verified. Review the device result and reload before retrying.';
        el('config-result').textContent = `${outcome}\nHTTP ${response.status}\n${JSON.stringify(result, null, 2)}`;
      } finally { clearTimeout(timer); }
    } catch (error) { el('config-result').textContent = `${error.message}\nIf apply was sent, its outcome may be unknown. Reload configuration and check live status before retrying.`; }
    finally { draft.candidate = null; el('rf-form').querySelectorAll('input, select').forEach(e => e.disabled = false); gainMode(); el('load-config').disabled = false; el('preview-config').disabled = false; }
  });
}
for (const key of ['a', 'b']) makeEditor(key);
for (const key of ['a', 'b']) {
  document.getElementById('configure' + key.toUpperCase()).addEventListener('click', () => {
    const editor = document.getElementById(`${key}-config-editor`);
    editor.querySelector('details').open = true;
    editor.scrollIntoView({behavior: 'smooth', block: 'start'});
    if (document.getElementById(`${key}-rf-form`).hidden) document.getElementById(`${key}-load-config`).click();
    else document.getElementById(`${key}-FREQUENCY`).focus();
  });
}
