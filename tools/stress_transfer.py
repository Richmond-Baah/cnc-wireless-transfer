#!/usr/bin/env python3
"""Stress test: repeated uploads through the transmitter, as the web UI does them.

    FX_ADMIN_PW=... tools/stress_transfer.py --sends 60 --big 3 --log out.jsonl

The password comes only from FX_ADMIN_PW and is never printed or logged. Prints
one line per send and stops the run after --max-fail failed sends. A restart is a
change of more than 5 s in the receiver's start time. Test files are written to
the root of the drive under names of their own and deleted at the end.
"""
import argparse, hashlib, http.client, json, os, subprocess, sys, time

CHUNK = 32 * 1024
RETRIES = 3
SMALL = 4440912  # the file that crashed
BIG = 50 * 1024 * 1024


class Tx:
    def __init__(self, host, user, password):
        self.host, self.user, self.password, self.token, self.conn = host, user, password, None, None

    def req(self, method, path, headers=None, body=None, timeout=30):
        for attempt in (1, 2):
            h = dict(headers or {})
            if self.token:
                h["Authorization"] = "Bearer " + self.token
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection(self.host, timeout=timeout)
                self.conn.timeout = timeout
                if self.conn.sock:
                    self.conn.sock.settimeout(timeout)
                self.conn.request(method, path, body=body, headers=h)
                r = self.conn.getresponse()
                data = r.read()
            except (OSError, http.client.HTTPException) as e:
                self.close()
                if attempt == 2:
                    raise ConnectionError(type(e).__name__)
                time.sleep(1)
                continue
            if r.status == 401 and path != "/login" and attempt == 1:
                self.login()
                continue
            return r.status, data
        raise ConnectionError("unreachable")

    def close(self):
        if self.conn:
            self.conn.close()
        self.conn = None

    def login(self):
        self.token = None
        st, data = self.req("POST", "/login", {"Content-Type": "application/json"},
                            json.dumps({"username": self.user, "password": self.password}))
        if st != 200:
            sys.exit("login failed: HTTP %d" % st)
        self.token = json.loads(data)["token"]

    def status(self, rx):
        st, data = self.req("GET", "/status", {"X-Rx-Id": rx})
        return json.loads(data) if st == 200 else {}


def text(data):
    return data.decode("utf-8", "replace")[:160]


def send(tx, rx, name, payload):
    """One upload. Returns (ok, stage, detail, seconds, chunk retries)."""
    t0 = time.time()
    begin = {"Content-Type": "application/json", "X-Rx-Id": rx}
    body = json.dumps({"name": name, "size": len(payload)})
    busy_until = t0 + 15  # the laptop may have just read the drive
    while True:
        st, data = tx.req("POST", "/upload_begin", begin, body)
        if st != 423:
            break
        if time.time() > busy_until:
            begin["X-Force"] = "1"
        time.sleep(1)
    if st != 200:
        return False, "begin", "HTTP %d %s" % (st, text(data)), time.time() - t0, 0
    session = json.loads(data)["session"]
    retries = 0
    index = 0
    try:
        for off in range(0, len(payload), CHUNK):
            part = payload[off:off + CHUNK]
            for attempt in range(1, RETRIES + 1):
                try:
                    st, data = tx.req("POST", "/upload_chunk",
                                      {"X-Session": session, "X-Chunk-Index": str(index),
                                       "Content-Type": "application/octet-stream"}, part)
                except ConnectionError as e:
                    st, data = -1, str(e).encode()
                if st == 200:
                    break
                retries += 1
                if 400 <= st < 500:
                    raise RuntimeError("chunk %d HTTP %d %s" % (index, st, text(data)))
                if attempt == RETRIES:
                    raise RuntimeError("chunk %d failed %d times: HTTP %d %s" % (index, RETRIES, st, text(data)))
                time.sleep(0.5 * attempt)
            index += 1
    except RuntimeError as e:
        try:
            tx.req("POST", "/upload_cancel", {"X-Session": session}, "")
        except ConnectionError:
            pass
        return False, "chunks", str(e), time.time() - t0, retries
    st, data = tx.req("POST", "/upload_finish", {"X-Session": session, "Content-Type": "application/json"},
                      json.dumps({"md5": hashlib.md5(payload).hexdigest(), "hash_ms": 0}), timeout=120)
    if st != 200:
        return False, "finish", "HTTP %d %s" % (st, text(data)), time.time() - t0, retries
    return True, "done", "", time.time() - t0, retries


def wait_drive_back(tx, rx, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = tx.status(rx)
        if s.get("state") not in (None, "FILE_READY", "RECEIVING", "OFFLINE"):
            return True
        time.sleep(1)
    return False


def host_md5(name, timeout=60):
    """MD5 of the file as the laptop reads it back over USB; None if not readable."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        out = subprocess.run(["lsblk", "-rpno", "NAME,TRAN,MODEL,MOUNTPOINT"], capture_output=True, text=True).stdout
        for mnt in [l.split(" ", 3)[3] for l in out.splitlines() if len(l.split(" ", 3)) == 4 and l.split(" ", 3)[3]]:
            mnt = mnt.replace("\\x20", " ")
            path = os.path.join(mnt, name)
            if mnt.startswith(("/media/", "/run/media/")) and os.path.exists(path):
                h = hashlib.md5()
                with open(path, "rb") as f:
                    for block in iter(lambda: f.read(1 << 20), b""):
                        h.update(block)
                return h.hexdigest()
        time.sleep(2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="filexfer.local")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--rx", default="rx-842c94")
    ap.add_argument("--sends", type=int, default=60)
    ap.add_argument("--big", type=int, default=3)
    ap.add_argument("--max-fail", type=int, default=5)
    ap.add_argument("--log", required=True)
    args = ap.parse_args()
    password = os.environ.get("FX_ADMIN_PW")
    if not password:
        sys.exit("FX_ADMIN_PW is not set")

    log = open(args.log, "a", buffering=1)

    def record(**kw):
        kw["t"] = time.strftime("%H:%M:%S")
        log.write(json.dumps(kw) + "\n")
        print(json.dumps(kw), flush=True)

    tx = Tx(args.host, args.user, password)
    tx.login()
    first = tx.status(args.rx)
    boot = time.time() - first["rst_ago_s"]
    record(kind="start", fw=first.get("fw"), tx_fw=first.get("tx_fw"), rst_reason=first.get("rst_reason"),
           brownouts=first.get("brownouts"), coredump=first.get("coredump"), free_bytes=first.get("free_bytes"))
    failed = restarts = 0

    def one(kind, n, name, payload, verify):
        nonlocal boot, failed, restarts
        try:
            ok, stage, detail, secs, retries = send(tx, args.rx, name, payload)
        except ConnectionError as e:
            ok, stage, detail, secs, retries = False, "connection", str(e), 0, 0
        back = wait_drive_back(tx, args.rx) if ok else False
        time.sleep(3 if ok else 45)  # after a failure, give a restarted receiver time to report
        s = tx.status(args.rx)
        new_boot = time.time() - s["rst_ago_s"] if s.get("rst_ago_s", -1) >= 0 else boot
        restarted = abs(new_boot - boot) > 5
        if restarted:
            restarts += 1
            boot = new_boot
        rec = dict(kind=kind, n=n, ok=ok, stage=stage, detail=detail, secs=round(secs, 1), retries=retries, drive_back=back,
                   restarted=restarted, rst_reason=s.get("rst_reason"), brownouts=s.get("brownouts"),
                   coredump=s.get("coredump"), rssi=s.get("rssi"))
        if ok and verify:
            got = host_md5(name)
            rec["sent_md5"] = hashlib.md5(payload).hexdigest()
            rec["host_md5"] = got
            rec["md5_match"] = got == rec["sent_md5"]
            ok = rec["md5_match"]
        if not ok:
            failed += 1
        record(**rec)

    names = []
    for i in range(1, args.sends + 1):
        name = "STRESS42.NC"
        if name not in names:
            names.append(name)
        one("small", i, name, os.urandom(SMALL), False)
        if failed >= args.max_fail:
            break
    if failed < args.max_fail:
        for i in range(1, args.big + 1):
            name = "STRESS50.BIN"
            if name not in names:
                names.append(name)
            one("big", i, name, os.urandom(BIG), True)
            if failed >= args.max_fail:
                break
    if names:
        st, data = tx.req("POST", "/receiver_delete", {"X-Rx-Id": args.rx, "X-Force": "1", "Content-Type": "application/json"},
                          json.dumps({"files": names}), timeout=60)
        record(kind="cleanup", status=st, reply=text(data))
    last = tx.status(args.rx)
    record(kind="end", failed=failed, restarts=restarts, rst_reason=last.get("rst_reason"), brownouts=last.get("brownouts"),
           coredump=last.get("coredump"))
    sys.exit(1 if failed or restarts else 0)


if __name__ == "__main__":
    main()
