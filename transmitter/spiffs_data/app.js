// CNC Wireless Transfer - Client Application Logic
document.addEventListener('DOMContentLoaded', () => {

    // ═══════════════════════════════════════════════════════════════════
    // LOGIN PAGE
    // ═══════════════════════════════════════════════════════════════════
    const loginForm = document.getElementById('loginForm');
    if (loginForm) {
        const token = sessionStorage.getItem('token');
        if (token) {
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
                    msg.textContent = 'Invalid credentials. Try again.';
                    msg.style.display = 'block';
                }
            } catch (err) {
                msg.textContent = 'Server unreachable. Check connection.';
                msg.style.display = 'block';
            }
        });
        return; // Stop further execution on login page
    }

    // ═══════════════════════════════════════════════════════════════════
    // UPLOAD PAGE
    // ═══════════════════════════════════════════════════════════════════
    const token = sessionStorage.getItem('token');
    if (!token) {
        window.location.href = 'index.html';
        return;
    }

    // Single source of truth for accepted machine-program extensions:
    // drives the drop-zone text, the file picker filter and the selection check.
    const VALID_EXTS = ['.nc', '.gcode', '.tap', '.cnc', '.gc', '.ngc', '.pcn', '.plt', '.hnc', '.txt', '.crv', '.sbp', '.dxf', '.svg', '.xcn', '.din', '.cut', '.iso'];
    const fileMessageEl = document.getElementById('fileMessage');
    if (fileMessageEl) fileMessageEl.textContent = 'or drag and drop ' + VALID_EXTS.join(', ');
    const fileInputEl = document.getElementById('fileInput');
    if (fileInputEl) fileInputEl.accept = VALID_EXTS.join(',');

    const STORAGE_LABELS = { 'SDCard': 'SD card', 'InternalFlash': 'Internal flash' };

    // Storage capacities (binary units): KB below 1 MB, else MB/GB with 2 decimals
    function formatStorage(bytes) {
        const kb = (bytes || 0) / 1024;
        if (kb < 1024) return kb.toFixed(kb < 10 ? 2 : 0) + ' KB';
        const mb = kb / 1024;
        return mb >= 1024 ? (mb / 1024).toFixed(2) + ' GB' : mb.toFixed(2) + ' MB';
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    }

    // ─── Logout ─────────────────────────────────────────────────────────
    const logoutBtn = document.getElementById('logoutBtn');
    if (logoutBtn) {
        logoutBtn.addEventListener('click', () => {
            sessionStorage.removeItem('token');
            window.location.href = 'index.html';
        });
    }

    // ─── Settings Modal ─────────────────────────────────────────────────
    const configBtn = document.getElementById('configBtn');
    const closeSettingsBtn = document.getElementById('closeSettingsBtn');
    const settingsModal = document.getElementById('settingsModal');
    const settingsForm = document.getElementById('settingsForm');
    const settingsFeedback = document.getElementById('settingsFeedback');
    let lastStaIp = ''; // from /status, for the post-save hint

    if (configBtn) {
        configBtn.addEventListener('click', () => {
            settingsModal.classList.add('show');
            settingsFeedback.style.display = 'none';
            fetch('/config', { headers: { 'Authorization': 'Bearer ' + token } })
                .then(r => {
                    if (!r.ok) throw new Error('HTTP ' + r.status);
                    return r.json();
                })
                .then(data => {
                    document.getElementById('wifiMode').value = data.wifi_mode || 'AP';
                    document.getElementById('apSsid').value = data.ap_ssid || '';
                    document.getElementById('apPass').value = '';
                    document.getElementById('staSsid').value = data.sta_ssid || '';
                    document.getElementById('staPass').value = '';

                    document.getElementById('receiverIp').value = data.receiver_ip || '';
                    document.getElementById('authUser').value = data.auth_user || '';
                    document.getElementById('authPass').value = '';
                }).catch(e => {
                    settingsFeedback.textContent = 'Could not load current settings (' + e.message + ').';
                    settingsFeedback.className = 'feedback-msg error';
                    settingsFeedback.style.display = 'block';
                });
        });

        closeSettingsBtn.addEventListener('click', () => {
            settingsModal.classList.remove('show');
            settingsFeedback.style.display = 'none';
        });

        settingsForm.addEventListener('submit', async (e) => {
            e.preventDefault();
            const formData = new FormData(settingsForm);
            const data = Object.fromEntries(formData.entries());

            try {
                const res = await fetch('/config', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                        'Authorization': 'Bearer ' + token
                    },
                    body: JSON.stringify(data)
                });

                if (res.ok) {
                    let text = "Settings saved. Device restarting...";
                    if (data.wifi_mode === 'STA') {
                        text += ' On your router, open http://cnc-transfer.local or http://' +
                            (lastStaIp || '<STA IP shown in status after restart>') + '.';
                    }
                    settingsFeedback.textContent = text;
                    settingsFeedback.className = "feedback-msg success";
                    settingsFeedback.style.display = 'block';
                    sessionStorage.removeItem('token');
                    setTimeout(() => { window.location.href = 'index.html'; },
                        data.wifi_mode === 'STA' ? 20000 : 5000);
                } else {
                    let reason = '';
                    try { reason = (await res.text()).trim(); } catch (e2) {}
                    settingsFeedback.textContent = "Failed to save settings" + (reason ? ': ' + reason : '.');
                    settingsFeedback.className = "feedback-msg error";
                    settingsFeedback.style.display = 'block';
                }
            } catch (err) {
                settingsFeedback.textContent = "Network error saving settings.";
                settingsFeedback.className = "feedback-msg error";
                settingsFeedback.style.display = 'block';
            }
        });
    }

    // ─── Status Polling ─────────────────────────────────────────────────
    const statusBadge = document.getElementById('receiverStatus');
    let statusInterval;
    let lastFree = null, lastTotal = 0; // receiver drive space, for the pre-upload size check

    const pollStatus = async () => {
        try {
            const res = await fetch('/status', {
                headers: { 'Authorization': 'Bearer ' + token }
            });
            if (res.ok) {
                const data = await res.json();
                const stateMap = {
                    'IDLE': { text: 'Ready', cls: 'status-ready' },
                    'FILE_READY': { text: 'File Ready', cls: 'status-ready' },
                    'RECEIVING': { text: 'Receiving...', cls: 'status-receiving' },
                    'USB_ACTIVE': { text: 'Ready (USB Linked)', cls: 'status-usb' },
                    'ERROR': { text: 'Error', cls: 'status-error' },
                    'CARD_ERROR': { text: 'Card unreadable – format FAT32 (≤32 GB) on PC or use Format', cls: 'status-error' },
                    'OFFLINE': { text: 'Receiver Offline', cls: 'status-offline' }
                };
                const info = stateMap[data.state] || { text: data.state || 'Unknown', cls: 'status-offline' };
                statusBadge.textContent = info.text;
                statusBadge.className = 'status-badge ' + info.cls;
                const fmtBtn = document.getElementById('formatReceiverBtn');
                if (fmtBtn) fmtBtn.style.display = (data.state === 'CARD_ERROR') ? 'block' : 'none';

                // Update storage info if present
                const stypeEl = document.getElementById('storageTypeText');
                const sfreeEl = document.getElementById('storageFreeText');
                if (stypeEl && data.storage_type) {
                    const label = STORAGE_LABELS[data.storage_type] || data.storage_type;
                    stypeEl.textContent = label + ' (' + formatStorage(data.total_bytes) + ')';
                }
                if (sfreeEl && data.free_bytes !== undefined) {
                    sfreeEl.textContent = formatStorage(data.free_bytes);
                }
                if (typeof data.free_bytes === 'number' && data.state !== 'OFFLINE') {
                    lastFree = data.free_bytes;
                    lastTotal = data.total_bytes || 0;
                    updateSpaceCheck();
                }
                const fwEl = document.getElementById('fwVersions');
                if (fwEl) {
                    fwEl.textContent = 'Firmware: transmitter ' + (data.tx_fw || '-') +
                        ' | receiver ' + (data.fw || '-');
                }
                const staEl = document.getElementById('staInfo');
                if (staEl && data.sta) {
                    const s = data.sta;
                    if (s.ip) lastStaIp = s.ip;
                    let t = '';
                    if (s.state === 'connected') {
                        t = 'Router Wi-Fi: connected, IP ' + s.ip + ' (http://cnc-transfer.local)';
                    } else if (s.state === 'failed') {
                        t = 'Router Wi-Fi: FAILED – ' + (s.reason || 'unknown error') +
                            '. Fix SSID/password in Settings (this hotspot stays up).';
                    } else if (s.state === 'connecting') {
                        t = 'Router Wi-Fi: connecting...' + (s.reason ? ' (last error: ' + s.reason + ')' : '');
                    }
                    staEl.textContent = t;
                    staEl.style.display = t ? 'block' : 'none';
                }
            } else if (res.status === 401) {
                sessionStorage.removeItem('token');
                window.location.href = 'index.html';
            }
        } catch (e) {
            statusBadge.textContent = "Transmitter Offline";
            statusBadge.className = "status-badge status-offline";
        }
    };

    if (statusBadge) {
        pollStatus();
        statusInterval = setInterval(pollStatus, 3000);
    }

    // ─── Receiver File List ────────────────────────────────────────────
    const receiverFileList = document.getElementById('receiverFileList');
    const refreshFilesBtn = document.getElementById('refreshFilesBtn');

    const loadReceiverFiles = async () => {
        if (!receiverFileList) return;
        receiverFileList.innerHTML = '<p class="text-muted">Fetching file list...</p>';
        const dsb = document.getElementById('deleteSelectedBtn');
        if (dsb) dsb.disabled = true; // selection is cleared by the re-render
        try {
            const res = await fetch('/receiver_list', {
                headers: { 'Authorization': 'Bearer ' + token }
            });
            if (res.ok) {
                const data = await res.json();
                // cached = the machine owns the drive; list is the receiver's last snapshot
                const note = document.getElementById('fileListNote');
                if (note) note.style.display = data.cached ? 'block' : 'none';
                if (!data.files || data.files.length === 0) {
                    receiverFileList.innerHTML = '<p class="text-muted">No files on storage.</p>';
                    return;
                }
                let html = '<ul class="file-items-list">';
                data.files.forEach(f => {
                    if (f.type === 'dir') {
                        // Folders: icon, no size
                        html += `<li class="d-flex align-center" style="gap: 8px;">
                        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="flex-shrink: 0; opacity: 0.8;"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"></path></svg>
                        <span class="file-name">${escapeHtml(f.name)}/</span>
                    </li>`;
                        return;
                    }
                    const n = escapeHtml(f.name);
                    html += `<li class="d-flex justify-between align-center" style="gap: 8px;">
                        <input type="checkbox" class="file-select" data-name="${n}" aria-label="Select ${n}">
                        <span class="file-name">${n}</span>
                        <span class="file-size text-muted">${formatBytes(f.size)}</span>
                        <button class="btn btn-danger btn-sm file-delete" data-name="${n}" title="Delete ${n}">Delete</button>
                    </li>`;
                });
                html += '</ul>';
                if (data.truncated) html += '<p class="text-muted">More files on storage than shown.</p>';
                receiverFileList.innerHTML = html;
            } else {
                receiverFileList.innerHTML = '<p class="text-error">Error: Receiver not responding</p>';
            }
        } catch (e) {
            receiverFileList.innerHTML = '<p class="text-error">Network error fetching file list</p>';
        }
    };

    if (refreshFilesBtn) {
        refreshFilesBtn.addEventListener('click', loadReceiverFiles);
        loadReceiverFiles();
    }

    // ─── Delete Files ───────────────────────────────────────────────────
    const deleteSelectedBtn = document.getElementById('deleteSelectedBtn');
    const selectedNames = () => Array.from(receiverFileList.querySelectorAll('.file-select:checked'))
        .map(cb => cb.dataset.name);
    const updateDeleteSelected = () => {
        if (deleteSelectedBtn) deleteSelectedBtn.disabled = selectedNames().length === 0;
    };

    async function deleteFiles(names) {
        if (!names.length) return;
        if (!confirm('Delete ' + names.length + ' file(s) from the receiver?\n\n' + names.join('\n'))) return;
        const send = (force) => {
            const headers = { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' };
            if (force) headers['X-Force'] = '1';
            return fetch('/receiver_delete', { method: 'POST', headers, body: JSON.stringify({ files: names }) });
        };
        if (deleteSelectedBtn) deleteSelectedBtn.disabled = true;
        try {
            let res = await send(false);
            if (res.status === 423 &&
                confirm('Machine is reading – stop the job or confirm it\'s idle.\nDelete anyway?')) {
                res = await send(true);
            }
            if (res.ok) {
                const d = await res.json();
                let msg = 'Deleted ' + (d.deleted || []).length + ' file(s).';
                if (d.skipped && d.skipped.length) {
                    msg += ' Skipped: ' + d.skipped.map(s => s.name + ' (' + s.reason + ')').join(', ') + '.';
                }
                if (typeof d.free_bytes === 'number') {
                    lastFree = d.free_bytes;
                    if (d.total_bytes) lastTotal = d.total_bytes;
                    updateSpaceCheck();
                }
                showFeedback(msg, d.skipped && d.skipped.length ? 'error' : 'success');
            } else if (res.status === 423) {
                showFeedback('Delete cancelled: machine is reading the drive.', 'error');
            } else {
                showFeedback('❌ Delete failed: ' + await errorText(res), 'error');
            }
        } catch (e) {
            showFeedback('❌ Network error deleting files.', 'error');
        }
        loadReceiverFiles();
        pollStatus(); // free space -> re-checks whether the selected file fits
        updateDeleteSelected();
    }

    if (receiverFileList) {
        receiverFileList.addEventListener('change', (e) => {
            if (e.target.classList.contains('file-select')) updateDeleteSelected();
        });
        receiverFileList.addEventListener('click', (e) => {
            const btn = e.target.closest('.file-delete');
            if (btn) deleteFiles([btn.dataset.name]);
        });
    }
    if (deleteSelectedBtn) {
        deleteSelectedBtn.addEventListener('click', () => deleteFiles(selectedNames()));
    }

    // ─── Clear Storage ──────────────────────────────────────────────────
    const clearBtn = document.getElementById('clearReceiverBtn');
    if (clearBtn) {
        const sendWipe = (force) => {
            const headers = { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' };
            if (force) headers['X-Force'] = '1';
            return fetch('/receiver_clear', {
                method: 'POST',
                headers,
                body: JSON.stringify({ confirm: 'WIPE' })
            });
        };
        clearBtn.addEventListener('click', async () => {
            const typed = prompt('Permanently delete ALL files from receiver storage?\nType WIPE to continue:');
            if (typed !== 'WIPE') {
                if (typed !== null) showFeedback('Wipe cancelled: you must type WIPE exactly.', 'error');
                return;
            }

            clearBtn.disabled = true;
            try {
                let res = await sendWipe(false);
                if (res.status === 423 &&
                    confirm('Machine is reading – stop the job or confirm it\'s idle.\nWipe anyway?')) {
                    res = await sendWipe(true);
                }
                if (res.ok) {
                    showFeedback('✅ Receiver storage cleared successfully.', 'success');
                    loadReceiverFiles();
                    pollStatus();
                } else if (res.status === 423) {
                    showFeedback('Wipe cancelled: machine is reading the drive.', 'error');
                } else {
                    showFeedback('❌ Failed to clear receiver storage.', 'error');
                }
            } catch (e) {
                showFeedback('❌ Network error clearing receiver.', 'error');
            } finally {
                clearBtn.disabled = false;
            }
        });
    }

    // Format Receiver Card (shown only on CARD_ERROR)
    const formatBtn = document.getElementById('formatReceiverBtn');
    if (formatBtn) {
        formatBtn.addEventListener('click', async () => {
            const typed = prompt('This ERASES ALL DATA on the receiver card and formats it as FAT32.\nType FORMAT to continue:');
            if (typed !== 'FORMAT') {
                if (typed !== null) showFeedback('Format cancelled: you must type FORMAT exactly.', 'error');
                return;
            }
            formatBtn.disabled = true;
            showFeedback('Formatting – don\'t unplug the receiver (can take up to 2 minutes)...', 'success');
            try {
                const res = await fetch('/receiver_format', {
                    method: 'POST',
                    headers: { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' },
                    body: JSON.stringify({ confirm: 'FORMAT' })
                });
                if (res.ok) {
                    showFeedback('Receiver card formatted.', 'success');
                    pollStatus();
                    loadReceiverFiles();
                } else {
                    showFeedback('Format failed (HTTP ' + res.status + ').', 'error');
                }
            } catch (e) {
                showFeedback('Network error while formatting receiver.', 'error');
            } finally {
                formatBtn.disabled = false;
            }
        });
    }

    // ─── File Selection ─────────────────────────────────────────────────
    const dropZone = document.getElementById('dropZone');
    const fileInput = document.getElementById('fileInput');
    const uploadBtn = document.getElementById('uploadBtn');
    const selectedFileInfo = document.getElementById('selectedFileInfo');
    const selectedFileName = document.getElementById('selectedFileName');
    const selectedFileSize = document.getElementById('selectedFileSize');

    const progressContainer = document.getElementById('progressContainer');
    const progressBar = document.getElementById('progressBar');
    const progressPercent = document.getElementById('progressPercent');
    const progressText = document.getElementById('progressText');
    const uploadFeedback = document.getElementById('uploadFeedback');
    const cancelUploadBtn = document.getElementById('cancelUploadBtn');

    let currentFile = null;

    if (dropZone) {
        dropZone.addEventListener('dragover', (e) => {
            e.preventDefault();
            dropZone.classList.add('is-active');
        });

        dropZone.addEventListener('dragleave', () => {
            dropZone.classList.remove('is-active');
        });

        dropZone.addEventListener('drop', (e) => {
            e.preventDefault();
            dropZone.classList.remove('is-active');
            if (e.dataTransfer.files.length) {
                handleFileSelect(e.dataTransfer.files[0]);
            }
        });

        fileInput.addEventListener('change', (e) => {
            if (e.target.files.length) {
                handleFileSelect(e.target.files[0]);
            }
        });

        uploadBtn.addEventListener('click', () => startUpload(false));

        cancelUploadBtn.addEventListener('click', () => {
            // Takes effect between chunks; startUpload then calls /upload_cancel
            cancelRequested = true;
            cancelUploadBtn.disabled = true;
            progressText.textContent = 'Cancelling after the current chunk...';
        });
    }

    function handleFileSelect(file) {
        const ext = file.name.substring(file.name.lastIndexOf('.')).toLowerCase();

        if (!VALID_EXTS.includes(ext)) {
            if (!confirm('Warning: "' + ext + '" is not a standard machine program extension. Proceed anyway?')) {
                currentFile = null;
                uploadBtn.disabled = true;
                selectedFileInfo.style.display = 'none';
                return;
            }
        }

        currentFile = file;
        selectedFileName.textContent = file.name;
        selectedFileSize.textContent = formatBytes(file.size);
        selectedFileInfo.style.display = 'block';
        hideFeedback();
        progressContainer.style.display = 'none';
        updateSpaceCheck();
    }

    // Same rule as the receiver's /prepare check: size + max(64 KB, 1% of drive)
    let uploading = false;
    let spaceWarningShown = false;
    function spaceNeeded(size, total) {
        return size + Math.max(64 * 1024, Math.floor((total || 0) / 100));
    }
    function spaceMessage(size, free, needed) {
        return 'File is ' + formatBytes(size) + ', drive has ' + formatBytes(free) +
            ' free. Delete about ' + formatBytes(Math.max(0, needed - free)) + ' below.';
    }
    function updateSpaceCheck() {
        if (!uploadBtn || uploading) return;
        if (!currentFile) {
            uploadBtn.disabled = true;
            return;
        }
        const needed = spaceNeeded(currentFile.size, lastTotal);
        const fits = lastFree === null || lastFree >= needed; // unknown: the receiver re-checks
        uploadBtn.disabled = !fits;
        if (!fits) {
            showFeedback(spaceMessage(currentFile.size, lastFree, needed), 'error');
            spaceWarningShown = true;
        } else if (spaceWarningShown) {
            hideFeedback();
            spaceWarningShown = false;
        }
    }

    function formatBytes(bytes) {
        if (bytes === 0) return '0 Bytes';
        const k = 1024;
        const sizes = ['Bytes', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    function showFeedback(msg, type) {
        spaceWarningShown = false; // callers showing the space warning set it again
        uploadFeedback.textContent = msg;
        uploadFeedback.className = 'feedback-msg ' + type;
        uploadFeedback.style.display = 'block';
    }

    function hideFeedback() {
        uploadFeedback.style.display = 'none';
        spaceWarningShown = false;
    }

    // ─── Upload Logic ───────────────────────────────────────────────────
    // Machine-busy retry: keep the refused file so the confirm button can resend it
    const forceUploadBtn = document.getElementById('forceUploadBtn');
    let busyRetryFile = null;
    if (forceUploadBtn) {
        forceUploadBtn.addEventListener('click', () => {
            forceUploadBtn.style.display = 'none';
            if (!busyRetryFile) return;
            currentFile = busyRetryFile;
            busyRetryFile = null;
            startUpload(true);
        });
    }

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

    async function apiPost(path, headers, body) {
        const res = await fetch(path, {
            method: 'POST',
            headers: Object.assign({ 'Authorization': 'Bearer ' + token }, headers),
            body
        });
        if (res.status === 401) {
            sessionStorage.removeItem('token');
            setTimeout(() => { window.location.href = 'index.html'; }, 2000);
        }
        return res;
    }

    async function errorText(res) {
        try {
            return (await res.text()) || ('HTTP ' + res.status);
        } catch (e) {
            return 'HTTP ' + res.status;
        }
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
                const res = await fetch('/status', { headers: { 'Authorization': 'Bearer ' + token } });
                if (res.ok) {
                    const d = await res.json();
                    // RECEIVING may still be the transmitter's cached value from mid-upload
                    if (d.state && d.state !== 'FILE_READY' && d.state !== 'RECEIVING') return d.state;
                }
            } catch (e) { /* keep waiting */ }
        }
        return null;
    }

    async function startUpload(force) {
        if (!currentFile) return;
        force = (force === true); // click handlers may pass an Event
        const file = currentFile;
        if (forceUploadBtn) forceUploadBtn.style.display = 'none';
        cancelRequested = false;
        cancelUploadBtn.disabled = false;

        uploading = true;
        uploadBtn.disabled = true;
        fileInput.disabled = true;
        progressContainer.style.display = 'block';
        document.getElementById('floatingCancel').style.display = 'block';
        hideFeedback();
        clearInterval(statusInterval);

        let stage = 'Checksum';
        let session = null;
        try {
            updateProgress(0, 'Calculating checksum...');
            const md5 = await calculateMD5(file);
            if (cancelRequested) throw new UploadError(stage, 'cancelled');

            stage = 'Starting';
            updateProgress(0, 'Preparing receiver...');
            const beginHeaders = { 'Content-Type': 'application/json' };
            if (force) beginHeaders['X-Force'] = '1';
            const res = await apiPost('/upload_begin', beginHeaders,
                JSON.stringify({ name: file.name, size: file.size, md5 }));
            if (res.status === 423) {
                showFeedback('Machine is reading – stop the job or confirm it\'s idle', 'error');
                busyRetryFile = file;
                if (forceUploadBtn) forceUploadBtn.style.display = 'block';
                resetUploadUI();
                return;
            }
            if (res.status === 507) {
                // Receiver re-checked the real drive: keep the file selected so
                // Start re-enables once enough is deleted below
                const raw = await errorText(res);
                let d = {};
                try { d = JSON.parse(raw); } catch (x) { /* plain text */ }
                if (typeof d.free_bytes === 'number') lastFree = d.free_bytes;
                updateProgress(0, 'Not enough space on the drive');
                showFeedback(typeof d.free_bytes === 'number'
                    ? spaceMessage(d.file_size, d.free_bytes, d.needed_bytes)
                    : 'Not enough space on the drive: ' + raw, 'error');
                spaceWarningShown = true;
                resetUploadUI(true);
                return;
            }
            if (!res.ok) throw new UploadError(stage, await errorText(res), res.status);
            session = (await res.json()).session;

            stage = 'Uploading';
            const total = file.size;
            let acked = 0;
            const t0 = performance.now();
            for (let index = 0, offset = 0; offset < total; index++, offset += CHUNK_SIZE) {
                if (cancelRequested) throw new UploadError(stage, 'cancelled');
                acked = await sendChunk(session, index, file.slice(offset, offset + CHUNK_SIZE));
                const pct = total ? (acked / total) * 100 : 100;
                const secs = (performance.now() - t0) / 1000;
                const rate = secs > 0 ? acked / 1024 / secs : 0;
                updateProgress(pct, 'Uploading ' + Math.floor(pct) + '% (' + formatBytes(acked) + ' / ' +
                    formatBytes(total) + ', ' + rate.toFixed(0) + ' KB/s)');
            }
            if (cancelRequested) throw new UploadError(stage, 'cancelled');

            stage = 'Verifying';
            cancelUploadBtn.disabled = true; // past the point of a clean cancel
            updateProgress(100, 'Verifying...');
            const fin = await apiPost('/upload_finish', { 'X-Session': session }, '');
            session = null; // the transmitter ends the session either way
            if (!fin.ok) throw new UploadError(stage, await errorText(fin), fin.status);

            stage = 'Reconnecting drive';
            updateProgress(100, 'Reconnecting drive...');
            const state = await waitForDriveReconnect(10000);
            updateProgress(100, 'Done');
            showFeedback(state ? '✅ File transferred and verified.'
                               : '✅ File verified. The drive is taking longer than usual to reconnect.', 'success');
            loadReceiverFiles();
        } catch (e) {
            const err = e instanceof UploadError ? e : new UploadError(stage, e.message || String(e));
            if (session) {
                try { await apiPost('/upload_cancel', { 'X-Session': session }, ''); } catch (x) { /* best effort */ }
            }
            if (err.message === 'cancelled') {
                updateProgress(lastPct, 'Cancelled');
                showFeedback('Upload cancelled.', 'error');
            } else {
                updateProgress(lastPct, 'Failed during ' + err.stage);
                showFeedback('❌ Failed during ' + err.stage + ': ' + err.message, 'error');
            }
        }
        resetUploadUI();
        pollStatus(); // refresh state + free/total space
    }

    function updateProgress(percent, text) {
        const p = Math.min(100, Math.max(0, Math.round(percent)));
        lastPct = p;
        progressBar.style.width = p + '%';
        progressPercent.textContent = p + '%';
        progressText.textContent = text;
    }

    function resetUploadUI(keepFile) {
        uploading = false;
        if (keepFile !== true) currentFile = null;
        fileInput.disabled = false;
        uploadBtn.disabled = true;
        document.getElementById('floatingCancel').style.display = 'none';
        statusInterval = setInterval(pollStatus, 3000);
        setTimeout(pollStatus, 1000);
    }

// -- Chunked MD5 (via spark-md5) -----------------------------
    // Read the file in 2 MB slices so we don't load the whole G-code into RAM.
    // Output is a lowercase hex digest that matches the receiver's mbedtls result.
    var MD5_CHUNK_SIZE = 2 * 1024 * 1024;

    function hashSlice(spark, blob) {
        return new Promise(function(resolve, reject) {
            var reader = new FileReader();
            reader.onload = function(e) {
                try {
                    if (typeof spark.append === 'function') {
                        spark.append(e.target.result);
                    } else if (typeof spark.appendBinary === 'function') {
                        var u8 = new Uint8Array(e.target.result);
                        var bin = '';
                        for (var i = 0; i < u8.length; i++) bin += String.fromCharCode(u8[i]);
                        spark.appendBinary(bin);
                    } else {
                        throw new Error('No valid append method on MD5 instance');
                    }
                    resolve();
                } catch (err) { reject(err); }
            };
            reader.onerror = function() { reject(new Error('File read error during MD5 calculation')); };
            reader.readAsArrayBuffer(blob);
        });
    }

    async function calculateMD5(file) {
        const MD5Lib = (typeof window !== 'undefined' && window.SparkMD5) || (typeof self !== 'undefined' && self.SparkMD5) || (typeof SparkMD5 !== 'undefined' ? SparkMD5 : null);
        if (!MD5Lib) {
            throw new Error('SparkMD5 library not loaded. Please refresh the page.');
        }
        var spark = new MD5Lib.ArrayBuffer();
        var offset = 0;
        while (offset < file.size) {
            var end = Math.min(offset + MD5_CHUNK_SIZE, file.size);
            await hashSlice(spark, file.slice(offset, end));
            offset = end;
        }
        return spark.end();
    }
});
