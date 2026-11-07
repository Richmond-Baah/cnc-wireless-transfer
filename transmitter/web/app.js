// FileXfer - Client Application Logic
document.addEventListener('DOMContentLoaded', () => {
    applyStrings();

    // ─── Show/hide toggle on every password field ───────────────────────
    const EYE = '<svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z"/><circle cx="12" cy="12" r="3"/></svg>';
    const EYE_OFF = '<svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.1 10.1 0 0 1 12 19C5 19 1 12 1 12a18.5 18.5 0 0 1 5.06-5.94M9.9 4.24A9.1 9.1 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19M14.12 14.12a3 3 0 1 1-4.24-4.24"/><path d="M1 1l22 22"/></svg>';
    function setPwVisible(btn, show) {
        btn.previousElementSibling.type = show ? 'text' : 'password';
        btn.setAttribute('aria-pressed', String(show));
        btn.setAttribute('aria-label', t(show ? 'pw.hide' : 'pw.show'));
        btn.innerHTML = show ? EYE_OFF : EYE;
    }
    document.querySelectorAll('input[type="password"]').forEach(input => {
        const wrap = document.createElement('span');
        wrap.className = 'pw-wrap';
        input.parentNode.insertBefore(wrap, input);
        wrap.appendChild(input);
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'pw-toggle';
        wrap.appendChild(btn);
        setPwVisible(btn, false);
        btn.addEventListener('click', () => setPwVisible(btn, btn.getAttribute('aria-pressed') !== 'true'));
    });
    // Back to hidden whenever a form is submitted
    document.addEventListener('submit', (e) => {
        e.target.querySelectorAll('.pw-toggle').forEach(b => setPwVisible(b, false));
    }, true);

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
    // One page, two views: login (no token) or the app
    if (!sessionStorage.getItem('token')) {
        document.getElementById('loginPage').hidden = false;
        document.title = t('app.title.login');
        const loginForm = document.getElementById('loginForm');
        if (sessionStorage.getItem('signedOut')) {
            sessionStorage.removeItem('signedOut');
            const msg = document.getElementById('loginError');
            msg.textContent = t('login.signedOut');
            msg.hidden = false;
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
                    window.location.reload();
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
    document.getElementById('appPage').hidden = false;
    document.title = t('app.title.main');

    const $ = (id) => document.getElementById(id);
    const esc = (s) => String(s).replace(/[&<>"']/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    const authHeaders = (h) => Object.assign({ 'Authorization': 'Bearer ' + token }, h || {});

    // Any 401 (session expired, or admin login reset by BOOT): back to login.
    // /status and /receivers are polled with the token, so this fires within a poll.
    const rawFetch = window.fetch.bind(window);
    let signingOut = false;
    window.fetch = async (...args) => {
        const res = await rawFetch(...args);
        if (res.status === 401 && !signingOut) {
            signingOut = true;
            sessionStorage.removeItem('token');
            sessionStorage.setItem('signedOut', '1');
            window.location.href = '/';
        }
        return res;
    };

    // Known machine-file extensions: a hint and a soft warning only, any file can be sent
    const VALID_EXTS = ['.nc', '.gcode', '.tap', '.cnc', '.gc', '.ngc', '.pcn', '.plt', '.hnc', '.txt', '.crv', '.sbp', '.dxf', '.svg', '.xcn', '.din', '.cut', '.iso', '.rd', '.dst', '.tif', '.tiff', '.tff'];

    // DOS 8.3 names (Ruida lasers and other controllers that read short names only)
    const SFN_CHARS = "A-Z0-9!#$%&'()@^_`{}~-";
    const SFN_RE = new RegExp('^[' + SFN_CHARS + ']{1,8}(\\.[' + SFN_CHARS + ']{1,3})?$');
    const isShortName = (n) => (n === n.toUpperCase() || n === n.toLowerCase()) && SFN_RE.test(n.toUpperCase());
    // Same rule as the receiver's filename_is_valid()
    const nameIsValid = (n) => !!n && !n.includes('..') && !/[\x00-\x1f\x7f/\\:]/.test(n) && !/^\.+$/.test(n);

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
        if (s < 5) return t('mc.lastSeen.now');
        if (s < 60) return t('mc.lastSeen.s', { n: Math.round(s) });
        if (s < 3600) return t('mc.lastSeen.min', { n: Math.round(s / 60) });
        return t('mc.lastSeen.h', { n: Math.round(s / 3600) });
    }

    // Receiver's last restart from the heartbeat cache; '' until one is known
    function restartText(v) {
        if (!v.rst_reason) return '';
        const s = v.rst_ago_s;
        if (typeof s !== 'number' || s < 0) return t('tx.restartNoTime', { reason: v.rst_reason, n: v.brownouts });
        const ago = s < 60 ? t('tx.agoS', { n: Math.round(s) })
            : s < 3600 ? t('tx.agoMin', { n: Math.round(s / 60) }) : t('tx.agoH', { n: Math.round(s / 3600) });
        return t('tx.restart', { reason: v.rst_reason, ago, n: v.brownouts });
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
    let driveNames = new Set();  // upper-cased names last listed on the selected drive
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
        window.location.href = '/';
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

    // Signal bars from the RSSI the machine reports in its heartbeat
    function rssiBars(rssi) {
        if (!rssi) return '';
        const on = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
        const level = on >= 3 ? 'good' : on === 2 ? 'fair' : 'weak';
        const label = esc(t('mc.signal', { rssi }));
        return `<span class="rssi ${level}" role="img" aria-label="${label}" title="${label}">` +
            [1, 2, 3, 4].map(i => `<i${i <= on ? ' class="on"' : ''}></i>`).join('') + '</span>';
    }

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
                <span class="mcard-top"><span class="mcard-name">${esc(r.name)}</span>${r.online ? rssiBars(r.rssi) : ''}<span class="${pcls}">${esc(ptxt)}</span></span>
                ${!r.online
                    ? `<span class="muted small">${esc(lastSeenText(r.last_seen_s))}</span>`
                    : (r.card_error || r.state === 'CARD_ERROR')
                        ? `<span class="small err-text">${esc(t('state.CARD_ERROR'))}</span>`
                        : `<span class="bar bar-sm" aria-hidden="true"><span class="bar-fill" style="width:${used.toFixed(1)}%"></span></span>
                           <span class="muted small">${esc(storage)}</span>`}
                ${r.online ? `<span class="muted small">${esc(lastSeenText(r.last_seen_s))}</span>` : ''}
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
            pending = d.pending || [];
            renderAll();
            renderFoundNotice();
            if (!addDialog.hidden) renderFound(false);
            setAddWindow(d.add_window_s || 0);
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

    // ─── New receivers + Add machine dialog ─────────────────────────────
    const addDialog = $('addDialog');
    let pending = [];               // /receivers "pending": found, not yet approved
    const ignored = new Set();      // hidden for this page view (there is no reject endpoint)
    let addWindowEnd = 0;           // when the transmitter stops listening (ms)
    let addTimer = null;
    let addLastFocus = null;
    let foundKey = '';
    const visiblePending = () => pending.filter(p => !ignored.has(p.id));

    function renderFoundNotice() {
        const n = visiblePending().length;
        $('foundNotice').hidden = n === 0 || !addDialog.hidden;
        $('foundText').textContent = n === 1 ? t('mc.found') : t('mc.foundN', { n });
    }
    function addStatus(text, kind) {
        const el = $('addStatus');
        el.textContent = text || '';
        el.className = 'form-status small ' + (kind === 'err' ? 'err-text' : 'ok-text');
        el.hidden = !text;
    }

    function openAddDialog(startSearch) {
        addLastFocus = document.activeElement;
        addDialog.hidden = false;
        document.body.classList.add('no-scroll');
        addStatus('');
        renderFound(true);
        renderAddSearch();
        renderFoundNotice();
        if (startSearch) startAddWindow();
        $('addClose').focus();
    }
    function closeAddDialog() {
        addDialog.hidden = true;
        document.body.classList.remove('no-scroll');
        renderFoundNotice();
        if (addLastFocus && addLastFocus.focus) addLastFocus.focus();
    }
    $('addMachineBtn').addEventListener('click', () => openAddDialog(true));
    $('foundReview').addEventListener('click', () => openAddDialog(false));
    $('addClose').addEventListener('click', closeAddDialog);
    addDialog.addEventListener('click', (e) => { if (e.target === addDialog) closeAddDialog(); });
    addDialog.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') { closeAddDialog(); return; }
        if (e.key !== 'Tab') return;
        // Keep focus inside the dialog
        const els = Array.from(addDialog.querySelectorAll('button, input, [href]'))
            .filter(el => !el.disabled && el.offsetParent !== null);
        if (!els.length) return;
        const first = els[0], last = els[els.length - 1];
        if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
        else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
    });

    async function startAddWindow() {
        $('addStop').disabled = true;
        try {
            const res = await fetch('/receivers/add', { method: 'POST', headers: authHeaders() });
            if (!res.ok) {
                addStatus(t('add.failed', { why: await errorText(res) }), 'err');
            } else {
                setAddWindow((await res.json()).seconds || 120);
            }
        } catch (e) {
            addStatus(t('add.failed', { why: t('msg.fail.network') }), 'err');
        }
        renderAddSearch();
    }
    function setAddWindow(seconds) {
        if (seconds > 0) addWindowEnd = Date.now() + seconds * 1000;
        else if (addWindowEnd && Date.now() < addWindowEnd - 5000) addWindowEnd = 0; // closed early (Stop, restart)
        if (!addTimer && addWindowEnd > Date.now()) addTimer = setInterval(renderAddSearch, 1000);
        renderAddSearch();
    }
    function renderAddSearch() {
        const left = Math.max(0, Math.round((addWindowEnd - Date.now()) / 1000));
        if (!left && addTimer) { clearInterval(addTimer); addTimer = null; }
        const on = left > 0;
        $('addSearch').classList.toggle('on', on);
        $('addSearchText').textContent = on
            ? t('add.searching', { time: Math.floor(left / 60) + ':' + String(left % 60).padStart(2, '0') })
            : t('add.stopped');
        const stop = $('addStop');
        stop.disabled = false;
        stop.textContent = t(on ? 'add.stop' : 'add.again');
        stop.dataset.act = on ? 'stop' : 'again';
        // Every receiver takes one of the hotspot's Wi-Fi slots
        const full = $('addFull');
        full.hidden = !(lastWifiMode === 'AP' && receivers.length >= 12);
        full.textContent = t('add.full', { n: receivers.length });
    }
    $('addStop').addEventListener('click', () => {
        if ($('addStop').dataset.act === 'stop') stopAddWindow();
        else startAddWindow();
    });
    // Closes the transmitter's listening window now
    async function stopAddWindow() {
        $('addStop').disabled = true;
        try {
            const res = await fetch('/pair_window/stop', { method: 'POST', headers: authHeaders() });
            if (res.ok) addWindowEnd = 0;
            else addStatus(t('add.stopFailed', { why: await errorText(res) }), 'err');
        } catch (e) {
            addStatus(t('add.stopFailed', { why: t('msg.fail.network') }), 'err');
        }
        renderAddSearch();
    }

    function renderFound(force) {
        const box = $('addFound');
        const list = visiblePending();
        const key = list.map(p => p.id + (p.radio ? 'r' : '') + (p.repair ? 'x' : '')).join(',');
        if (!force && key === foundKey) return; // keep names being typed
        foundKey = key;
        const typed = {};
        box.querySelectorAll('input[data-id]').forEach(i => { typed[i.dataset.id] = i.value; });
        box.innerHTML = list.map(p => {
            const id = esc(p.id);
            const name = typed[p.id] !== undefined ? typed[p.id] : p.name;
            return `<div class="found-card">
                <div class="small muted">${esc(t('add.idLabel'))}</div>
                <div class="found-id mono">${id}</div>
                <p class="small"><b>${esc(t('add.idCheck'))}</b></p>
                <p class="small muted">${esc(p.radio ? t('add.viaRadio') : t('add.viaWifi', { ip: p.ip }))}${p.repair ? ' · ' + esc(t('add.repair')) : ''}</p>
                <div class="field">
                    <label for="fn-${id}">${esc(t('add.name'))}</label>
                    <input type="text" id="fn-${id}" maxlength="31" data-id="${id}" value="${esc(name)}">
                </div>
                <div class="confirm-actions">
                    <button type="button" class="btn btn-primary" data-act="approve" data-id="${id}">${esc(t('add.approve'))}</button>
                    <button type="button" class="btn" data-act="ignore" data-id="${id}">${esc(t('add.ignore'))}</button>
                </div>
            </div>`;
        }).join('');
    }
    $('addFound').addEventListener('click', async (e) => {
        const b = e.target.closest('button[data-act]');
        if (!b) return;
        const id = b.dataset.id;
        if (b.dataset.act === 'ignore') {
            ignored.add(id);
            renderFound(true);
            renderFoundNotice();
            return;
        }
        const input = $('addFound').querySelector(`input[data-id="${CSS.escape(id)}"]`);
        const name = input ? input.value.trim() : '';
        if (!name) { addStatus(t('add.needName'), 'err'); if (input) input.focus(); return; }
        b.disabled = true;
        const { error, message } = await postReceiverAction('approve', { id, name });
        b.disabled = false;
        if (error) { addStatus(t('msg.error.pairing') + ': ' + error, 'err'); return; }
        // ESP-NOW approvals report that the settings were sent; HTTP ones are paired now
        addStatus(t(message ? 'add.approvedRadio' : 'add.approved', { name }), 'ok');
        pending = pending.filter(p => p.id !== id);
        renderFound(true);
        if (!selectedReceiver()) rememberRx(id);
        await loadReceivers();
        loadReceiverFiles();
        pollStatus();
    });

    // Pairing file (fallback when the receiver can't hear the radio)
    $('pairFileToggle').addEventListener('click', () => {
        const box = $('pairFileBox');
        box.hidden = !box.hidden;
        $('pairFileToggle').setAttribute('aria-expanded', String(!box.hidden));
        if (!box.hidden) $('pairFileName').focus();
    });
    $('pairFileBtn').addEventListener('click', async () => {
        const name = $('pairFileName').value.trim();
        if (!name) { addStatus(t('add.needName'), 'err'); $('pairFileName').focus(); return; }
        $('pairFileBtn').disabled = true;
        const r = await downloadPairFile(name);
        $('pairFileBtn').disabled = false;
        addStatus(r.error ? t('add.fileFailed', { why: r.error }) : t('add.fileDone', { name }), r.error ? 'err' : 'ok');
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
            return { error: t('msg.fail.network') };
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

    // Same rule as the receiver's /prepare check: size + max(64 KB, 1% of drive)
    const spaceNeeded = (size, total) => size + Math.max(64 * 1024, Math.floor((total || 0) / 100));
    const fileFits = () => !currentFile || lastFree === null || lastFree >= spaceNeeded(currentFile.size, lastTotal);

    // "My sign job.rd" -> "MYSIGN01.RD" (number skips names already on the drive)
    function suggestShortName(name) {
        const dot = name.lastIndexOf('.');
        const clean = (x) => x.toUpperCase().replace(/[^A-Z0-9_-]/g, '');
        const base = clean(dot > 0 ? name.substring(0, dot) : name).substring(0, 6) || 'JOB';
        const ext = dot > 0 ? clean(name.substring(dot + 1)).substring(0, 3) : '';
        for (let i = 1; i < 100; i++) {
            const n = base + String(i).padStart(2, '0') + (ext ? '.' + ext : '');
            if (!driveNames.has(n)) return n;
        }
        return base + '99' + (ext ? '.' + ext : '');
    }

    // Name the file is stored under on the machine
    function sendName() {
        if (!currentFile) return '';
        if ($('shortName').hidden || !$('shortNameUse').checked) return currentFile.name;
        return $('shortNameInput').value.trim();
    }

    function setFile(file, name) {
        currentFile = file;
        const offer = !!file && !isShortName(file.name);
        $('shortName').hidden = !offer;
        if (offer) {
            const keepLong = name === file.name;
            $('shortNameUse').checked = !keepLong;
            $('shortNameInput').value = name && !keepLong ? name : suggestShortName(file.name);
        }
    }

    function renderShortName() {
        if ($('shortName').hidden) return;
        const use = $('shortNameUse').checked;
        const input = $('shortNameInput');
        input.disabled = !use || uploading;
        $('shortNameUse').disabled = uploading;
        const n = input.value.trim();
        const note = $('shortNameNote');
        note.hidden = !use || (nameIsValid(n) && isShortName(n));
        note.textContent = t(nameIsValid(n) ? 'file.short.long' : 'file.short.bad');
        note.className = 'small ' + (nameIsValid(n) ? 'muted' : 'err-text');
    }

    function renderTransfer() {
        const v = selectedView();
        const pill = $('txPill');
        if (!v) {
            $('txName').textContent = t('tx.noMachine');
            $('txSub').textContent = t('tx.noMachine.why');
            $('txReads').hidden = true;
            $('txRestart').hidden = true;
            pill.hidden = true;
        } else {
            $('txName').textContent = v.name;
            const st = v.online ? (v.state || 'UNKNOWN') : 'OFFLINE';
            const parts = [t('state.' + st)];
            if (v.storage_type) parts.push(storageLabel(v.storage_type));
            if (v.fw) parts.push(t('tx.fw', { v: v.fw }));
            $('txSub').textContent = parts.join(' · ');
            const rd = v.online ? v.reads : null;
            $('txReads').hidden = !rd;
            if (rd) {
                $('txReads').textContent = t('tx.reads', rd) + (rd.file ? ' ' + t('tx.readsFile', { file: rd.file }) : '');
            }
            const restart = restartText(v);
            $('txRestart').hidden = !restart;
            $('txRestart').textContent = restart;
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
            renderShortName();
        }
        fileZone.classList.toggle('disabled', uploading);
        $('chooseFileBtn').disabled = uploading;
        $('chooseAnotherBtn').disabled = uploading;

        uploadBtn.textContent = t('tx.send', { name: v ? v.name : '…' });
        uploadBtn.disabled = uploading || !currentFile || !nameIsValid(sendName()) || !v || !v.online || !fileFits() ||
            v.state === 'CARD_ERROR' || !!v.card_error || waitTimer !== null;
        cancelBtn.hidden = !uploading && !currentFile;
        renderMsgArea();
    }

    function renderAll() {
        renderMachines();
        renderTransfer();
        renderFilesHeading();
        renderSettings();
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
    const STAGE_KEYS = { Starting: 'stage.starting', Uploading: 'stage.uploading',
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
        if (v.storage_type === 'None') return { type: 'nocard' };
        if (v.card_error || v.state === 'CARD_ERROR') return { type: 'card' };
        if (currentFile && !fileFits()) {
            const need = spaceNeeded(currentFile.size, lastTotal);
            return { type: 'big', file: sendName(), need, free: lastFree };
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
        case 'nocard':
            return { key: 'nocard:' + (v ? v.id : ''), kind: 'err', icon: 'card', title: t('msg.nocard.title', { name }), why: t('msg.nocard.why') };
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
            const pText = m.phases ? phaseText(m.phases) : '';
            return { key: 'done:' + others.map(r => r.id + (r.busy ? 'b' : '')).join(',') + (m.showDetails ? ':' + pText : ''), kind: 'ok', icon: 'ok',
                title: t('msg.done.title', { file: m.file, name }),
                why: t('msg.done.why', { size: fmtBytes(m.size), time: fmtDuration(m.secs) }) + (m.slow ? ' ' + t('msg.done.slow') : ''),
                extra: (chips ? `<div class="again"><span class="small muted">${esc(t('msg.done.again'))}</span><div class="chips">${chips}</div></div>` : '') +
                    (m.showDetails && pText ? `<pre class="details">${esc(pText)}</pre>` : ''),
                actions: pText ? btn('details', t(m.showDetails ? 'msg.fail.hide' : 'msg.fail.details'), 'btn-ghost') : '' };
        }
        case 'offline':
            return { key: 'offline:' + (v ? v.id + ':' + Math.round((v.last_seen_s || 0) / 60) : ''), kind: 'info', icon: 'offline',
                title: t('msg.offline.title', { name }), why: t('msg.offline.why', { seen: lastSeenText(v ? v.last_seen_s : -1) }) };
        case 'failed': {
            const title = m.notStarted ? t('msg.fail.notStarted')
                : t(m.cancelled ? 'msg.fail.cancelled' : 'msg.fail.title', { pct: m.pct });
            const details = `<p class="msg-why">${esc(t('msg.fail.safe'))}</p>` + (m.showDetails && m.details
                ? `<pre class="details">${esc(m.details)}</pre>` : '');
            return { key: 'failed:' + !!m.showDetails + ':' + m.reason + ':' + m.details, kind: 'err', icon: 'err', title, why: m.reason, extra: details,
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
                setFile(lastSent.file, lastSent.name);
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
        setMsg({ type: 'waiting', name: target.name, file: sendName() });
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
        setFile(file);
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
        setFile(null);
        clearMsg();
        renderTransfer();
    });
    $('shortNameUse').addEventListener('change', () => renderTransfer());
    $('shortNameInput').addEventListener('input', () => renderTransfer());

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
            renderSettings();
            if (!addDialog.hidden) renderAddSearch();
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
            driveNames = new Set((data.files || []).map(f => String(f.name).toUpperCase()));
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
            setTimeout(() => { window.location.href = '/'; }, 2000);
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
            await new Promise(r => setTimeout(r, 250));
            try {
                const res = await fetch('/status', { headers: rxHeaders() });
                if (res.ok) {
                    const d = await res.json();
                    // RECEIVING may still be the transmitter's cached value from mid-upload
                    if (d.state && d.state !== 'FILE_READY' && d.state !== 'RECEIVING') return d;
                }
            } catch (e) { /* keep waiting */ }
        }
        return null;
    }

    // A failed transfer's cause shows up in the heartbeats after it: a receiver
    // that has been running for less time than the transfer took restarted during it.
    async function explainFailure(rxId, tStart) {
        const base = explicitMsg.details;
        const tFail = performance.now();
        for (let i = 0; i < 30; i++) { // a restart and Wi-Fi rejoin can take this long
            await new Promise(r => setTimeout(r, 1500));
            const m = explicitMsg;
            if (!m || m.type !== 'failed' || m.tStart !== tStart) return;
            try {
                const res = await fetch('/status', { headers: authHeaders({ 'X-Rx-Id': rxId }) });
                if (!res.ok) continue;
                const d = await res.json();
                const now = performance.now();
                const restarted = d.rst_ago_s >= 0 && d.rst_ago_s < (now - tStart) / 1000;
                const line = restartText(d);
                if (line) m.details = base + '\n' + line;
                if (restarted) m.reason = t('msg.fail.rxRestarted');
                renderMsgArea();
                // last_seen_s is rounded down: + 1 so a heartbeat from before the failure can't pass as newer
                if (restarted || (d.last_seen_s >= 0 && d.last_seen_s + 1 < (now - tFail) / 1000)) return;
            } catch (e) { /* keep waiting */ }
        }
    }

    // Bytes/s over the last ~5 s of acked progress (0 until there is 1 s of data)
    function rateMeter() {
        const pts = [];
        return (bytes) => {
            const now = performance.now();
            pts.push([now, bytes]);
            while (pts.length > 2 && now - pts[1][0] > 5000) pts.shift();
            const dt = (now - pts[0][0]) / 1000;
            return dt >= 1 ? (bytes - pts[0][1]) / dt : 0;
        };
    }

    // Phases of the last transfer, for "Show details" on the done message
    const PHASES = ['hash', 'start', 'ready', 'data', 'flush', 'verify', 'list', 'unmount', 'reattach', 'host'];
    function phaseText(p) {
        return PHASES.filter(k => typeof p[k] === 'number' && p[k] >= 0)
            .map(k => t('phase.' + k) + ': ' + Math.round(p[k]) + ' ms').join('\n');
    }
    // The drive's re-attach and the host's mount come after the done message:
    // fill them in from the heartbeat cache as they arrive
    async function fillLatePhases(rxId) {
        for (let i = 0; i < 30; i++) {
            await new Promise(r => setTimeout(r, 1000));
            const m = explicitMsg;
            if (!m || m.type !== 'done' || m.rxId !== rxId || !m.phases) return;
            try {
                const res = await fetch('/status', { headers: authHeaders({ 'X-Rx-Id': rxId }) });
                if (!res.ok) continue;
                const d = await res.json();
                if (d.reattach_ms >= 0) m.phases.reattach = d.reattach_ms;
                if (d.host_ms >= 0) m.phases.host = d.host_ms;
                renderMsgArea();
                if (d.reattach_ms >= 0 && d.host_ms >= 0) return;
            } catch (e) { /* keep trying */ }
        }
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
        const name = sendName();
        if (!nameIsValid(name)) return;
        cancelRequested = false;
        cancelBtn.disabled = false;
        clearMsg();
        uploading = true;
        clearInterval(statusInterval);
        renderAll(); // lock the selection while uploading

        let stage = 'Starting';
        let session = null;
        let outcome = null; // message shown in place of the progress area
        const tStart = performance.now();
        try {
            if (typeof SparkMD5 === 'undefined') throw new UploadError(stage, t('msg.fail.reload'));
            const phases = { hash: 0 };
            let tp = performance.now();
            setProgress(0, t('tx.preparing', { name: target.name }), 0);
            tp = performance.now();
            const beginHeaders = { 'Content-Type': 'application/json', 'X-Rx-Id': target.id };
            if (force) beginHeaders['X-Force'] = '1';
            let res;
            try {
                // Transmitter gives up on the receiver after 10 s (504); this is the backstop
                res = await apiPost('/upload_begin', beginHeaders,
                    JSON.stringify({ name, size: file.size }), 12000);
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
                    outcome = { type: 'big', name: target.name, file: name, free: d.free_bytes,
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
            const begun = await res.json();
            session = begun.session;
            const ready = (begun.rx && begun.rx.ms) || {};
            phases.ready = (ready.detach || 0) + (ready.mount || 0) + (ready.space || 0) + (ready.open || 0);
            phases.start = performance.now() - tp - phases.ready;
            tp = performance.now();

            stage = 'Uploading';
            const total = file.size;
            let acked = 0;
            const rate = rateMeter();
            const spark = new SparkMD5.ArrayBuffer(); // hashed as it is read for sending
            for (let index = 0, offset = 0; offset < total; index++, offset += CHUNK_SIZE) {
                if (cancelRequested) throw new UploadError(stage, 'cancelled');
                const th = performance.now();
                let part;
                try {
                    part = await file.slice(offset, offset + CHUNK_SIZE).arrayBuffer();
                } catch (x) {
                    throw new UploadError(stage, t('msg.fail.read'));
                }
                spark.append(part);
                phases.hash += performance.now() - th;
                acked = await sendChunk(session, index, part);
                const bps = rate(acked);
                setProgress(total ? (acked / total) * 100 : 100, t('tx.uploading', { progress: fmtPair(acked, total) }) +
                    (bps ? ' · ' + t('tx.rate', { rate: fmtBytes(bps), eta: fmtDuration((total - acked) / bps) }) : ''), 0);
            }
            if (cancelRequested) throw new UploadError(stage, 'cancelled');

            stage = 'Verifying';
            cancelBtn.disabled = true; // past the point of a clean cancel
            setProgress(100, t('tx.verifying'), 1);
            phases.data = performance.now() - tp - phases.hash;
            const fin = await apiPost('/upload_finish', { 'X-Session': session, 'Content-Type': 'application/json' },
                JSON.stringify({ md5: spark.end(), hash_ms: Math.round(phases.hash) }));
            session = null; // the transmitter ends the session either way
            if (!fin.ok) throw new UploadError(stage, await errorText(fin), fin.status);
            const finMs = ((await fin.json()).rx || {}).ms || {};
            phases.flush = finMs.flush;
            phases.verify = (finMs.md5 || 0) + (finMs.commit || 0);
            phases.list = finMs.list;
            phases.unmount = finMs.unmount;
            const secs = (performance.now() - tStart) / 1000;

            stage = 'Reconnecting drive';
            setProgress(100, t('tx.reconnecting'), 2);
            const state = await waitForDriveReconnect(10000);
            setProgress(100, t('tx.done'), 3);
            outcome = { type: 'done', name: target.name, rxId: target.id, file: name, size: file.size, secs, slow: !state, phases };
            fillLatePhases(target.id);
            lastSent = { file, name, rxId: target.id };
            setFile(null);
        } catch (e) {
            const err = e instanceof UploadError ? e : new UploadError(stage, e.message || String(e));
            if (session) {
                try { await apiPost('/upload_cancel', { 'X-Session': session }, ''); } catch (x) { /* best effort */ }
            }
            const network = e instanceof TypeError || /network error/.test(err.message);
            outcome = {
                type: 'failed',
                tStart,
                rxId: target.id,
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
            if (outcome && outcome.tStart && !outcome.cancelled) explainFailure(outcome.rxId, outcome.tStart);
            loadReceiverFiles();
            startStatusPolling();
            setTimeout(pollStatus, 1000);
        }
    }

    // ─── Settings page ──────────────────────────────────────────────────
    const SECTIONS = ['network', 'machines', 'login', 'appearance', 'system'];
    const netForm = $('netForm');
    const loginSetForm = $('loginSetForm');
    let cfg = null;          // GET /config (no passwords)
    let macEdit = null;      // id being renamed
    let macConfirm = null;   // id asking to be removed

    function formStatus(el, text, kind) {
        el.textContent = text || '';
        el.className = 'form-status small ' + (kind === 'err' ? 'err-text' : 'ok-text');
        el.hidden = !text;
    }

    function route() {
        const h = location.hash.replace(/^#/, '');
        const inSettings = h === 'settings' || h.startsWith('settings/');
        const wasSettings = !$('settingsView').hidden;
        $('dashView').hidden = inSettings;
        $('settingsView').hidden = !inSettings;
        document.title = t(inSettings ? 'set.title' : 'app.title.main');
        if (!inSettings) return;
        const sec = SECTIONS.includes(h.split('/')[1]) ? h.split('/')[1] : 'network';
        document.querySelectorAll('.set-nav a').forEach(a => {
            if (a.dataset.sec === sec) a.setAttribute('aria-current', 'page');
            else a.removeAttribute('aria-current');
        });
        document.querySelectorAll('.set-panel').forEach(p => { p.hidden = p.dataset.panel !== sec; });
        if (!wasSettings) {
            window.scrollTo(0, 0);
            loadSettings();
        }
        renderSettings();
    }
    window.addEventListener('hashchange', route);
    $('configBtn').addEventListener('click', () => { location.hash = 'settings/network'; });

    // ─── Accent colour (stored on the transmitter, shared by every browser) ─
    const ACCENT_DEFAULT = '#C2410C';
    let accentSaved = window.FX_ACCENT;
    function markAccent() {
        const cur = window.FX_ACCENT || ACCENT_DEFAULT;
        let preset = false;
        document.querySelectorAll('[data-accent]').forEach(b => {
            const on = b.dataset.accent === cur;
            preset = preset || on;
            b.setAttribute('aria-pressed', String(on));
        });
        $('accentCustom').value = cur.toLowerCase();
        $('accentCustom').parentElement.setAttribute('aria-pressed', String(!preset));
    }
    async function saveAccent(hex) {
        applyAccent(hex); // live preview
        markAccent();
        if (window.FX_ACCENT === accentSaved) return; // unchanged: nothing to store
        try {
            const res = await fetch('/api/ui', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ accent: window.FX_ACCENT })
            });
            if (!res.ok) throw new Error(await errorText(res));
            accentSaved = window.FX_ACCENT;
            formStatus($('accentStatus'), '');
        } catch (e) {
            applyAccent(accentSaved);
            markAccent();
            formStatus($('accentStatus'), t('app.accent.failed', { why: e.message === 'Failed to fetch' ? t('msg.fail.network') : e.message }), 'err');
        }
    }
    $('accentSwatches').addEventListener('click', (e) => {
        const b = e.target.closest('[data-accent]');
        if (b) saveAccent(b.dataset.accent);
    });
    // Custom picker: preview on input; save on change (debounced 400 ms); picker
    // closed without a change -> back to the saved colour
    let accentTimer = null, accentPreview = false;
    $('accentCustom').addEventListener('input', () => {
        accentPreview = true;
        applyAccent($('accentCustom').value);
    });
    $('accentCustom').addEventListener('change', () => {
        accentPreview = false;
        clearTimeout(accentTimer);
        accentTimer = setTimeout(() => saveAccent($('accentCustom').value), 400);
    });
    $('accentCustom').addEventListener('blur', () => {
        if (!accentPreview) return;
        accentPreview = false;
        applyAccent(accentSaved);
        markAccent();
    });
    $('accentReset').addEventListener('click', () => saveAccent(ACCENT_DEFAULT));
    markAccent();

    // ─── Firmware update ────────────────────────────────────────────────
    // Image header (24) + segment header (8) + esp_app_desc_t: version at +16,
    // project name at +48 (32 bytes each, NUL-padded)
    const FW_DESC = 32, FW_HEAD = FW_DESC + 256;
    let fwFile = null, fwImage = null, fwCancelled = false, fwBusy = false;
    const fwRx = {};            // receiver id -> {text, kind, version, until} (update result)
    let fwListHtml = '';
    const isTxImage = () => !!fwImage && fwImage.project === 'cnc_transmitter';
    const isRxImage = () => !!fwImage && fwImage.project === 'cnc_receiver';
    function readFwDesc(buf) {
        const u = new Uint8Array(buf);
        if (u.length < FW_HEAD || u[0] !== 0xE9 || new DataView(buf).getUint32(FW_DESC, true) !== 0xABCD5432) return null;
        const str = (o) => { let s = ''; for (let i = o; i < o + 32 && u[i]; i++) s += String.fromCharCode(u[i]); return s; };
        return { version: str(FW_DESC + 16), project: str(FW_DESC + 48) };
    }
    function fwProgress(pct) {
        $('fwBar').hidden = pct === null;
        $('fwFill').style.width = (pct || 0) + '%';
    }
    // Receiver image: one row per machine with its own Update button and result
    function renderFwMachines() {
        const list = $('fwMachines');
        list.hidden = !isRxImage();
        if (list.hidden) return;
        for (const r of receivers) {
            const s = fwRx[r.id];
            if (s && s.version && r.online && r.fw === s.version) fwRx[r.id] = { text: t('sys.fwRxDone', { v: s.version }), kind: 'ok' };
            else if (s && s.version && Date.now() > s.until) fwRx[r.id] = { text: t('sys.fwRxLost', { v: s.version, cur: r.fw || '-' }), kind: 'err' };
        }
        const html = receivers.length ? receivers.map(r => {
            const why = !r.online ? t('pill.offline') : r.state === 'RECEIVING' ? t('pill.receiving') : r.busy ? t('pill.job') : '';
            const s = fwRx[r.id];
            return `<li class="fw-row"><span class="fw-name"><b>${esc(r.name || r.id)}</b> <span class="muted small">${esc(r.fw || '-')}${why ? ' · ' + esc(why) : ''}</span></span>` +
                `<button type="button" class="btn" data-fw-rx="${esc(r.id)}"${fwBusy || why ? ' disabled' : ''}>${esc(t('sys.fwUpdate', { name: r.name || r.id }))}</button>` +
                (s ? `<span class="small ${s.kind === 'err' ? 'err-text' : 'ok-text'}" role="status">${esc(s.text)}</span>` : '') + '</li>';
        }).join('') : `<li class="muted small">${esc(t('mac.none'))}</li>`;
        if (html !== fwListHtml) list.innerHTML = fwListHtml = html; // keep focus when nothing changed
    }
    setInterval(() => { if (isRxImage()) renderFwMachines(); }, 2000);
    $('fwFile').addEventListener('change', async () => {
        fwFile = $('fwFile').files[0] || null;
        fwImage = null;
        formStatus($('fwStatus'), '');
        fwProgress(null);
        const info = $('fwInfo');
        info.hidden = !fwFile;
        if (fwFile) {
            fwImage = readFwDesc(await fwFile.slice(0, FW_HEAD).arrayBuffer());
            // Signed images end in a 4 KB signature sector starting E7 02
            const tail = fwFile.size % 4096 === 0 && fwFile.size > 4096
                ? new Uint8Array(await fwFile.slice(fwFile.size - 4096, fwFile.size - 4094).arrayBuffer()) : null;
            const signed = !!tail && tail[0] === 0xE7 && tail[1] === 0x02;
            if (fwImage && !signed) {
                fwImage = null;
                info.hidden = false;
                info.textContent = t('sys.fwUnsigned');
                $('fwBtn').hidden = false;
                $('fwBtn').disabled = true;
                renderFwMachines();
                return;
            }
            info.textContent = isTxImage() ? t('sys.fwTx', { v: fwImage.version, cur: status ? status.tx_fw : '-' })
                : isRxImage() ? t('sys.fwRx', { v: fwImage.version }) : t('sys.fwBad');
        }
        $('fwBtn').hidden = isRxImage();
        $('fwBtn').disabled = !isTxImage();
        renderFwMachines();
    });
    async function fwPost(path, headers, body) {
        for (let attempt = 0; attempt < 3; attempt++) {
            try {
                const res = await fetch(path, { method: 'POST', headers: authHeaders(headers), body });
                if (res.status < 500 || attempt === 2) return res;
            } catch (e) { /* retry */ }
            await new Promise(r => setTimeout(r, 1000));
        }
        throw new Error('network');
    }
    // begin / chunks / finish against pathFor(op); returns the finish reply, null if cancelled
    async function fwSend(pathFor, onPct) {
        let res = await fwPost(pathFor('begin'), { 'Content-Type': 'application/json' }, JSON.stringify({ size: fwFile.size }));
        if (!res.ok) throw new Error(await errorText(res));
        const chunk = (await res.json()).chunk_size || 16384;
        for (let off = 0; off < fwFile.size; off += chunk) {
            if (fwCancelled) {
                await fwPost(pathFor('cancel'), {}, '').catch(() => {});
                return null;
            }
            res = await fwPost(pathFor('chunk'), { 'X-Offset': String(off), 'Content-Type': 'application/octet-stream' },
                fwFile.slice(off, off + chunk));
            if (!res.ok) throw new Error(await errorText(res));
            onPct(Math.round(Math.min(off + chunk, fwFile.size) / fwFile.size * 100));
        }
        res = await fwPost(pathFor('finish'), {}, '');
        if (!res.ok) throw new Error(await errorText(res));
        return res.json();
    }
    const fwWhy = (e) => e.message === 'network' ? t('msg.fail.network') : e.message;
    function fwLock(on) {
        fwBusy = on;
        fwCancelled = false;
        $('fwFile').disabled = on;
        $('fwCancel').hidden = !on;
        $('fwBtn').disabled = on || !isTxImage() || signingOut;
        renderFwMachines();
    }
    $('fwCancel').addEventListener('click', () => { fwCancelled = true; });
    $('fwBtn').addEventListener('click', async () => {
        if (!fwFile || !isTxImage() || fwBusy) return;
        const st = $('fwStatus');
        fwLock(true);
        fwProgress(0);
        try {
            const d = await fwSend(op => '/fw/' + op, pct => {
                fwProgress(pct);
                formStatus(st, t('sys.fwUploading', { pct }), 'ok');
            });
            fwProgress(null);
            if (!d) {
                formStatus(st, t('sys.fwCancelled'), 'ok');
            } else {
                signingOut = true; // restarting: the session ends with it
                sessionStorage.removeItem('token');
                formStatus(st, t('sys.fwDone', { v: d.version }), 'ok');
                setTimeout(() => { window.location.href = '/'; }, 45000);
            }
        } catch (e) {
            fwProgress(null);
            formStatus(st, t('sys.fwFailed', { why: fwWhy(e) }), 'err');
        } finally {
            fwLock(false);
        }
    });
    $('fwMachines').addEventListener('click', async (e) => {
        const b = e.target.closest('[data-fw-rx]');
        if (!b || fwBusy || !isRxImage()) return;
        const id = b.dataset.fwRx;
        const version = fwImage.version;
        fwLock(true);
        const set = (s) => { fwRx[id] = s; renderFwMachines(); };
        try {
            const d = await fwSend(op => '/rxfw/' + op + '?rx=' + encodeURIComponent(id),
                pct => set({ text: t('sys.fwRxProgress', { pct }), kind: 'ok' }));
            set(d ? { text: t('sys.fwRxRestart'), kind: 'ok', version: d.version || version, until: Date.now() + 180000 }
                  : { text: t('sys.fwCancelled'), kind: 'ok' });
        } catch (err) {
            set({ text: t('sys.fwRxFailed', { why: fwWhy(err) }), kind: 'err' });
        } finally {
            fwLock(false);
        }
    });

    // Crash report (core dump) download: transmitter or a machine
    function renderCrashSelect() {
        const sel = $('crashDevice');
        const keep = sel.value;
        sel.innerHTML = `<option value="">${esc(t('sys.crashTx'))}</option>` +
            receivers.map(r => `<option value="${esc(r.id)}">${esc(r.name || r.id)}</option>`).join('');
        sel.value = [...sel.options].some(o => o.value === keep) ? keep : '';
    }
    $('crashBtn').addEventListener('click', async () => {
        const sel = $('crashDevice');
        const id = sel.value;
        const name = sel.options[sel.selectedIndex].text;
        const st = $('crashStatus');
        $('crashBtn').disabled = true;
        try {
            const res = await fetch(id ? '/receiver_coredump?rx=' + encodeURIComponent(id) : '/coredump',
                { headers: authHeaders() });
            if (res.ok) {
                const a = document.createElement('a');
                a.href = URL.createObjectURL(await res.blob());
                a.download = (id || 'transmitter') + '-coredump.bin';
                a.click();
                URL.revokeObjectURL(a.href);
                formStatus(st, t('sys.crashDone', { name }), 'ok');
            } else {
                const why = await errorText(res);
                const none = res.status === 404 && why.startsWith('No crash');
                formStatus(st, none ? t('sys.crashNone', { name }) : t('sys.crashFailed', { why }), none ? 'ok' : 'err');
            }
        } catch (e) {
            formStatus(st, t('sys.crashFailed', { why: t('msg.fail.network') }), 'err');
        }
        $('crashBtn').disabled = false;
    });

    async function loadSettings() {
        loadRadio();
        [netForm, loginSetForm].forEach(f => formStatus(f.querySelector('.form-status'), ''));
        try {
            const r = await fetch('/config', { headers: authHeaders() });
            if (!r.ok) throw new Error('HTTP ' + r.status);
            cfg = await r.json();
        } catch (e) {
            formStatus(netForm.querySelector('.form-status'), t('set.loadFailed', { why: e.message }), 'err');
            return;
        }
        const mode = netForm.querySelector(`input[name="wifi_mode"][value="${cfg.wifi_mode === 'STA' ? 'STA' : 'AP'}"]`);
        mode.checked = true;
        $('staSsid').value = cfg.sta_ssid || '';
        $('staPass').value = '';
        $('apSsid').value = cfg.ap_ssid || '';
        $('apPass').value = '';
        $('authUser').value = cfg.auth_user || '';
        $('authPass').value = '';
        renderSettings();
    }

    function renderSettings() {
        if ($('settingsView').hidden) return;
        // Network: router fields only when joining one; live router status
        const sta = netForm.querySelector('input[name="wifi_mode"]:checked');
        const joining = sta && sta.value === 'STA';
        $('routerFields').hidden = !joining;
        const line = $('staStatus');
        const st = status && status.sta;
        let text = '', kind = '';
        if (joining && cfg && cfg.wifi_mode === 'STA' && st) {
            if (st.state === 'connected') { text = t('net.sta.connected', { ip: st.ip }); kind = 'ok'; }
            else if (st.state === 'failed') { text = t('net.sta.failed', { why: st.reason || '?' }); kind = 'err'; }
            else if (st.state === 'connecting') { text = t('net.sta.connecting'); }
        }
        line.textContent = text;
        line.className = 'status-line small ' + (kind === 'ok' ? 'ok-text' : kind === 'err' ? 'err-text' : 'muted');
        line.hidden = !text;
        renderOfflineWarn();
        renderMachineTable();
        renderPerfSelect();
        renderCrashSelect();
        // System
        if (status) {
            $('sysFw').textContent = status.tx_fw || '-';
            $('sysId').textContent = status.tx_id || '-';
            if (typeof status.ap_clients === 'number') {
                $('sysDevices').textContent = t('sys.devicesVal', { n: status.ap_clients, max: status.ap_max_clients || 15 });
            }
            const left = status.recovery_s || 0;
            $('recoveryOn').hidden = !left;
            $('recoveryOn').textContent = t('sys.recoveryOn', { n: Math.ceil(left / 60) });
        }
    }
    netForm.addEventListener('change', renderSettings);
    netForm.addEventListener('input', (e) => { if (e.target.id === 'apSsid' || e.target.id === 'apPass') renderOfflineWarn(); });

    // Offline machines miss a hotspot name/password change
    function renderOfflineWarn() {
        const warn = $('offlineWarn');
        const changed = cfg && ($('apPass').value.length > 0 || $('apSsid').value.trim() !== (cfg.ap_ssid || ''));
        const offline = receivers.filter(r => !r.online);
        warn.hidden = !changed || !offline.length;
        if (warn.hidden) return;
        warn.innerHTML = `<p>${esc(t('net.offlineWarn'))}</p><ul>${offline.map(r => `<li>${esc(r.name)}</li>`).join('')}</ul>` +
            `<p>${esc(t('net.offlineWarn2'))}</p>`;
    }

    // POST /config: blank fields keep the current value; the transmitter restarts
    async function saveConfig(form, data) {
        const statusEl = form.querySelector('.form-status');
        const submit = form.querySelector('button[type="submit"]');
        submit.disabled = true;
        try {
            const res = await fetch('/config', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify(data)
            });
            if (!res.ok) {
                formStatus(statusEl, t('set.saveFailed', { why: await errorText(res) }), 'err');
                submit.disabled = false;
                return;
            }
            const router = data.wifi_mode === 'STA';
            formStatus(statusEl, router ? t('set.savedRouter', { ip: lastStaIp || '…' }) : t('set.saved'), 'ok');
            sessionStorage.removeItem('token');
            setTimeout(() => { window.location.href = '/'; }, router ? 20000 : 5000);
        } catch (e) {
            formStatus(statusEl, t('set.netError'), 'err');
            submit.disabled = false;
        }
    }
    netForm.addEventListener('submit', (e) => {
        e.preventDefault();
        const mode = netForm.querySelector('input[name="wifi_mode"]:checked');
        const apPass = $('apPass').value;
        if (apPass && apPass.length < 8) {
            formStatus(netForm.querySelector('.form-status'), t('net.passShort'), 'err');
            $('apPass').focus();
            return;
        }
        saveConfig(netForm, {
            wifi_mode: mode ? mode.value : 'AP',
            sta_ssid: $('staSsid').value.trim(),
            sta_pass: $('staPass').value,
            ap_ssid: $('apSsid').value.trim(),
            ap_pass: apPass
        });
    });
    loginSetForm.addEventListener('submit', (e) => {
        e.preventDefault();
        saveConfig(loginSetForm, { auth_user: $('authUser').value.trim(), auth_pass: $('authPass').value });
    });

    // Machines: rename / remove inline
    function renderMachineTable() {
        if (macEdit || macConfirm) return; // don't wipe what is being typed or confirmed
        const box = $('macTable');
        if (!receivers.length) {
            box.innerHTML = '<p class="muted small">' + esc(t('mac.none')) + '</p>';
            return;
        }
        box.innerHTML = `<table class="mtable"><thead><tr><th>${esc(t('mac.colName'))}</th><th>${esc(t('mac.colId'))}</th>
            <th>${esc(t('mac.colStatus'))}</th><th><span class="visually-hidden">${esc(t('mac.rename'))}</span></th></tr></thead><tbody>` +
            receivers.map(r => {
                const [pcls, ptxt] = pillFor(r);
                const id = esc(r.id);
                return `<tr data-id="${id}"><td class="m-name">${esc(r.name)}</td><td class="mono m-id">${id}</td>
                    <td><span class="${pcls}">${esc(ptxt)}</span></td>
                    <td class="m-act"><button type="button" class="btn" data-act="rename" data-id="${id}">${esc(t('mac.rename'))}</button>
                    <button type="button" class="btn btn-ghost danger-text" data-act="remove" data-id="${id}">${esc(t('mac.remove'))}</button></td></tr>`;
            }).join('') + '</tbody></table>';
    }
    $('macTable').addEventListener('click', async (e) => {
        const b = e.target.closest('button[data-act]');
        if (!b) return;
        const id = b.dataset.id;
        const r = receivers.find(x => x.id === id);
        const row = $('macTable').querySelector(`tr[data-id="${CSS.escape(id)}"]`);
        const st = $('macStatus');
        switch (b.dataset.act) {
        case 'rename':
            macEdit = id;
            row.querySelector('.m-name').innerHTML = `<input type="text" maxlength="31" value="${esc(r ? r.name : '')}" aria-label="${esc(t('mac.colName'))}">`;
            row.querySelector('.m-act').innerHTML = `<button type="button" class="btn btn-primary" data-act="save" data-id="${esc(id)}">${esc(t('mac.save'))}</button>
                <button type="button" class="btn" data-act="cancel" data-id="${esc(id)}">${esc(t('common.cancel'))}</button>`;
            row.querySelector('input').focus();
            break;
        case 'save': {
            const name = row.querySelector('input').value.trim();
            if (!name) return;
            b.disabled = true;
            const res = await postReceiverAction('rename', { id, name });
            formStatus(st, res.error ? t('mac.failed', { why: res.error }) : t('mac.renamed', { name }), res.error ? 'err' : 'ok');
            macEdit = null;
            await loadReceivers();
            renderMachineTable();
            break;
        }
        case 'remove':
            macConfirm = id;
            row.insertAdjacentHTML('afterend', `<tr class="m-confirm"><td colspan="4"><div class="confirm">
                <p class="confirm-title">${esc(t('mac.confirmRemove', { name: r ? r.name : id }))}</p>` +
                confirmButtons(t('mac.remove'), t('files.keep')).replace(/data-act="yes"/, `data-act="removeyes" data-id="${esc(id)}"`)
                    .replace(/data-act="no"/, `data-act="cancel" data-id="${esc(id)}"`) + '</div></td></tr>');
            break;
        case 'removeyes': {
            b.disabled = true;
            const res = await postReceiverAction('unpair', { id });
            // Offline receivers: "Hold its BOOT button 10 s to finish"
            formStatus(st, res.error ? t('mac.failed', { why: res.error })
                : t('mac.removed', { name: r ? r.name : id }) + (res.message ? ' ' + res.message + '.' : ''), res.error ? 'err' : 'ok');
            macConfirm = null;
            await loadReceivers();
            renderMachineTable();
            break;
        }
        case 'cancel':
            macEdit = null;
            macConfirm = null;
            renderMachineTable();
            break;
        }
    });

    // Recovery mode (typed RECOVERY, checked server-side)
    $('recoveryWord').addEventListener('input', () => {
        $('recoveryBtn').disabled = $('recoveryWord').value.trim() !== 'RECOVERY';
    });
    $('recoveryBtn').addEventListener('click', async () => {
        if ($('recoveryWord').value.trim() !== 'RECOVERY') return;
        const st = $('recoveryStatus');
        $('recoveryBtn').disabled = true;
        try {
            const res = await fetch('/recovery', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ confirm: 'RECOVERY' })
            });
            formStatus(st, res.ok ? t('sys.recoveryStarted') : t('sys.recoveryFailed', { why: await errorText(res) }), res.ok ? 'ok' : 'err');
        } catch (e) {
            // Switching the network drops this connection: expected
            formStatus(st, t('sys.recoveryStarted'), 'ok');
        }
        $('recoveryWord').value = '';
    });

    // ─── Radio power: saved level, and a test of each level ────────────────
    const RADIO_LEVELS = [[34, '8.5'], [44, '11'], [52, '13'], [60, '15'], [68, '17'], [78, '19.5']];
    const radioDbm = (q) => { const l = RADIO_LEVELS.find(x => x[0] === q); return l ? l[1] : (q / 4).toFixed(1); };
    $('radioLevel').innerHTML = RADIO_LEVELS.map(l => `<option value="${l[0]}">${l[1]} dBm</option>`).join('');
    async function loadRadio() {
        try {
            const res = await fetch('/radio', { headers: authHeaders() });
            if (!res.ok) return;
            const d = await res.json();
            $('radioLevel').value = String(d.saved);
            $('radioInfo').textContent = t('radio.info', { dbm: radioDbm(d.saved), reason: d.reset_reason, n: d.brownouts });
        } catch (e) { /* shown again on the next visit */ }
    }
    $('radioSave').addEventListener('click', async () => {
        const st = $('radioStatus');
        const qdbm = Number($('radioLevel').value);
        $('radioSave').disabled = true;
        try {
            const res = await fetch('/radio', { method: 'POST', headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ qdbm }) });
            if (!res.ok) throw new Error(await errorText(res));
            const d = await res.json();
            formStatus(st, t(d.failed ? 'radio.savedSome' : 'radio.saved', { dbm: radioDbm(qdbm), n: d.machines, k: d.failed }), d.failed ? 'err' : 'ok');
            loadRadio();
        } catch (e) {
            formStatus(st, t('radio.failed', { why: e.message }), 'err');
        }
        $('radioSave').disabled = false;
    });
    $('radioTest').addEventListener('click', async () => {
        const rxId = $('radioRx').value;
        if (!rxId || uploading) return;
        const st = $('radioTestStatus');
        const body = $('radioTable').querySelector('tbody');
        body.innerHTML = '';
        $('radioTable').hidden = false;
        $('radioTest').disabled = true;
        $('radioSave').disabled = true;
        uploading = true; // keep the dashboard from starting a transfer meanwhile
        try {
            for (const [qdbm, dbm] of RADIO_LEVELS) {
                formStatus(st, t('radio.testing', { dbm }), 'ok');
                let d;
                try {
                    const res = await fetch('/radio/test?rx=' + encodeURIComponent(rxId), { method: 'POST',
                        headers: authHeaders({ 'Content-Type': 'application/json' }), body: JSON.stringify({ qdbm, seconds: 30 }) });
                    if (!res.ok) throw new Error(await errorText(res));
                    d = await res.json();
                } catch (e) {
                    formStatus(st, t('radio.testLost', { dbm }), 'err');
                    return;
                }
                const result = d.brownouts > 0 ? 'radio.brownout' : d.rebooted ? 'radio.rebooted' : !d.answered ? 'radio.noAnswer' : !d.link_ok ? 'radio.lost' : 'radio.ok';
                body.insertAdjacentHTML('beforeend', `<tr><td>${esc(dbm)} dBm</td>` +
                    `<td>${esc(fmtBytes(d.up_kbps * 1024))}/s / ${esc(fmtBytes(d.down_kbps * 1024))}/s</td>` +
                    `<td>${esc(String(d.rssi_rx))} / ${esc(String(d.rssi_tx))} dBm</td><td>${esc(t(result))}</td></tr>`);
                if (result !== 'radio.ok') {
                    formStatus(st, t('radio.testStopped', { dbm }), 'err');
                    return;
                }
            }
            formStatus(st, t('radio.testDone'), 'ok');
        } finally {
            uploading = false;
            $('radioSave').disabled = false;
            renderPerfSelect();
            loadRadio();
        }
    });

    // ─── Speed test: each leg separately, then a real upload of test data ──
    function renderPerfSelect() {
        const sel = $('perfRx');
        const keep = sel.value || selectedRx;
        sel.innerHTML = receivers.filter(r => r.online)
            .map(r => `<option value="${esc(r.id)}">${esc(r.name || r.id)}</option>`).join('');
        if ([...sel.options].some(o => o.value === keep)) sel.value = keep;
        $('perfBtn').disabled = !sel.options.length || uploading;
        const rsel = $('radioRx');
        const rkeep = rsel.value || selectedRx;
        rsel.innerHTML = sel.innerHTML;
        if ([...rsel.options].some(o => o.value === rkeep)) rsel.value = rkeep;
        $('radioTest').disabled = !rsel.options.length || uploading;
    }
    function perfRow(leg, d) {
        const kbps = d.bytes / 1024 / (d.ms / 1000);
        $('perfTable').querySelector('tbody').insertAdjacentHTML('beforeend',
            `<tr><td>${esc(leg)}</td><td>${esc(fmtBytes(kbps * 1024))}/s</td><td>${esc(fmtDuration(d.ms / 1000))}${d.chunks ? '<br><span class="small muted">' + esc(t('perf.chunks', d.chunks)) + '</span>' : ''}</td></tr>`);
        $('perfTable').hidden = false;
    }
    async function perfJson(res) {
        if (!res.ok) throw new Error(await errorText(res));
        return res.json();
    }
    // Upload size bytes of a repeated 32 KB block through the normal path, then delete it
    async function perfFull(rxId, size, relay) {
        const block = new Uint8Array(CHUNK_SIZE);
        for (let i = 0; i < block.length; i++) block[i] = (i * 31 + 7) & 255;
        const spark = new SparkMD5.ArrayBuffer();
        for (let off = 0; off < size; off += CHUNK_SIZE) spark.append(block.subarray(0, Math.min(CHUNK_SIZE, size - off)).slice().buffer);
        const name = 'FXSPEED.BIN';
        const t0 = performance.now();
        const begin = await perfJson(await fetch('/upload_begin', { method: 'POST',
            headers: authHeaders({ 'Content-Type': 'application/json', 'X-Rx-Id': rxId, 'X-Relay': relay, 'X-Force': '1' }),
            body: JSON.stringify({ name, size, md5: spark.end() }) }));
        const times = [];
        for (let i = 0, off = 0; off < size; i++, off += CHUNK_SIZE) {
            const tc = performance.now();
            await sendChunk(begin.session, i, new Blob([block.subarray(0, Math.min(CHUNK_SIZE, size - off))]));
            times.push(performance.now() - tc);
        }
        console.log('perf chunk ms (' + relay + ')', times.map(Math.round).join(' '));
        await perfJson(await fetch('/upload_finish', { method: 'POST', headers: authHeaders({ 'X-Session': begin.session }) }));
        const ms = performance.now() - t0;
        await fetch('/receiver_delete', { method: 'POST', headers: authHeaders({ 'Content-Type': 'application/json', 'X-Rx-Id': rxId, 'X-Force': '1' }),
            body: JSON.stringify({ files: [name] }) }).catch(() => {});
        const chunks = { min: Math.round(Math.min(...times)), max: Math.round(Math.max(...times)),
            avg: Math.round(times.reduce((a, b) => a + b, 0) / times.length) };
        return { bytes: size, ms, chunks };
    }
    $('perfBtn').addEventListener('click', async () => {
        const rxId = $('perfRx').value, size = Number($('perfSize').value);
        if (!rxId || uploading) return;
        const st = $('perfStatus');
        $('perfBtn').disabled = true;
        $('perfTable').querySelector('tbody').innerHTML = '';
        const legs = [
            ['perf.up', async () => {
                const blob = new Blob([new Uint8Array(CHUNK_SIZE)]);
                const n = Math.ceil(size / CHUNK_SIZE), t0 = performance.now();
                for (let i = 0; i < n; i++) {
                    const mark = i === 0 ? 'begin' : i === n - 1 ? 'end' : 'mid';
                    await perfJson(await fetch('/perf/sink', { method: 'POST', body: blob,
                        headers: authHeaders({ 'Content-Type': 'application/octet-stream', 'X-Perf': mark }) }));
                }
                return { bytes: n * CHUNK_SIZE, ms: performance.now() - t0 };
            }],
            ['perf.link', async () => perfJson(await fetch('/perf/link?rx=' + encodeURIComponent(rxId), { method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }), body: JSON.stringify({ size }) }))],
            ['perf.sd', async () => perfJson(await fetch('/perf/sd?rx=' + encodeURIComponent(rxId), { method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }), body: JSON.stringify({ size }) }))],
            ['perf.full', async () => perfFull(rxId, size, 'buffered')],
            ['perf.fullStream', async () => perfFull(rxId, size, 'stream')],
            ['perf.fullNoReuse', async () => perfFull(rxId, size, 'noreuse')]
        ];
        uploading = true; // keep the dashboard from starting a transfer meanwhile
        try {
            for (const [key, run] of legs) {
                formStatus(st, t('perf.running', { leg: t(key) }), 'ok');
                try {
                    perfRow(t(key), await run());
                } catch (e) {
                    formStatus(st, t('perf.failed', { leg: t(key), why: e.message }), 'err');
                    return;
                }
            }
            formStatus(st, t('perf.done'), 'ok');
        } finally {
            uploading = false;
            renderPerfSelect();
        }
    });

    // Factory reset (typed RESET, checked server-side)
    $('resetWord').addEventListener('input', () => {
        $('resetBtn').disabled = $('resetWord').value.trim() !== 'RESET';
    });
    $('resetBtn').addEventListener('click', async () => {
        if ($('resetWord').value.trim() !== 'RESET') return;
        const st = $('resetStatus');
        $('resetBtn').disabled = true;
        try {
            const res = await fetch('/factory_reset', {
                method: 'POST',
                headers: authHeaders({ 'Content-Type': 'application/json' }),
                body: JSON.stringify({ confirm: 'RESET', remove_machines: $('resetMachines').checked })
            });
            if (res.ok) {
                const d = await res.json();
                signingOut = true; // keep this message up; polls now get 401
                formStatus(st, t('sys.resetDone', { ssid: d.ssid }), 'ok');
                sessionStorage.removeItem('token');
            } else {
                formStatus(st, t('sys.resetFailed', { why: await errorText(res) }), 'err');
            }
        } catch (e) {
            formStatus(st, t('sys.resetFailed', { why: t('msg.fail.network') }), 'err');
        }
        $('resetWord').value = '';
    });

    // ─── Start ──────────────────────────────────────────────────────────
    route();
    renderAll();
    loadReceivers();
    setInterval(loadReceivers, 4000);
    pollStatus();
    startStatusPolling();
    loadReceiverFiles();
});
