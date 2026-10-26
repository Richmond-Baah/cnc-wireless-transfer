// CNC Wireless Transfer - Client Application Logic
document.addEventListener('DOMContentLoaded', () => {
    applyStrings();

    // ─── Theme: System / Light / Dark, remembered per browser ───────────
    const THEME_KEY = 'cnc.theme';
    function setTheme(choice, save) {
        if (!['system', 'light', 'dark'].includes(choice)) choice = 'system';
        document.documentElement.dataset.theme = choice;
        document.querySelectorAll('[data-theme-choice]').forEach(b =>
            b.setAttribute('aria-pressed', String(b.dataset.themeChoice === choice)));
        if (save) {
            try { localStorage.setItem(THEME_KEY, choice); } catch (e) { /* storage blocked */ }
        }
    }
    setTheme(document.documentElement.dataset.theme, false);
    document.addEventListener('click', (e) => {
        const b = e.target.closest('[data-theme-choice]');
        if (b) setTheme(b.dataset.themeChoice, true);
    });

    // ═══════════════════════════════════════════════════════════════════
    // LOGIN PAGE
    // ═══════════════════════════════════════════════════════════════════
    const loginForm = document.getElementById('loginForm');
    if (loginForm) {
        if (sessionStorage.getItem('token')) {
            window.location.href = 'upload.html';
            return;
        }
        loginForm.addEventListener('submit', async (e) => {
            e.preventDefault();
            const username = document.getElementById('username').value;
            const password = document.getElementById('password').value;
            const msg = document.getElementById('loginError');
            try {
                const res = await fetch('/login', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ username, password })
                });
                if (res.ok) {
                    const data = await res.json();
                    sessionStorage.setItem('token', data.token);
                    window.location.href = 'upload.html';
                } else {
                    msg.textContent = t('login.bad');
                    msg.hidden = false;
                }
            } catch (err) {
                msg.textContent = t('login.unreachable');
                msg.hidden = false;
            }
        });
        return;
    }

    // ═══════════════════════════════════════════════════════════════════
    // MAIN PAGE
    // ═══════════════════════════════════════════════════════════════════
    const token = sessionStorage.getItem('token');
    if (!token) {
        window.location.href = 'index.html';
        return;
    }

    const $ = (id) => document.getElementById(id);
    const esc = (s) => String(s).replace(/[&<>"']/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    const authHeaders = (h) => Object.assign({ 'Authorization': 'Bearer ' + token }, h || {});

    // Single source of truth for accepted machine-program extensions
    const VALID_EXTS = ['.nc', '.gcode', '.tap', '.cnc', '.gc', '.ngc', '.pcn', '.plt', '.hnc', '.txt', '.crv', '.sbp', '.dxf', '.svg', '.xcn', '.din', '.cut', '.iso'];

    // Binary units. fmtBytes(4404019) = "4.2 MB"; fmtPair(1258291, 4404019) = "1.2 of 4.2 MB"
    const UNITS = [['unit.b', 1, 0], ['unit.kb', 1024, 1], ['unit.mb', 1048576, 1], ['unit.gb', 1073741824, 2]];
    const unitFor = (b) => UNITS.reduce((u, x) => (b >= x[1] ? x : u), UNITS[0]);
    const num = (v, u) => {
        const x = v / u[1];
        return u[2] && x < 100 ? x.toFixed(u[2]) : String(Math.round(x));
    };
    const fmtBytes = (bytes) => { const u = unitFor(bytes || 0); return t(u[0], { n: num(bytes || 0, u) }); };
    const fmtPair = (a, b) => { const u = unitFor(b || 0); return t(u[0], { n: t('unit.of', { a: num(a, u), b: num(b, u) }) }); };

    function lastSeenText(s) {
        if (typeof s !== 'number' || s < 0) return t('mc.lastSeen.never');
        if (s < 60) return t('mc.lastSeen.now');
        if (s < 3600) return t('mc.lastSeen.min', { n: Math.round(s / 60) });
        return t('mc.lastSeen.h', { n: Math.round(s / 3600) });
    }

    async function errorText(res) {
        try {
            return (await res.text()) || ('HTTP ' + res.status);
        } catch (e) {
            return 'HTTP ' + res.status;
        }
    }

    // ─── State ──────────────────────────────────────────────────────────
    const RX_STORAGE_KEY = 'cnc.selectedRx';
    let selectedRx = null;
    try { selectedRx = localStorage.getItem(RX_STORAGE_KEY); } catch (e) { /* storage blocked */ }
    let receivers = [];         // /receivers: paired machines with cached heartbeat status
    let status = null;          // /status for the selected machine (+ transmitter fields)
    let lastFree = null, lastTotal = 0; // selected drive space, for the pre-upload size check
    let lastWifiMode = 'AP';    // AP = hotspot only, STA = also on a router
    let lastStaIp = '';
    let uploading = false;
    let currentFile = null;
    let statusInterval = null;

    const selectedReceiver = () => receivers.find(r => r.id === selectedRx) || null;
    // Adds the selected receiver to request headers
    const rxHeaders = (h) => authHeaders(Object.assign({}, h || {}, selectedRx ? { 'X-Rx-Id': selectedRx } : {}));

    function rememberRx(id) {
        selectedRx = id;
        try {
            if (id) localStorage.setItem(RX_STORAGE_KEY, id);
            else localStorage.removeItem(RX_STORAGE_KEY);
        } catch (e) { /* storage blocked */ }
    }

    // Selected machine as shown: cached list entry, refreshed by /status
    function selectedView() {
        const r = selectedReceiver();
        if (!r) return null;
        const v = Object.assign({}, r);
        if (status && status.rx_id === r.id) {
            ['online', 'state', 'fw', 'storage_type', 'total_bytes', 'free_bytes', 'card_error', 'busy', 'last_seen_s']
                .forEach(k => { if (status[k] !== undefined) v[k] = status[k]; });
        }
        return v;
    }

    // [pill class, text] for a machine
    function pillFor(r) {
        if (!r || !r.online || r.state === 'OFFLINE') return ['pill', t('pill.offline')];
        if (r.card_error || r.state === 'CARD_ERROR') return ['pill pill-err', t('pill.cardError')];
        if (r.state === 'RECEIVING') return ['pill pill-busy', t('pill.receiving')];
        if (r.busy) return ['pill pill-busy', t('pill.job')];
        return ['pill pill-ok', t('pill.ready')];
    }
    const storageLabel = (type) => t('storage.' + (type || 'unknown'));

    // ─── Header ─────────────────────────────────────────────────────────
    function logoutExpired() {
        sessionStorage.removeItem('token');
        window.location.href = 'index.html';
    }
    $('logoutBtn').addEventListener('click', logoutExpired);

    const netChip = $('netChip');
    let netSsid = '', netClients = null, netMax = 15;
    function renderNetChip() {
        if (!netSsid || netClients === null) return;
        const [before, after] = t('hdr.network', { ssid: '\u0001', n: netClients, max: netMax }).split('\u0001');
        const el = $('netChipText');
        const b = document.createElement('b');
        b.textContent = netSsid;
        el.textContent = '';
        el.append(before, b, after || '');
        netChip.hidden = false;
    }
    fetch('/config', { headers: authHeaders() })
        .then(r => (r.ok ? r.json() : null))
        .then(d => { if (d && d.ap_ssid) { netSsid = d.ap_ssid; renderNetChip(); } })
        .catch(() => { /* chip stays hidden */ });

    // ─── Machines (left column) ─────────────────────────────────────────
    const machineList = $('machineList');

    function renderMachines() {
        if (!receivers.length) {
            machineList.innerHTML = '<p class="muted small">' + esc(t('mc.none')) + '</p>';
            return;
        }
        machineList.innerHTML = receivers.map(r => {
            const [pcls, ptxt] = pillFor(r);
            const total = r.total_bytes || 0;
            const used = total ? Math.min(100, Math.max(0, (1 - (r.free_bytes || 0) / total) * 100)) : 0;
            const storage = total
                ? t('mc.storage', { type: storageLabel(r.storage_type), free: fmtBytes(r.free_bytes), total: fmtBytes(total) })
                : storageLabel(r.storage_type);
            const sel = r.id === selectedRx;
            return `<button type="button" class="mcard${sel ? ' selected' : ''}${r.online ? '' : ' offline'}"
                    data-id="${esc(r.id)}" aria-pressed="${sel}"${uploading ? ' disabled' : ''}>
                <span class="mcard-top"><span class="mcard-name">${esc(r.name)}</span><span class="${pcls}">${esc(ptxt)}</span></span>
                ${!r.online
                    ? `<span class="muted small">${esc(lastSeenText(r.last_seen_s))}</span>`
                    : (r.card_error || r.state === 'CARD_ERROR')
                        ? `<span class="small err-text">${esc(t('state.CARD_ERROR'))}</span>`
                        : `<span class="bar bar-sm" aria-hidden="true"><span class="bar-fill" style="width:${used.toFixed(1)}%"></span></span>
                           <span class="muted small">${esc(storage)}</span>`}
            </button>`;
        }).join('');
    }

    machineList.addEventListener('click', (e) => {
        const card = e.target.closest('.mcard');
        if (card && !card.disabled) selectRx(card.dataset.id);
    });

    function selectRx(id) {
        if (uploading || id === selectedRx) return;
        rememberRx(id);
        status = null;
        lastFree = null;
        lastTotal = 0;
        clearMsg();
        renderAll();
        loadReceiverFiles();
        pollStatus();
    }

    async function loadReceivers() {
        try {
            const res = await fetch('/receivers', { headers: authHeaders() });
            if (res.status === 401) return logoutExpired();
            if (!res.ok) return;
            const d = await res.json();
            receivers = d.receivers || [];
            if (!selectedReceiver() && !uploading) {
                const prev = selectedRx;
                rememberRx(receivers.length ? receivers[0].id : null);
                if (selectedRx !== prev) {
                    status = null;
                    lastFree = null;
                    loadReceiverFiles();
                    pollStatus();
                }
            }
            renderAll();
            renderPending(d.pending || []);
            renderAddWindow(d.add_window_s || 0);
        } catch (e) { /* transmitter unreachable: the next poll retries */ }
    }

    async function postReceiverAction(action, body) {
        const res = await fetch('/receivers/' + action, {
            method: 'POST',
            headers: authHeaders({ 'Content-Type': 'application/json' }),
            body: JSON.stringify(body)
        });
        if (!res.ok) return { error: await errorText(res) };
        try { return { message: (await res.json()).message || '' }; } catch (e) { return { message: '' }; }
    }

    // ─── New devices + Add machine (replaced by the dialog in stage D) ──
    const newDeviceBanner = $('newDeviceBanner');
    let pendingKey = '';
    function renderPending(pending) {
        const key = pending.map(p => p.id).join(',');
        if (key === pendingKey) return; // keep names being typed
        pendingKey = key;
        if (!pending.length) {
            newDeviceBanner.style.display = 'none';
            newDeviceBanner.innerHTML = '';
            return;
        }
        newDeviceBanner.innerHTML = '<h2 style="margin-bottom: 4px;">New device found</h2>' +
            '<p class="muted small">Check the ID matches the label on the device before approving.</p>' + pending.map(p => `
            <div class="new-device-row">
                <span class="muted"><strong class="mono">${esc(p.id)}</strong> (${p.radio ? 'by radio' : esc(p.ip)})${p.repair ? ' – was reset, approve again' : ''}</span>
                <input type="text" maxlength="31" value="${esc(p.name)}" data-id="${esc(p.id)}" aria-label="Name for ${esc(p.id)}">
                <button type="button" class="btn btn-primary approve-rx" data-id="${esc(p.id)}">Approve</button>
            </div>`).join('');
        newDeviceBanner.style.display = 'block';
    }
    newDeviceBanner.addEventListener('click', async (e) => {
        const btn = e.target.closest('.approve-rx');
        if (!btn) return;
        const id = btn.dataset.id;
        const input = newDeviceBanner.querySelector(`input[data-id="${CSS.escape(id)}"]`);
        const name = input ? input.value.trim() : '';
        if (!name) return;
        btn.disabled = true;
        const { error } = await postReceiverAction('approve', { id, name });
        btn.disabled = false;
        if (error) { setMsg({ type: 'error', title: t('msg.error.pairing'), why: error }); return; }
        pendingKey = '';
        if (!selectedReceiver()) rememberRx(id);
        await loadReceivers();
        loadReceiverFiles();
        pollStatus();
    });

    const addMachineBtn = $('addMachineBtn');
    const addMachineInfo = $('addMachineInfo');
    let addWindowEnd = 0;
    let addWindowTimer = null;
    function renderAddWindow(seconds) {
        if (seconds > 0) addWindowEnd = Date.now() + seconds * 1000;
        else if (addWindowEnd && Date.now() < addWindowEnd - 5000) addWindowEnd = 0; // closed early (restart)
        const tick = () => {
            const left = Math.max(0, Math.round((addWindowEnd - Date.now()) / 1000));
            if (!left) {
                addMachineInfo.hidden = true;
                addMachineBtn.disabled = false;
                clearInterval(addWindowTimer);
                addWindowTimer = null;
                return;
            }
            addMachineInfo.textContent = 'Looking for new machines (' + left + ' s). Power on the receiver; it appears above as a new device.';
            addMachineInfo.hidden = false;
            addMachineBtn.disabled = true;
        };
        tick();
        if (addWindowEnd && !addWindowTimer) addWindowTimer = setInterval(tick, 1000);
    }
    addMachineBtn.addEventListener('click', async () => {
        addMachineBtn.disabled = true;
        try {
            const res = await fetch('/receivers/add', { method: 'POST', headers: authHeaders() });
            if (!res.ok) {
                setMsg({ type: 'error', title: t('msg.error.add'), why: await errorText(res) });
                addMachineBtn.disabled = false;
                return;
            }
            renderAddWindow((await res.json()).seconds || 120);
        } catch (e) {
            addMachineBtn.disabled = false;
        }
    });

    $('pairFileBtn').addEventListener('click', async () => {
        const name = (prompt('Machine name for the new receiver:') || '').trim();
        if (!name) return;
        if (!confirm('The pairing file contains this hotspot\'s Wi-Fi password and is valid for 24 hours.\n\n' +
            'Copy it to the root of the receiver\'s drive as pair.cfg, restart the receiver, ' +
            'and delete any other copies.\n\nDownload it?')) return;
        const r = await downloadPairFile(name);
        if (r.error) setMsg({ type: 'error', title: t('msg.error.pairFile'), why: r.error });
    });
    async function downloadPairFile(name) {
        try {
            const res = await fetch('/receivers/pair_file', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ name, now: Math.floor(Date.now() / 1000) })
            });
            if (!res.ok) return { error: await errorText(res) };
            const url = URL.createObjectURL(await res.blob());
            const a = document.createElement('a');
            a.href = url;
            a.download = 'pair.cfg';
            document.body.appendChild(a);
            a.click();
            a.remove();
            setTimeout(() => URL.revokeObjectURL(url), 1000);
            return {};
        } catch (e) {
            return { error: e.message };
        }
    }

    // ─── Transfer card (right) ──────────────────────────────────────────
    const fileInput = $('fileInput');
    const fileZone = $('fileZone');
    const uploadBtn = $('uploadBtn');
    const cancelBtn = $('cancelBtn');
    const msgBox = $('msgBox');
    const progressBox = $('progressBox');
    $('fileTypes').textContent = t('file.types', { list: VALID_EXTS.join(', ') });
    fileInput.accept = VALID_EXTS.join(',');

    // Same rule as the receiver's /prepare check: size + max(64 KB, 1% of drive)
    const spaceNeeded = (size, total) => size + Math.max(64 * 1024, Math.floor((total || 0) / 100));
    const fileFits = () => !currentFile || lastFree === null || lastFree >= spaceNeeded(currentFile.size, lastTotal);

    function renderTransfer() {
        const v = selectedView();
        const pill = $('txPill');
        if (!v) {
            $('txName').textContent = t('tx.noMachine');
            $('txSub').textContent = t('tx.noMachine.why');
            pill.hidden = true;
        } else {
            $('txName').textContent = v.name;
            const st = v.online ? (v.state || 'UNKNOWN') : 'OFFLINE';
            const parts = [t('state.' + st)];
            if (v.storage_type) parts.push(storageLabel(v.storage_type));
            if (v.fw) parts.push(t('tx.fw', { v: v.fw }));
            $('txSub').textContent = parts.join(' · ');
            const [pcls, ptxt] = pillFor(v);
            pill.className = pcls;
            pill.textContent = ptxt;
            pill.hidden = false;
        }

        // File row
        $('dropEmpty').hidden = !!currentFile;
        $('dropFile').hidden = !currentFile;
        if (currentFile) {
            $('fileName').textContent = currentFile.name;
            $('fileSize').textContent = fmtBytes(currentFile.size);
            const fit = $('fileFit');
            fit.hidden = lastFree === null;
            fit.textContent = t(fileFits() ? 'file.fits' : 'file.tooBig', { free: fmtBytes(lastFree) });
            fit.className = fileFits() ? 'ok-text' : 'err-text';
            const dot = currentFile.name.lastIndexOf('.');
            const ext = dot >= 0 ? currentFile.name.substring(dot).toLowerCase() : '';
            $('fileOdd').hidden = VALID_EXTS.includes(ext);
            $('fileOdd').textContent = t('file.oddType', { ext: ext || currentFile.name });
        }
        fileZone.classList.toggle('disabled', uploading);
        $('chooseFileBtn').disabled = uploading;
        $('chooseAnotherBtn').disabled = uploading;

        uploadBtn.textContent = t('tx.send', { name: v ? v.name : '…' });
        uploadBtn.disabled = uploading || !currentFile || !v || !v.online || !fileFits() ||
            v.state === 'CARD_ERROR' || !!v.card_error || waitTimer !== null;
        cancelBtn.hidden = !uploading && !currentFile;
        renderMsgArea();
    }

    function renderAll() {
        renderMachines();
        renderTransfer();
        renderFilesHeading();
    }

    // ─── Messages ───────────────────────────────────────────────────────
    // Shown in place of the progress area (never pop-ups): icon + title +
    // one-line why + actions. Explicit ones come from actions/outcomes;
    // otherwise the selected machine's state may produce one (offline, card
    // error, file too big). Re-rendered only when the content changes, so a
    // half-typed FORMAT survives the status polls.
    const ICONS = {
        ok: '<path d="M22 11.1V12a10 10 0 1 1-5.9-9.1"/><path d="M22 4 12 14l-3-3"/>',
        err: '<circle cx="12" cy="12" r="10"/><path d="M12 8v4M12 16h.01"/>',
        busy: '<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>',
        offline: '<path d="M2 2l20 20M8.5 16.4a5 5 0 0 1 7 0M5 12.6a10 10 0 0 1 5.2-2.7M19 12.6a10 10 0 0 0-2.2-1.6M2 8.8a15 15 0 0 1 4.2-2.6M10.7 5a15 15 0 0 1 11.3 3.8"/><path d="M12 20h.01"/>',
        card: '<path d="M6 2h9l5 5v13a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z"/><path d="M8 6v3M11 6v3M14 6v3"/><path d="M12 13v3M12 19h.01"/>',
        big: '<path d="M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>'
    };
    const STAGE_KEYS = { Checksum: 'stage.checksum', Starting: 'stage.starting', Uploading: 'stage.uploading',
                         Verifying: 'stage.verifying', 'Reconnecting drive': 'stage.reconnecting' };
    let explicitMsg = null;
    let msgKey = '';
    let waitTimer = null;
    let lastSent = null; // {file, rxId} for "Send it again to"

    function setMsg(m) {
        if (!m || m.type !== 'waiting') stopWaiting();
        explicitMsg = m;
        renderTransfer(); // also re-renders the message area
    }
    function clearMsg() { setMsg(null); }

    function fmtDuration(secs) {
        const s = Math.max(1, Math.round(secs));
        return s < 60 ? t('dur.s', { n: s }) : t('dur.ms', { m: Math.floor(s / 60), s: s % 60 });
    }

    function derivedMsg(v) {
        if (!v) return null;
        if (!v.online) return { type: 'offline' };
        if (v.card_error || v.state === 'CARD_ERROR') return { type: 'card' };
        if (currentFile && !fileFits()) {
            const need = spaceNeeded(currentFile.size, lastTotal);
            return { type: 'big', file: currentFile.name, need, free: lastFree };
        }
        return null;
    }

    const btn = (act, label, cls) => `<button type="button" class="btn ${cls || ''}" data-msg-act="${act}">${esc(label)}</button>`;

    // -> {key, kind, icon, title, why, extra, actions}
    function buildMsg(m, v) {
        const name = m.name || (v ? v.name : '');
        switch (m.type) {
        case 'busy':
            return { key: 'busy', kind: 'busy', icon: 'busy', title: t('msg.busy.title', { name }), why: t('msg.busy.why'),
                actions: btn('wait', t('msg.busy.wait'), 'btn-primary') + btn('force', t('msg.busy.force')) };
        case 'waiting':
            return { key: 'waiting', kind: 'busy', icon: 'busy', title: t('msg.wait.title', { name }),
                why: t('msg.wait.why', { file: m.file }), actions: btn('stopwait', t('msg.wait.stop')) };
        case 'big': {
            const missing = Math.max(0, m.need - m.free);
            const freePct = m.need ? Math.max(0, Math.min(100, (m.free / m.need) * 100)) : 0;
            return { key: 'big:' + m.file + ':' + m.free + ':' + m.need, kind: 'err', icon: 'big',
                title: t('msg.big.title', { file: m.file, name }),
                why: t('msg.big.why', { need: fmtBytes(m.need), free: fmtBytes(m.free), missing: fmtBytes(missing) }),
                extra: `<div class="split-bar" aria-hidden="true"><span class="split-free" style="width:${freePct.toFixed(1)}%"></span><span class="split-miss"></span></div>
                    <div class="split-legend small"><span><i class="dot dot-free"></i>${esc(t('msg.big.free', { free: fmtBytes(m.free) }))}</span>
                    <span><i class="dot dot-miss"></i>${esc(t('msg.big.missing', { missing: fmtBytes(missing) }))}</span></div>`,
                actions: btn('pickdelete', t('msg.big.pick')) };
        }
        case 'card':
            return { key: 'card:' + (v ? v.id : ''), kind: 'err', icon: 'card', title: t('msg.card.title', { name }), why: t('msg.card.why'),
                extra: `<label class="label small" for="formatWord">${esc(t('msg.card.type'))}</label>`,
                actions: `<input type="text" id="formatWord" class="confirm-word" autocomplete="off" autocapitalize="characters">` +
                    `<button type="button" class="btn btn-danger" data-msg-act="format" disabled>${esc(t('msg.card.btn'))}</button>` };
        case 'formatting':
            return { key: 'formatting', kind: 'busy', icon: 'busy', title: t('msg.card.busy', { name }), why: t('msg.card.busyWhy') };
        case 'formatted':
            return { key: 'formatted', kind: 'ok', icon: 'ok', title: t('msg.card.done', { name }), why: t('msg.card.doneWhy') };
        case 'done': {
            const others = receivers.filter(r => r.id !== m.rxId && r.online && !(r.card_error || r.state === 'CARD_ERROR'));
            const chips = others.map(r => {
                const busy = r.busy || r.state === 'RECEIVING';
                return `<button type="button" class="chip" data-msg-act="again" data-id="${esc(r.id)}"${busy ? ' disabled' : ''}>${esc(r.name)}${busy ? ' · ' + esc(t('pill.job')) : ''}</button>`;
            }).join('');
            return { key: 'done:' + others.map(r => r.id + (r.busy ? 'b' : '')).join(','), kind: 'ok', icon: 'ok',
                title: t('msg.done.title', { file: m.file, name }),
                why: t('msg.done.why', { size: fmtBytes(m.size), time: fmtDuration(m.secs) }) + (m.slow ? ' ' + t('msg.done.slow') : ''),
                extra: chips ? `<div class="again"><span class="small muted">${esc(t('msg.done.again'))}</span><div class="chips">${chips}</div></div>` : '' };
        }
        case 'offline':
            return { key: 'offline:' + (v ? v.id + ':' + Math.round((v.last_seen_s || 0) / 60) : ''), kind: 'info', icon: 'offline',
                title: t('msg.offline.title', { name }), why: t('msg.offline.why', { seen: lastSeenText(v ? v.last_seen_s : -1) }) };
        case 'failed': {
            const title = m.notStarted ? t('msg.fail.notStarted')
                : t(m.cancelled ? 'msg.fail.cancelled' : 'msg.fail.title', { pct: m.pct });
            const details = `<p class="msg-why">${esc(t('msg.fail.safe'))}</p>` + (m.showDetails && m.details
                ? `<pre class="details">${esc(m.details)}</pre>` : '');
            return { key: 'failed:' + !!m.showDetails, kind: 'err', icon: 'err', title, why: m.reason, extra: details,
                actions: (m.retry === false ? '' : btn('retry', t('msg.fail.retry'), 'btn-primary')) +
                    (m.details ? btn('details', t(m.showDetails ? 'msg.fail.hide' : 'msg.fail.details'), 'btn-ghost') : '') };
        }
        default: // generic {title, why}
            return { key: 'error:' + m.title + m.why, kind: m.kind || 'err', icon: m.kind === 'ok' ? 'ok' : 'err', title: m.title, why: m.why };
        }
    }

    function renderMsgArea() {
        progressBox.hidden = !uploading;
        // A 507 from the receiver goes away once enough was deleted
        if (explicitMsg && explicitMsg.type === 'big' && (!currentFile || (lastFree !== null && lastFree >= explicitMsg.need))) {
            explicitMsg = null;
        } else if (explicitMsg && explicitMsg.type === 'big') {
            explicitMsg.free = lastFree;
        }
        const v = selectedView();
        const m = uploading ? null : (explicitMsg || derivedMsg(v));
        // Messages with their own actions replace Send; card errors replace the drop zone too
        const type = m ? m.type : '';
        uploadBtn.hidden = ['busy', 'waiting', 'card', 'formatting', 'failed'].includes(type) ||
            (type === 'done' && !currentFile);
        fileZone.hidden = type === 'card' || type === 'formatting';
        if (!m) {
            msgBox.hidden = true;
            msgBox.innerHTML = '';
            msgKey = '';
            return;
        }
        const b = buildMsg(m, v);
        const key = m.type + '|' + b.key + '|' + (v ? v.id + v.name : '');
        if (key !== msgKey) {
            msgKey = key;
            msgBox.className = 'msg msg-' + b.kind;
            msgBox.innerHTML = `<svg class="i msg-icon" viewBox="0 0 24 24" aria-hidden="true">${ICONS[b.icon] || ICONS.err}</svg>
                <div class="msg-body"><div class="msg-title">${esc(b.title)}</div>
                ${b.why ? `<p class="msg-why">${esc(b.why)}</p>` : ''}${b.extra || ''}
                ${b.actions ? `<div class="msg-actions">${b.actions}</div>` : ''}</div>`;
        }
        msgBox.hidden = false;
    }

    msgBox.addEventListener('input', (e) => {
        if (e.target.id === 'formatWord') {
            const go = msgBox.querySelector('[data-msg-act="format"]');
            if (go) go.disabled = e.target.value.trim() !== 'FORMAT';
        }
    });
    msgBox.addEventListener('click', (e) => {
        const a = e.target.closest('[data-msg-act]');
        if (!a || a.disabled) return;
        const target = selectedReceiver();
        switch (a.dataset.msgAct) {
        case 'wait': if (target) startWaiting(target); break;
        case 'force': startUpload(true); break;
        case 'stopwait': clearMsg(); break;
        case 'retry': clearMsg(); startUpload(false); break;
        case 'details':
            explicitMsg = Object.assign({}, explicitMsg, { showDetails: !explicitMsg.showDetails });
            renderMsgArea();
            break;
        case 'pickdelete': {
            const card = $('filesCard');
            card.scrollIntoView({ behavior: 'smooth', block: 'start' });
            const first = card.querySelector('.file-select, #selectAllFiles');
            if (first) setTimeout(() => first.focus({ preventScroll: true }), 300);
            break;
        }
        case 'format': {
            const word = $('formatWord');
            if (word && word.value.trim() === 'FORMAT') formatCard();
            break;
        }
        case 'again':
            if (lastSent && !uploading) {
                const id = a.dataset.id;
                selectRx(id);
                currentFile = lastSent.file;
                renderTransfer();
                startUpload(false);
            }
            break;
        }
    });

    // Busy: re-check every 10 s and send once the machine stops reading
    function startWaiting(target) {
        stopWaiting();
        const file = currentFile;
        if (!file) return;
        setMsg({ type: 'waiting', name: target.name, file: file.name });
        waitTimer = setInterval(async () => {
            try {
                const res = await fetch('/status', { headers: authHeaders({ 'X-Rx-Id': target.id }) });
                if (!res.ok) return;
                const d = await res.json();
                const idle = d.online !== false && d.state !== 'OFFLINE' && d.state !== 'RECEIVING' && !d.busy;
                if (idle && selectedRx === target.id && currentFile === file && !uploading) {
                    stopWaiting();
                    explicitMsg = null;
                    startUpload(false);
                }
            } catch (e) { /* keep waiting */ }
        }, 10000);
    }
    function stopWaiting() {
        clearInterval(waitTimer);
        waitTimer = null;
    }

    // Card error: format from the message (typed FORMAT, checked server-side)
    async function formatCard() {
        const r = selectedReceiver();
        if (!r) return;
        setMsg({ type: 'formatting', name: r.name });
        try {
            const res = await fetch('/receiver_format', {
                method: 'POST',
                headers: rxHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ confirm: 'FORMAT' })
            });
            setMsg(res.ok ? { type: 'formatted', name: r.name }
                : { type: 'error', title: t('msg.card.failed'), why: await errorText(res) });
        } catch (e) {
            setMsg({ type: 'error', title: t('msg.card.failed'), why: t('msg.fail.network') });
        }
        pollStatus();
        loadReceiverFiles();
    }

    // File selection: drop zone, Choose file, Choose another file
    function pickFile() { if (!uploading) fileInput.click(); }
    $('chooseFileBtn').addEventListener('click', pickFile);
    $('chooseAnotherBtn').addEventListener('click', pickFile);
    fileInput.addEventListener('change', () => {
        if (fileInput.files.length) handleFileSelect(fileInput.files[0]);
        fileInput.value = '';
    });
    fileZone.addEventListener('dragover', (e) => {
        e.preventDefault();
        if (!uploading) fileZone.classList.add('is-active');
    });
    fileZone.addEventListener('dragleave', () => fileZone.classList.remove('is-active'));
    fileZone.addEventListener('drop', (e) => {
        e.preventDefault();
        fileZone.classList.remove('is-active');
        if (!uploading && e.dataTransfer.files.length) handleFileSelect(e.dataTransfer.files[0]);
    });

    function handleFileSelect(file) {
        currentFile = file;
        clearMsg();
        renderTransfer();
    }

    uploadBtn.addEventListener('click', () => startUpload(false));
    cancelBtn.addEventListener('click', () => {
        if (uploading) {
            // Takes effect between chunks; startUpload then calls /upload_cancel
            cancelRequested = true;
            cancelBtn.disabled = true;
            $('progressText').textContent = t('tx.cancelling');
            return;
        }
        currentFile = null;
        clearMsg();
        renderTransfer();
    });

    // ─── Status polling ─────────────────────────────────────────────────
    const pollStatus = async () => {
        try {
            const res = await fetch('/status', { headers: rxHeaders() });
            if (res.status === 401) return logoutExpired();
            if (!res.ok) return;
            const data = await res.json();
            status = data;
            if (data.rx_id === selectedRx && typeof data.free_bytes === 'number' && data.state !== 'OFFLINE') {
                lastFree = data.free_bytes;
                lastTotal = data.total_bytes || 0;
            }
            if (data.wifi_mode) lastWifiMode = data.wifi_mode;
            if (data.sta && data.sta.ip) lastStaIp = data.sta.ip;
            if (typeof data.ap_clients === 'number') {
                netClients = data.ap_clients;
                netMax = data.ap_max_clients || 15;
                renderNetChip();
            }
            if (!uploading) renderTransfer();
        } catch (e) { /* transmitter unreachable: the next poll retries */ }
    };
    function startStatusPolling() {
        clearInterval(statusInterval);
        statusInterval = setInterval(pollStatus, 3000);
    }

    // ─── Files card ─────────────────────────────────────────────────────
    const fileTable = $('fileTable');
    const deleteSelectedBtn = $('deleteSelectedBtn');
    const filesConfirm = $('filesConfirm');
    const filesNote = $('filesNote');

    function renderFilesHeading() {
        const r = selectedReceiver();
        $('filesHeading').textContent = r ? t('files.heading', { name: r.name }) : t('files.headingNone');
        $('wipeBtn').hidden = !r;
    }
    function filesFeedback(text, kind) {
        filesNote.textContent = text || '';
        filesNote.className = 'files-feedback small ' + (kind === 'err' ? 'err-text' : 'ok-text');
        filesNote.hidden = !text;
    }

    const loadReceiverFiles = async () => {
        renderFilesHeading();
        closeFilesConfirm();
        $('filesCached').hidden = true;
        const rxAtStart = selectedRx;
        const show = (html) => { if (selectedRx === rxAtStart) { fileTable.innerHTML = html; updateDeleteSelected(); } };
        if (!selectedRx) {
            show('<p class="muted small">' + esc(t('files.pick')) + '</p>');
            return;
        }
        show('<p class="muted small">' + esc(t('files.loading')) + '</p>');
        try {
            const res = await fetch('/receiver_list', { headers: rxHeaders() });
            if (!res.ok) {
                show('<p class="err-text small">' + esc(t('files.error')) + '</p>');
                return;
            }
            const data = await res.json();
            if (selectedRx !== rxAtStart) return;
            // cached = the machine owns the drive; list is the receiver's last snapshot
            $('filesCached').hidden = !data.cached;
            const r = selectedReceiver();
            if (data.reason === 'receiver_offline') {
                show('<p class="muted small">' + esc(t('files.offline', { name: r ? r.name : '' })) + '</p>');
                return;
            }
            if (!data.files || !data.files.length) {
                show('<p class="muted small">' + esc(t('files.empty')) + '</p>');
                return;
            }
            const rows = data.files.map(f => {
                const n = esc(f.name);
                if (f.type === 'dir') {
                    return `<tr class="is-dir"><td class="cell-check"></td><td class="mono">${n}/</td><td class="num"></td></tr>`;
                }
                return `<tr><td class="cell-check"><label class="check"><input type="checkbox" class="file-select" data-name="${n}" aria-label="${esc(t('files.select', { name: f.name }))}"></label></td>
                    <td class="mono">${n}</td><td class="num muted">${esc(fmtBytes(f.size))}</td></tr>`;
            }).join('');
            show(`<table class="ftable"><thead><tr>
                <th class="cell-check"><label class="check"><input type="checkbox" id="selectAllFiles" aria-label="${esc(t('files.selectAll'))}"></label></th>
                <th>${esc(t('files.colName'))}</th><th class="num">${esc(t('files.colSize'))}</th></tr></thead>
                <tbody>${rows}</tbody></table>` +
                (data.truncated ? '<p class="muted small">' + esc(t('files.truncated')) + '</p>' : ''));
        } catch (e) {
            show('<p class="err-text small">' + esc(t('files.error')) + '</p>');
        }
    };
    $('refreshFilesBtn').addEventListener('click', () => { filesFeedback(''); loadReceiverFiles(); });

    const selectedNames = () => Array.from(fileTable.querySelectorAll('.file-select:checked')).map(cb => cb.dataset.name);
    function updateDeleteSelected() {
        const n = selectedNames().length;
        deleteSelectedBtn.hidden = n === 0;
        deleteSelectedBtn.textContent = t('files.deleteN', { n });
        fileTable.querySelectorAll('.file-select').forEach(cb => cb.closest('tr').classList.toggle('sel', cb.checked));
        const all = $('selectAllFiles');
        if (all) {
            const boxes = fileTable.querySelectorAll('.file-select').length;
            all.checked = boxes > 0 && n === boxes;
            all.indeterminate = n > 0 && n < boxes;
        }
    }
    fileTable.addEventListener('change', (e) => {
        if (e.target.id === 'selectAllFiles') {
            fileTable.querySelectorAll('.file-select').forEach(cb => { cb.checked = e.target.checked; });
        }
        updateDeleteSelected();
    });

    // Inline confirmations in the files card (never pop-ups)
    function openFilesConfirm(html, handler) {
        filesConfirm.innerHTML = html;
        filesConfirm.hidden = false;
        filesConfirm.onclick = handler;
        filesConfirm.oninput = handler;
    }
    function closeFilesConfirm() {
        filesConfirm.hidden = true;
        filesConfirm.innerHTML = '';
        filesConfirm.onclick = null;
        filesConfirm.oninput = null;
    }
    const confirmButtons = (yes, no) => `<div class="confirm-actions">
        <button type="button" class="btn btn-danger" data-act="yes">${esc(yes)}</button>
        <button type="button" class="btn" data-act="no">${esc(no)}</button></div>`;

    deleteSelectedBtn.addEventListener('click', () => confirmDelete(selectedNames(), false));

    function confirmDelete(names, busy) {
        if (!names.length) return;
        const r = selectedReceiver();
        const rxName = r ? r.name : '';
        const shown = names.slice(0, 8).map(n => `<li class="mono">${esc(n)}</li>`).join('') +
            (names.length > 8 ? `<li class="muted">+${names.length - 8}</li>` : '');
        openFilesConfirm(`<p class="confirm-title">${esc(busy ? t('files.confirmBusy', { name: rxName }) : t('files.confirmDelete', { n: names.length, name: rxName }))}</p>
            <ul class="confirm-list">${shown}</ul>` +
            confirmButtons(busy ? t('files.deleteAnyway') : t('files.delete'), t('files.keep')), (e) => {
            const b = e.target.closest('button');
            if (!b || e.type !== 'click') return;
            closeFilesConfirm();
            if (b.dataset.act === 'yes') deleteFiles(names, busy);
        });
    }

    async function deleteFiles(names, force) {
        const headers = rxHeaders({ 'Content-Type': 'application/json' });
        if (force) headers['X-Force'] = '1';
        deleteSelectedBtn.disabled = true;
        try {
            const res = await fetch('/receiver_delete', { method: 'POST', headers, body: JSON.stringify({ files: names }) });
            deleteSelectedBtn.disabled = false;
            if (res.status === 423) {
                confirmDelete(names, true);
                return;
            }
            if (res.ok) {
                const d = await res.json();
                let text = t('files.deleted', { n: (d.deleted || []).length });
                if (d.skipped && d.skipped.length) {
                    text += ' ' + t('files.skipped', { list: d.skipped.map(s => s.name + ' (' + s.reason + ')').join(', ') });
                }
                if (typeof d.free_bytes === 'number') {
                    lastFree = d.free_bytes;
                    if (d.total_bytes) lastTotal = d.total_bytes;
                }
                filesFeedback(text, d.skipped && d.skipped.length ? 'err' : 'ok');
            } else {
                filesFeedback(t('files.deleteFailed', { why: await errorText(res) }), 'err');
            }
        } catch (e) {
            deleteSelectedBtn.disabled = false;
            filesFeedback(t('files.netError'), 'err');
        }
        await loadReceiverFiles();
        pollStatus(); // free space -> re-checks whether the selected file fits
        renderTransfer();
    }

    // Erase all files (typed WIPE, checked server-side)
    $('wipeBtn').addEventListener('click', () => confirmWipe(false));
    function confirmWipe(busy) {
        const r = selectedReceiver();
        if (!r) return;
        if (busy) {
            openFilesConfirm(`<p class="confirm-title">${esc(t('files.wipeBusy', { name: r.name }))}</p>` +
                confirmButtons(t('files.wipeAnyway'), t('files.keep')), (e) => {
                const b = e.target.closest('button');
                if (!b || e.type !== 'click') return;
                closeFilesConfirm();
                if (b.dataset.act === 'yes') wipe(true);
            });
            return;
        }
        openFilesConfirm(`<p class="confirm-title">${esc(t('files.wipeConfirm', { name: r.name }))}</p>
            <div class="confirm-actions">
                <input type="text" class="confirm-word" autocomplete="off" autocapitalize="characters" aria-label="WIPE">
                <button type="button" class="btn btn-danger" data-act="yes" disabled>${esc(t('files.wipeBtn'))}</button>
                <button type="button" class="btn" data-act="no">${esc(t('common.cancel'))}</button>
            </div>`, (e) => {
            const word = filesConfirm.querySelector('.confirm-word');
            const ok = word.value.trim() === 'WIPE';
            filesConfirm.querySelector('[data-act="yes"]').disabled = !ok;
            if (e.type !== 'click') return;
            const b = e.target.closest('button');
            if (!b || (b.dataset.act === 'yes' && !ok)) return;
            closeFilesConfirm();
            if (b.dataset.act === 'yes') wipe(false);
        });
        filesConfirm.querySelector('.confirm-word').focus();
    }
    async function wipe(force) {
        const r = selectedReceiver();
        const headers = rxHeaders({ 'Content-Type': 'application/json' });
        if (force) headers['X-Force'] = '1';
        try {
            const res = await fetch('/receiver_clear', { method: 'POST', headers, body: JSON.stringify({ confirm: 'WIPE' }) });
            if (res.status === 423) { confirmWipe(true); return; }
            filesFeedback(res.ok ? t('files.wiped', { name: r ? r.name : '' }) : t('files.wipeFailed', { why: await errorText(res) }),
                res.ok ? 'ok' : 'err');
        } catch (e) {
            filesFeedback(t('files.netError'), 'err');
        }
        await loadReceiverFiles();
        pollStatus();
    }

    // ─── Upload ─────────────────────────────────────────────────────────
    // Browser-driven chunked upload: /upload_begin, /upload_chunk x N, /upload_finish.
    // Each chunk request returns only after the receiver has acked it, so the
    // progress bar shows bytes actually stored, not bytes handed to the network.
    const CHUNK_SIZE = 32 * 1024;
    const CHUNK_RETRIES = 3;
    let cancelRequested = false;
    let lastPct = 0;

    class UploadError extends Error {
        constructor(stage, reason, status) {
            super(reason);
            this.stage = stage;
            this.status = status;
        }
    }

    async function apiPost(path, headers, body, timeoutMs) {
        const ctrl = timeoutMs ? new AbortController() : null;
        const timer = ctrl ? setTimeout(() => ctrl.abort(), timeoutMs) : null;
        let res;
        try {
            res = await fetch(path, {
                method: 'POST',
                headers: authHeaders(headers),
                body,
                signal: ctrl ? ctrl.signal : undefined
            });
        } finally {
            if (timer) clearTimeout(timer);
        }
        if (res.status === 401) {
            sessionStorage.removeItem('token');
            setTimeout(() => { window.location.href = 'index.html'; }, 2000);
        }
        return res;
    }

    async function sendChunk(session, index, blob) {
        let lastErr = '';
        for (let attempt = 1; attempt <= CHUNK_RETRIES; attempt++) {
            try {
                const res = await apiPost('/upload_chunk', {
                    'X-Session': session,
                    'X-Chunk-Index': String(index),
                    'Content-Type': 'application/octet-stream'
                }, blob);
                if (res.ok) return (await res.json()).acked;
                lastErr = await errorText(res);
                // 4xx (expired session, out of order, too large, auth) won't succeed on retry
                if (res.status >= 400 && res.status < 500) throw new UploadError('Uploading', lastErr, res.status);
            } catch (e) {
                if (e instanceof UploadError) throw e;
                lastErr = 'network error: ' + e.message;
            }
            if (attempt < CHUNK_RETRIES) await new Promise(r => setTimeout(r, 500 * attempt));
        }
        throw new UploadError('Uploading', 'chunk ' + index + ' failed after ' + CHUNK_RETRIES + ' attempts: ' + lastErr);
    }

    // The receiver stays FILE_READY until its USB drive is reconnected
    async function waitForDriveReconnect(timeoutMs) {
        const t0 = Date.now();
        while (Date.now() - t0 < timeoutMs) {
            await new Promise(r => setTimeout(r, 700));
            try {
                const res = await fetch('/status', { headers: rxHeaders() });
                if (res.ok) {
                    const d = await res.json();
                    // RECEIVING may still be the transmitter's cached value from mid-upload
                    if (d.state && d.state !== 'FILE_READY' && d.state !== 'RECEIVING') return d.state;
                }
            } catch (e) { /* keep waiting */ }
        }
        return null;
    }

    // step: 0 Upload, 1 Verify, 2 Reconnect drive, 3 Done
    function setProgress(percent, text, step) {
        const p = Math.min(100, Math.max(0, Math.round(percent)));
        lastPct = p;
        $('progressFill').style.width = p + '%';
        $('progressPct').textContent = p + '%';
        $('progressText').textContent = text;
        $('stepper').querySelectorAll('li').forEach((li, i) => {
            li.className = i < step ? 'done' : i === step ? 'active' : '';
            if (i === step) li.setAttribute('aria-current', 'step');
            else li.removeAttribute('aria-current');
        });
    }

    async function startUpload(force) {
        const target = selectedReceiver();
        if (!currentFile || !target) return;
        force = (force === true); // click handlers may pass an Event
        const file = currentFile;
        cancelRequested = false;
        cancelBtn.disabled = false;
        clearMsg();
        uploading = true;
        clearInterval(statusInterval);
        renderAll(); // lock the selection while uploading

        let stage = 'Checksum';
        let session = null;
        let outcome = null; // message shown in place of the progress area
        const tStart = performance.now();
        try {
            setProgress(0, t('tx.checking'), 0);
            const md5 = await calculateMD5(file);
            if (cancelRequested) throw new UploadError(stage, 'cancelled');

            stage = 'Starting';
            setProgress(0, t('tx.preparing', { name: target.name }), 0);
            const beginHeaders = { 'Content-Type': 'application/json', 'X-Rx-Id': target.id };
            if (force) beginHeaders['X-Force'] = '1';
            let res;
            try {
                // Transmitter gives up on the receiver after 10 s (504); this is the backstop
                res = await apiPost('/upload_begin', beginHeaders,
                    JSON.stringify({ name: file.name, size: file.size, md5 }), 12000);
            } catch (e) {
                if (e.name !== 'AbortError') throw e;
                throw new UploadError(stage, t('msg.fail.timeout', { name: target.name }));
            }
            if (res.status === 423) {
                outcome = { type: 'busy', name: target.name };
                return;
            }
            if (res.status === 507) {
                // Receiver re-checked the real drive: keep the file selected so
                // Send re-enables once enough is deleted below
                const raw = await errorText(res);
                let d = {};
                try { d = JSON.parse(raw); } catch (x) { /* plain text */ }
                if (typeof d.free_bytes === 'number') {
                    lastFree = d.free_bytes;
                    outcome = { type: 'big', name: target.name, file: file.name, free: d.free_bytes,
                                need: d.needed_bytes || spaceNeeded(file.size, lastTotal) };
                } else {
                    outcome = { type: 'failed', notStarted: true, reason: raw, retry: false };
                }
                return;
            }
            if (res.status === 409) {
                // "Busy: uploading to <name>"
                outcome = { type: 'failed', notStarted: true, reason: await errorText(res) };
                return;
            }
            if (!res.ok) throw new UploadError(stage, await errorText(res), res.status);
            session = (await res.json()).session;

            stage = 'Uploading';
            const total = file.size;
            let acked = 0;
            for (let index = 0, offset = 0; offset < total; index++, offset += CHUNK_SIZE) {
                if (cancelRequested) throw new UploadError(stage, 'cancelled');
                acked = await sendChunk(session, index, file.slice(offset, offset + CHUNK_SIZE));
                setProgress(total ? (acked / total) * 100 : 100, t('tx.uploading', { progress: fmtPair(acked, total) }), 0);
            }
            if (cancelRequested) throw new UploadError(stage, 'cancelled');

            stage = 'Verifying';
            cancelBtn.disabled = true; // past the point of a clean cancel
            setProgress(100, t('tx.verifying'), 1);
            const fin = await apiPost('/upload_finish', { 'X-Session': session }, '');
            session = null; // the transmitter ends the session either way
            if (!fin.ok) throw new UploadError(stage, await errorText(fin), fin.status);
            const secs = (performance.now() - tStart) / 1000;

            stage = 'Reconnecting drive';
            setProgress(100, t('tx.reconnecting'), 2);
            const state = await waitForDriveReconnect(10000);
            setProgress(100, t('tx.done'), 3);
            outcome = { type: 'done', name: target.name, rxId: target.id, file: file.name, size: file.size, secs, slow: !state };
            lastSent = { file, rxId: target.id };
            currentFile = null;
        } catch (e) {
            const err = e instanceof UploadError ? e : new UploadError(stage, e.message || String(e));
            if (session) {
                try { await apiPost('/upload_cancel', { 'X-Session': session }, ''); } catch (x) { /* best effort */ }
            }
            const network = !(e instanceof UploadError) || /network error/.test(err.message);
            outcome = {
                type: 'failed',
                pct: lastPct,
                cancelled: err.message === 'cancelled',
                reason: err.message === 'cancelled' ? '' : network ? t('msg.fail.network') : err.message,
                details: err.message === 'cancelled' ? '' : t('msg.fail.detailLine', {
                    stage: t(STAGE_KEYS[err.stage] || 'stage.uploading'),
                    status: err.status ? 'HTTP ' + err.status : '-'
                }) + '\n' + err.message
            };
        } finally {
            uploading = false;
            cancelBtn.disabled = false;
            explicitMsg = outcome;
            renderAll();
            loadReceiverFiles();
            startStatusPolling();
            setTimeout(pollStatus, 1000);
        }
    }

    // ─── Settings modal (replaced by the Settings page in stage D) ──────
    const settingsModal = $('settingsModal');
    const settingsForm = $('settingsForm');
    const settingsFeedback = $('settingsFeedback');
    let loadedApSsid = '';

    const receiverSettingsList = $('receiverSettingsList');
    function renderReceiverSettings() {
        if (!receivers.length) {
            receiverSettingsList.innerHTML = '<p class="text-muted">No receivers paired.</p>';
            return;
        }
        receiverSettingsList.innerHTML = receivers.map(r => `
            <div class="rs-row">
                <input type="text" maxlength="31" value="${esc(r.name)}" data-id="${esc(r.id)}" aria-label="Name of ${esc(r.id)}">
                <button type="button" class="btn btn-secondary btn-sm rs-rename" data-id="${esc(r.id)}">Rename</button>
                <button type="button" class="btn btn-danger btn-sm rs-unpair" data-id="${esc(r.id)}">Unpair</button>
                <div class="rs-id">${esc(r.id)} · ${r.online ? 'online, ' + esc(r.ip) : 'offline'}</div>
            </div>`).join('');
    }
    receiverSettingsList.addEventListener('click', async (e) => {
        const btn = e.target.closest('button');
        if (!btn) return;
        const id = btn.dataset.id;
        const r = receivers.find(x => x.id === id);
        let result;
        if (btn.classList.contains('rs-rename')) {
            const input = receiverSettingsList.querySelector(`input[data-id="${CSS.escape(id)}"]`);
            const name = input ? input.value.trim() : '';
            if (!name) return;
            result = await postReceiverAction('rename', { id, name });
        } else if (btn.classList.contains('rs-unpair')) {
            if (!confirm('Unpair "' + (r ? r.name : id) + '"? It must be approved again before it can be used.')) return;
            result = await postReceiverAction('unpair', { id });
        } else {
            return;
        }
        settingsFeedback.textContent = result.error ? 'Failed: ' + result.error : (result.message || 'Saved.');
        settingsFeedback.className = 'feedback-msg ' + (result.error ? 'error' : 'success');
        settingsFeedback.style.display = 'block';
        await loadReceivers();
        renderReceiverSettings();
    });

    $('recoveryBtn').addEventListener('click', async () => {
        const typed = prompt('Recovery mode: for 5 minutes the hotspot uses the FACTORY name and password ' +
            'so receivers that missed a Wi-Fi change can reconnect and update.\n' +
            'This page disconnects; rejoin the factory hotspot to watch.\nType RECOVERY to continue:');
        if (typed !== 'RECOVERY') return;
        try {
            const res = await fetch('/recovery', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ confirm: 'RECOVERY' })
            });
            settingsFeedback.textContent = res.ok
                ? 'Recovery mode on for 5 min: hotspot is now the factory network. Settings revert automatically.'
                : 'Recovery mode failed: ' + await errorText(res);
            settingsFeedback.className = 'feedback-msg ' + (res.ok ? 'success' : 'error');
        } catch (e) {
            settingsFeedback.textContent = 'Recovery mode started; this connection dropped as expected.';
            settingsFeedback.className = 'feedback-msg success';
        }
        settingsFeedback.style.display = 'block';
    });

    $('configBtn').addEventListener('click', () => {
        settingsModal.classList.add('show');
        renderReceiverSettings();
        settingsFeedback.style.display = 'none';
        fetch('/config', { headers: authHeaders() })
            .then(r => {
                if (!r.ok) throw new Error('HTTP ' + r.status);
                return r.json();
            })
            .then(data => {
                $('wifiMode').value = data.wifi_mode || 'AP';
                $('apSsid').value = data.ap_ssid || '';
                loadedApSsid = data.ap_ssid || '';
                $('apPass').value = '';
                $('staSsid').value = data.sta_ssid || '';
                $('staPass').value = '';
                $('authUser').value = data.auth_user || '';
                $('authPass').value = '';
            }).catch(e => {
                settingsFeedback.textContent = 'Could not load current settings (' + e.message + ').';
                settingsFeedback.className = 'feedback-msg error';
                settingsFeedback.style.display = 'block';
            });
    });
    $('closeSettingsBtn').addEventListener('click', () => {
        settingsModal.classList.remove('show');
        settingsFeedback.style.display = 'none';
    });
    settingsForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const data = Object.fromEntries(new FormData(settingsForm).entries());
        const apChanged = (data.ap_pass && data.ap_pass.length > 0) || (data.ap_ssid && data.ap_ssid !== loadedApSsid);
        const offline = receivers.filter(r => !r.online).map(r => r.name);
        if (apChanged && offline.length &&
            !confirm('These receivers are offline and will not get the new Wi-Fi settings:\n\n' +
                offline.join('\n') + '\n\nThey reconnect later via Recovery mode. Save anyway?')) {
            return;
        }
        try {
            const res = await fetch('/config', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify(data)
            });
            if (res.ok) {
                let text = 'Settings saved. Device restarting...';
                if (data.wifi_mode === 'STA') {
                    text += ' On your router, open http://cnc-transfer.local or http://' +
                        (lastStaIp || '<STA IP shown in status after restart>') + '.';
                }
                settingsFeedback.textContent = text;
                settingsFeedback.className = 'feedback-msg success';
                settingsFeedback.style.display = 'block';
                sessionStorage.removeItem('token');
                setTimeout(() => { window.location.href = 'index.html'; }, data.wifi_mode === 'STA' ? 20000 : 5000);
            } else {
                settingsFeedback.textContent = 'Failed to save settings: ' + await errorText(res);
                settingsFeedback.className = 'feedback-msg error';
                settingsFeedback.style.display = 'block';
            }
        } catch (err) {
            settingsFeedback.textContent = 'Network error saving settings.';
            settingsFeedback.className = 'feedback-msg error';
            settingsFeedback.style.display = 'block';
        }
    });

    // ─── Chunked MD5 (via spark-md5) ────────────────────────────────────
    // Read the file in 2 MB slices so we don't load the whole G-code into RAM.
    // Output is a lowercase hex digest that matches the receiver's mbedtls result.
    const MD5_CHUNK_SIZE = 2 * 1024 * 1024;

    function hashSlice(spark, blob) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = (e) => {
                try {
                    spark.append(e.target.result);
                    resolve();
                } catch (err) { reject(err); }
            };
            reader.onerror = () => reject(new Error('File read error during MD5 calculation'));
            reader.readAsArrayBuffer(blob);
        });
    }

    async function calculateMD5(file) {
        if (typeof SparkMD5 === 'undefined') throw new Error('SparkMD5 library not loaded. Please refresh the page.');
        const spark = new SparkMD5.ArrayBuffer();
        for (let offset = 0; offset < file.size; offset += MD5_CHUNK_SIZE) {
            await hashSlice(spark, file.slice(offset, offset + MD5_CHUNK_SIZE));
        }
        return spark.end();
    }

    // ─── Start ──────────────────────────────────────────────────────────
    renderAll();
    loadReceivers();
    setInterval(loadReceivers, 4000);
    pollStatus();
    startStatusPolling();
    loadReceiverFiles();
});
