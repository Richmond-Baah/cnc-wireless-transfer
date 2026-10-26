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
    'login.unreachable': 'Can\'t reach the transmitter. Check you are on its Wi-Fi.'
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
