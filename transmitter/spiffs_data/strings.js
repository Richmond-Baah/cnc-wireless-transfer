// All UI text, key -> English. Translate by replacing the values; {name}-style
// placeholders are filled by t(key, {name: ...}).
window.STRINGS = {
    'app.name': 'CNC Wireless Transfer',
    'app.title.login': 'Log in',
    'app.title.main': 'Machines',

    // Header
    'hdr.network': 'Network {ssid} · {n} of {max} devices',
    'hdr.theme': 'Theme',
    'hdr.theme.system': 'Use system theme',
    'hdr.theme.light': 'Light theme',
    'hdr.theme.dark': 'Dark theme',
    'hdr.settings': 'Settings',
    'hdr.logout': 'Log out',

    // Login
    'login.heading': 'Log in',
    'login.sub': 'Use the transmitter\'s admin account.',
    'login.user': 'Username',
    'login.pass': 'Password',
    'login.submit': 'Log in',
    'login.bad': 'Wrong username or password.',
    'login.unreachable': 'Can\'t reach the transmitter. Check you are on its Wi-Fi.',

    // Units
    'unit.b': '{n} B',
    'unit.kb': '{n} KB',
    'unit.mb': '{n} MB',
    'unit.gb': '{n} GB',
    'unit.of': '{a} of {b}',

    // Machine list
    'mc.heading': 'Machines',
    'mc.none': 'No machines yet. Use Add machine to connect a receiver.',
    'mc.add': 'Add machine',
    'mc.storage': '{type} · {free} free of {total}',
    'mc.lastSeen.now': 'Last seen just now',
    'mc.lastSeen.min': 'Last seen {n} min ago',
    'mc.lastSeen.h': 'Last seen {n} h ago',
    'mc.lastSeen.never': 'Not seen since the transmitter started',
    'storage.SDCard': 'SD card',
    'storage.InternalFlash': 'Internal memory',
    'storage.unknown': 'Drive',

    // Status pills
    'pill.ready': 'Ready',
    'pill.job': 'Job running',
    'pill.receiving': 'Receiving',
    'pill.cardError': 'Card error',
    'pill.offline': 'Offline',

    // Drive state (sub-line of the selected machine)
    'state.USB_ACTIVE': 'The machine has the drive',
    'state.IDLE': 'Drive ready',
    'state.FILE_READY': 'New file ready',
    'state.RECEIVING': 'Receiving a file',
    'state.CARD_ERROR': 'Card can\'t be read',
    'state.ERROR': 'Error',
    'state.UNKNOWN': 'Waiting for status',
    'state.OFFLINE': 'Offline',
    'tx.fw': 'Firmware {v}',
    'tx.noMachine': 'No machine selected',
    'tx.noMachine.why': 'Choose a machine on the left, or add one.',

    // File chooser
    'file.drop': 'Drop a file here or',
    'file.choose': 'Choose file',
    'file.chooseAnother': 'Choose another file',
    'file.types': 'Machine programs: {list}',
    'file.fits': 'fits ({free} free)',
    'file.tooBig': 'too big ({free} free)',
    'file.oddType': '"{ext}" is not a usual machine program type. It will be sent as it is.',

    // Transfer
    'tx.send': 'Send to {name}',
    'tx.cancel': 'Cancel',
    'tx.cancelling': 'Cancelling after this part…',
    'tx.checking': 'Checking the file…',
    'tx.preparing': 'Getting {name} ready…',
    'tx.uploading': 'Uploading · {progress}',
    'tx.verifying': 'Verifying…',
    'tx.reconnecting': 'Reconnecting the drive…',
    'tx.done': 'Done',
    'step.upload': 'Upload',
    'step.verify': 'Verify',
    'step.reconnect': 'Reconnect drive',
    'step.done': 'Done',

    // Files
    'files.heading': 'Files on {name}',
    'files.headingNone': 'Files',
    'files.cached': 'As of the last transfer · the machine has the drive',
    'files.refresh': 'Refresh',
    'files.deleteN': 'Delete {n} selected',
    'files.colName': 'Name',
    'files.colSize': 'Size',
    'files.selectAll': 'Select all files',
    'files.select': 'Select {name}',
    'files.loading': 'Loading files…',
    'files.empty': 'No files on this drive.',
    'files.offline': '{name} is offline. Its files show when it reconnects.',
    'files.pick': 'Choose a machine to see its files.',
    'files.error': 'Couldn\'t load the file list.',
    'files.truncated': 'There are more files on the drive than shown.',
    'files.confirmDelete': 'Delete {n} file(s) from {name}?',
    'files.confirmBusy': '{name} is reading its drive. Deleting now may stop its job.',
    'files.delete': 'Delete',
    'files.deleteAnyway': 'Delete anyway',
    'files.keep': 'Keep',
    'files.deleted': 'Deleted {n} file(s).',
    'files.skipped': 'Skipped: {list}.',
    'files.deleteFailed': 'Delete failed: {why}',
    'files.netError': 'Network error. Nothing was deleted.',
    'files.wipe': 'Erase all files…',
    'files.wipeConfirm': 'Erase every file on {name}? Type WIPE to confirm.',
    'files.wipeBtn': 'Erase all files',
    'files.wipeBusy': '{name} is reading its drive. Erasing now may stop its job.',
    'files.wipeAnyway': 'Erase anyway',
    'files.wiped': 'All files on {name} were erased.',
    'files.wipeFailed': 'Erase failed: {why}',
    'common.cancel': 'Cancel'
};

window.t = function (key, vars) {
    let s = Object.prototype.hasOwnProperty.call(window.STRINGS, key) ? window.STRINGS[key] : key;
    if (vars) s = s.replace(/\{(\w+)\}/g, (m, k) => (vars[k] !== undefined ? vars[k] : m));
    return s;
};

// Fills [data-t] (text), [data-t-ph] (placeholder), [data-t-aria] (aria-label)
// and [data-t-title] (title) under root.
window.applyStrings = function (root) {
    (root || document).querySelectorAll('[data-t],[data-t-ph],[data-t-aria],[data-t-title]').forEach(el => {
        if (el.dataset.t) el.textContent = t(el.dataset.t);
        if (el.dataset.tPh) el.placeholder = t(el.dataset.tPh);
        if (el.dataset.tAria) el.setAttribute('aria-label', t(el.dataset.tAria));
        if (el.dataset.tTitle) el.title = t(el.dataset.tTitle);
    });
};
