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

    if (configBtn) {
        configBtn.addEventListener('click', () => {
            settingsModal.classList.add('show');
            fetch('/config', { headers: { 'Authorization': 'Bearer ' + token } })
                .then(r => r.json())
                .then(data => {
                    document.getElementById('wifiMode').value = data.wifi_mode || 'AP';
                    document.getElementById('apSsid').value = data.ap_ssid || '';
                    document.getElementById('apPass').value = '';
                    document.getElementById('staSsid').value = data.sta_ssid || '';
                    document.getElementById('staPass').value = '';

                    document.getElementById('receiverIp').value = data.receiver_ip || '';
                    document.getElementById('authUser').value = data.auth_user || '';
                    document.getElementById('authPass').value = '';
                }).catch(e => console.error('Config fetch error:', e));
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
                    settingsFeedback.textContent = "Settings saved. Device restarting...";
                    settingsFeedback.className = "feedback-msg success";
                    settingsFeedback.style.display = 'block';
                    sessionStorage.removeItem('token');
                    setTimeout(() => { window.location.href = 'index.html'; }, 5000);
                } else {
                    settingsFeedback.textContent = "Failed to save settings.";
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
                const fwEl = document.getElementById('fwVersions');
                if (fwEl) {
                    fwEl.textContent = 'Firmware: transmitter ' + (data.tx_fw || '-') +
                        ' | receiver ' + (data.fw || '-');
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
                    html += `<li class="d-flex justify-between align-center">
                        <span class="file-name">${escapeHtml(f.name)}</span>
                        <span class="file-size text-muted">${formatBytes(f.size)}</span>
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
    let currentXhr = null;

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

        uploadBtn.addEventListener('click', startUpload);
        
        cancelUploadBtn.addEventListener('click', async () => {
            currentFile = null;
            if (currentXhr) {
                currentXhr.abort();
                currentXhr = null;
            }
            try {
                await fetch('/upload_cancel', { 
                    method: 'POST',
                    headers: { 'Authorization': 'Bearer ' + token }
                });
            } catch(e) {}
            showFeedback('Upload cancelled.', 'error');
            resetUploadUI();
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
        uploadBtn.disabled = false;
        hideFeedback();
        progressContainer.style.display = 'none';
    }

    function formatBytes(bytes) {
        if (bytes === 0) return '0 Bytes';
        const k = 1024;
        const sizes = ['Bytes', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    function showFeedback(msg, type) {
        uploadFeedback.textContent = msg;
        uploadFeedback.className = 'feedback-msg ' + type;
        uploadFeedback.style.display = 'block';
    }

    function hideFeedback() {
        uploadFeedback.style.display = 'none';
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

    async function startUpload(force) {
        if (!currentFile) return;
        force = (force === true); // click handler passes an Event
        if (forceUploadBtn) forceUploadBtn.style.display = 'none';

        uploadBtn.disabled = true;
        fileInput.disabled = true;
        progressContainer.style.display = 'block';
        document.getElementById('floatingCancel').style.display = 'block';
        hideFeedback();
        clearInterval(statusInterval);

        try {
            // Step 1: Calculate MD5 on client side
            updateProgress(0, 'Calculating MD5 checksum...');
            const md5 = await calculateMD5(currentFile);
            updateProgress(5, 'MD5: ' + md5.substring(0, 8) + '...');

            // Step 2: Upload raw file body with metadata in headers
            // The ESP32 server reads X-Filename, X-MD5, X-Filesize headers
            // and streams the body directly to the receiver.
            updateProgress(5, 'Uploading to transmitter...');
            currentXhr = new XMLHttpRequest();
            const xhr = currentXhr;
            xhr.open('POST', '/upload', true);
            xhr.setRequestHeader('Authorization', 'Bearer ' + token);
            xhr.setRequestHeader('X-Filename', currentFile.name);
            xhr.setRequestHeader('X-Filesize', currentFile.size.toString());
            xhr.setRequestHeader('X-MD5', md5);
            if (force) xhr.setRequestHeader('X-Force', '1');

            xhr.upload.onprogress = (e) => {
                if (e.lengthComputable) {
                    const pct = Math.round((e.loaded / e.total) * 100);
                    updateProgress(pct, 'Uploading... (' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total) + ')');
                }
            };

            xhr.onload = () => {
                if (xhr.status === 200) {
                    updateProgress(100, 'Transfer Complete!');
                    showFeedback('✅ File transferred and verified successfully!', 'success');
                    loadReceiverFiles();
                    pollStatus(); // refresh free/total space
                } else if (xhr.status === 423) {
                    showFeedback('Machine is reading – stop the job or confirm it\'s idle', 'error');
                    busyRetryFile = currentFile;
                    if (forceUploadBtn) forceUploadBtn.style.display = 'block';
                } else if (xhr.status === 503) {
                    showFeedback('❌ Receiver is busy. Please wait a moment and try again.', 'error');
                } else if (xhr.status === 400 && xhr.responseText) {
                    // Receiver's reason (invalid filename, checksum mismatch, ...)
                    showFeedback('Rejected: ' + xhr.responseText, 'error');
                } else if (xhr.status === 400) {
                    showFeedback('❌ Checksum verification failed. Please retry.', 'error');
                } else if (xhr.status === 401) {
                    showFeedback('❌ Session expired. Please login again.', 'error');
                    setTimeout(() => { window.location.href = 'index.html'; }, 2000);
                } else {
                    showFeedback('❌ Transfer failed: ' + (xhr.responseText || 'Unknown error'), 'error');
                }
                resetUploadUI();
                currentXhr = null;
            };

            xhr.onerror = () => {
                showFeedback('❌ Network error during upload. Check Wi-Fi connection.', 'error');
                resetUploadUI();
            };

            xhr.ontimeout = () => {
                showFeedback('❌ Upload timed out. File may be too large or connection lost.', 'error');
                resetUploadUI();
            };

            xhr.timeout = 300000; // 5 minute timeout for large files

            // Send raw file as body (not FormData)
            xhr.send(currentFile);

        } catch (error) {
            showFeedback('❌ Error processing file: ' + error.message, 'error');
            resetUploadUI();
        }
    }

    function updateProgress(percent, text) {
        const p = Math.min(100, Math.max(0, Math.round(percent)));
        progressBar.style.width = p + '%';
        progressPercent.textContent = p + '%';
        progressText.textContent = text;
    }

    function resetUploadUI() {
        currentFile = null;
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
