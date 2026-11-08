#!/usr/bin/env python3
"""Stress test: repeated uploads through the transmitter, as the web UI does them.

    FX_ADMIN_PW=... tools/stress_transfer.py --sends 60 --big 3 --log out.jsonl

The password comes only from FX_ADMIN_PW and is never printed or logged. Prints
one line per send and stops the run after --max-fail failed sends. A restart is a
change of more than 5 s in the receiver's start time.

Each send has its own name (S<run><n>.NC, B<run><n>.BIN) and logs its source
MD5, busy_s (waiting out the receiver's busy check, 423) and xfer_s (from the
accepted /upload_begin to /upload_finish). Big files are read back from the
laptop's mount of the drive (mounted with udisksctl if needed, unmounted again)
and compared. Small files are deleted every 20 sends and the rest at the end,
except the last big file, which is kept for a later host-side check; --keep
keeps every test file.
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
    """One upload. Returns a dict: ok, stage, detail, secs, busy_s, xfer_s, forced, retries."""
    t0 = time.time()
    res = dict(ok=False, stage="begin", detail="", secs=0, busy_s=0, xfer_s=0, forced=False, retries=0)

    def done(ok, stage, detail=""):
        now = time.time()
        res.update(ok=ok, stage=stage, detail=detail, secs=round(now - t0, 1),
                   xfer_s=round(now - t1, 1) if t1 else 0)
        return res

    t1 = None
    begin = {"Content-Type": "application/json", "X-Rx-Id": rx}
    body = json.dumps({"name": name, "size": len(payload)})
    busy_until = t0 + 15  # the laptop may have just read the drive
    while True:
        st, data = tx.req("POST", "/upload_begin", begin, body)
        if st != 423:
            break
        if time.time() > busy_until:
            begin["X-Force"] = "1"
            res["forced"] = True
        time.sleep(1)
    t1 = time.time()
    res["busy_s"] = round(t1 - t0, 1)
    if st != 200:
        return done(False, "begin", "HTTP %d %s" % (st, text(data)))
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
                res["retries"] = retries
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
        return done(False, "chunks", str(e))
    st, data = tx.req("POST", "/upload_finish", {"X-Session": session, "Content-Type": "application/json"},
                      json.dumps({"md5": hashlib.md5(payload).hexdigest(), "hash_ms": 0}), timeout=120)
    if st != 200:
        return done(False, "finish", "HTTP %d %s" % (st, text(data)))
    return done(True, "done")


def wait_drive_back(tx, rx, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = tx.status(rx)
        if s.get("state") not in (None, "FILE_READY", "RECEIVING", "OFFLINE"):
            return True
        time.sleep(1)
    return False


def drive_parts():
    """(partition, mountpoint or "") of the FileXfer USB drive."""
    out = subprocess.run(["lsblk", "-rpno", "NAME,TYPE,MODEL,MOUNTPOINT"], capture_output=True, text=True).stdout
    disk, parts = None, []
    for line in out.splitlines():
        f = line.split(" ")
        if len(f) >= 3 and f[1] == "disk":
            disk = f[0] if f[2].replace("\\x20", " ") == "FileXfer" else None
        elif len(f) >= 2 and f[1] == "part" and disk and f[0].startswith(disk):
            parts.append((f[0], f[3].replace("\\x20", " ") if len(f) > 3 else ""))
    return parts


def host_md5(name, timeout=60):
    """MD5 of the file as the laptop reads it back over USB; None if not readable.
    Mounts the drive with udisksctl if it is not mounted, and unmounts it again."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for part, mnt in drive_parts():
            mounted_here = False
            if not mnt:
                r = subprocess.run(["udisksctl", "mount", "-b", part], capture_output=True, text=True)
                mnt = next((p for p in drive_parts() if p[0] == part), ("", ""))[1]
                mounted_here = r.returncode == 0 and bool(mnt)
            path = os.path.join(mnt, name) if mnt else ""
            try:
                if path and os.path.exists(path):
                    h = hashlib.md5()
                    with open(path, "rb") as f:
                        for block in iter(lambda: f.read(1 << 20), b""):
                            h.update(block)
                    return h.hexdigest()
            finally:
                if mounted_here:
                    subprocess.run(["udisksctl", "unmount", "-b", part], capture_output=True)
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
    ap.add_argument("--keep", action="store_true", help="keep every test file on the drive")
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
    run = "%02X" % (int(time.time()) // 60 % 256)  # names unique per send and per run
    pending = []  # sent test files not yet deleted

    def delete(names):
        for i in range(0, len(names), 40):  # the receiver takes up to 48 per request
            st, data = tx.req("POST", "/receiver_delete",
                              {"X-Rx-Id": args.rx, "X-Force": "1", "Content-Type": "application/json"},
                              json.dumps({"files": names[i:i + 40]}), timeout=60)
            record(kind="cleanup", status=st, reply=text(data))

    def one(kind, n, name, payload, verify):
        nonlocal boot, failed, restarts
        sent_md5 = hashlib.md5(payload).hexdigest()
        try:
            r = send(tx, args.rx, name, payload)
        except ConnectionError as e:
            r = dict(ok=False, stage="connection", detail=str(e), secs=0, busy_s=0, xfer_s=0, forced=False, retries=0)
        ok = r["ok"]
        pending.append(name)
        back = wait_drive_back(tx, args.rx) if ok else False
        time.sleep(3 if ok else 45)  # after a failure, give a restarted receiver time to report
        s = tx.status(args.rx)
        new_boot = time.time() - s["rst_ago_s"] if s.get("rst_ago_s", -1) >= 0 else boot
        restarted = abs(new_boot - boot) > 5
        if restarted:
            restarts += 1
            boot = new_boot
        rec = dict(kind=kind, n=n, name=name, size=len(payload), sent_md5=sent_md5, **r)
        if ok and r["xfer_s"] > 0:
            rec["kb_s"] = round(len(payload) / 1024 / r["xfer_s"], 1)
        rec.update(drive_back=back, restarted=restarted, rst_reason=s.get("rst_reason"), brownouts=s.get("brownouts"),
                   coredump=s.get("coredump"), rssi=s.get("rssi"))
        if ok and verify:
            got = host_md5(name)
            rec["host_md5"] = got
            rec["md5_match"] = got == sent_md5
            ok = rec["md5_match"]
        if not ok:
            failed += 1
        record(**rec)

    last_big = None
    for i in range(1, args.sends + 1):
        one("small", i, "S%s%03d.NC" % (run, i), os.urandom(SMALL), False)
        if failed >= args.max_fail:
            break
        if not args.keep and len(pending) >= 20:
            delete(pending)
            pending.clear()
    if failed < args.max_fail:
        for i in range(1, args.big + 1):
            name = "B%s%02d.BIN" % (run, i)
            one("big", i, name, os.urandom(BIG), True)
            if last_big and not args.keep:
                delete([last_big])
                pending.remove(last_big)
            last_big = name
            if failed >= args.max_fail:
                break
    if last_big in pending:
        pending.remove(last_big)  # kept for a later host-side check
    if pending and not args.keep:
        delete(pending)
    last = tx.status(args.rx)
    record(kind="end", failed=failed, restarts=restarts, kept=last_big if not args.keep else "all",
           rst_reason=last.get("rst_reason"), brownouts=last.get("brownouts"), coredump=last.get("coredump"))
    sys.exit(1 if failed or restarts else 0)


if __name__ == "__main__":
    main()
