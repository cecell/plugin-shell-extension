"""patterncheck.py <master.db> <patterndump.exe> <patterns.json> <plugin dir> [plugin ...]

Checks the C++ pattern engine against a reference database built by the
Rust populator: runs patterndump over each plugin and compares every decoded
subrecord with the database's packed per-subrecord column (and the
record_header column) of the same record version.

The database stores the engine's value after three transforms, which this
script reproduces (master-db-populator/src/pass2.rs):
  * a top-level "[LSTRING:0xN]" in a column with an lstring class becomes
    the st_lstrings row id (declared class first, then strings, ilstrings,
    dlstrings; id 0 -> NULL; no row -> the text is kept);
  * every "[XXXXXXXX]" string at any depth becomes the st_formids row id of
    the form it names (raw 0 -> NULL). The row ids themselves are build
    artefacts, so the check maps the stored id back to its form key
    (owner plugin, local id) and compares that with the raw FormID's;
  * slot_to_sql_value / to_sql_value: repeated subrecords and is_multi
    columns become a JSON array, scalars bind by column type, anything else
    is JSON text.
Columns derived from the packed one (flat struct columns, child tables) are
not compared; they are produced from the same value.
"""
import collections
import decimal
import json
import os
import re
import sqlite3
import subprocess
import sys
import time

FID_RE = re.compile(r"^\[([0-9A-Fa-f]{1,8})\]$")
LS_RE = re.compile(r"^\[LSTRING:0x([0-9A-Fa-f]+)\]$")
LS_FALLBACK = ["strings", "ilstrings", "dlstrings"]


class Fid:
    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = raw


class Lstr:
    __slots__ = ("sid", "text", "lid")

    def __init__(self, sid, text):
        self.sid, self.text = sid, text


def rust_display(f):
    """Rust's f64 Display: shortest round-trip digits, never an exponent."""
    if f != f:
        return "NaN"
    if f in (float("inf"), float("-inf")):
        return "inf" if f > 0 else "-inf"
    s = format(decimal.Decimal(repr(f)), "f")
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return s


def tag_formids(v):
    """pass2's substitute_formid_leaves_in_value, as markers."""
    if isinstance(v, str):
        if not v.startswith("[LSTRING:"):
            m = FID_RE.match(v)
            if m:
                return Fid(int(m.group(1), 16))
        return v
    if isinstance(v, list):
        return [tag_formids(x) for x in v]
    if isinstance(v, dict):
        return {k: tag_formids(x) for k, x in v.items()}
    return v


class Checker:
    def __init__(self, db_path):
        self.con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        c = self.con
        self.catalog = {}
        for rt, fs, col, ty, lc, multi in c.execute(
                "SELECT rt.sig, c.fs_sig, c.sql_column_name, c.sql_type, c.lstring_class, c.is_multi "
                "FROM st_rt_fs_columns c JOIN st_record_types rt ON rt.id = c.fk_record_type "
                "JOIN sp_patterns p ON p.fk_rt_fs = c.id WHERE c.status='active' AND p.pattern_json IS NOT NULL"):
            self.catalog[(rt, fs)] = (col, ty, lc, bool(multi))
        self.col_info = {}  # (rt, col) -> (sql_type, lstring_class, is_multi)
        for (rt, fs), (col, ty, lc, multi) in self.catalog.items():
            self.col_info[(rt, col)] = (ty, lc, multi)
        self.plugins = {fn.lower(): pid for pid, fn in c.execute(
            "SELECT id, filename FROM st_plugins WHERE crc32 IS NOT NULL")}
        self.masters = collections.defaultdict(dict)  # pid -> idx -> master pid
        for pid, idx, mpid in c.execute(
                "SELECT fk_plugin_file, idx, fk_master_file FROM st_plugin_masters WHERE idx >= 0"):
            self.masters[pid][idx] = mpid
        self.num_masters = {pid: n for pid, n in c.execute("SELECT id, num_masters FROM st_plugins")}
        print("loading st_formids ...", flush=True)
        self.form_key = {}  # id -> (origin pid, local24)
        for fid_id, formid, origin in c.execute("SELECT id, formid, fk_origin_plugin FROM st_formids"):
            self.form_key[fid_id] = (origin, formid & 0xFFFFFF)
        print("loading st_lstrings ...", flush=True)
        self.lstr = {}  # (pid, class, sid) -> id
        for lid, pid, cls, sid in c.execute("SELECT id, fk_plugin, class, string_id FROM st_lstrings"):
            self.lstr[(pid, cls, sid)] = lid
        self.table_cols = {}
        self.mismatch = collections.Counter()
        self.samples = collections.defaultdict(list)
        self.cells = 0
        self.records = 0
        self.missing_rows = collections.Counter()

    # -- per plugin -------------------------------------------------------------
    def record_ids(self, pid):
        """(master idx or -1, local24) -> [st_formids.id, ...] recorded by pid."""
        out = collections.defaultdict(collections.deque)
        for fid_id, formid, idx, rt in self.con.execute(
                "SELECT f.id, f.formid, pm.idx, rt.sig FROM st_formids f "
                "JOIN st_plugin_masters pm ON pm.id = f.fk_plugin_master "
                "JOIN st_record_types rt ON rt.id = f.fk_record_type "
                "WHERE pm.fk_plugin_file = ? AND f.is_unresolvable = 0 ORDER BY f.id", (pid,)):
            out[(idx, formid & 0xFFFFFF, rt)].append(fid_id)
        return out

    def db_row(self, rt, fk_form):
        tbl = rt.lower()
        if tbl not in self.table_cols:
            cur = self.con.execute(f'SELECT * FROM "{tbl}" LIMIT 0')
            self.table_cols[tbl] = [d[0] for d in cur.description]
        row = self.con.execute(f'SELECT * FROM "{tbl}" WHERE fk_form = ?', (fk_form,)).fetchone()
        return None if row is None else dict(zip(self.table_cols[tbl], row))

    def owner(self, pid, raw):
        hb = raw >> 24
        n = self.num_masters.get(pid, 0)
        if hb < n and hb in self.masters[pid]:
            return self.masters[pid][hb], raw & 0xFFFFFF
        return pid, raw & 0xFFFFFF

    # -- comparison -------------------------------------------------------------
    def same(self, exp, got, pid, path, problems):
        if isinstance(exp, Fid):
            if exp.raw == 0:
                ok = got is None
            elif isinstance(got, int) and not isinstance(got, bool):
                ok = self.form_key.get(got) == self.owner(pid, exp.raw)
            else:
                ok = got == "[%08X]" % exp.raw  # left unresolved
            if not ok:
                problems.append(f"{path}: formid {exp.raw:08X} -> {got!r} "
                                f"(db key {self.form_key.get(got) if isinstance(got, int) else None}, "
                                f"expected {self.owner(pid, exp.raw)})")
            return
        if isinstance(exp, Lstr):
            if exp.sid == 0:
                ok = got is None
            else:
                want = exp.lid
                ok = got == (want if want is not None else exp.text)
            if not ok:
                problems.append(f"{path}: lstring {exp.sid} -> {got!r}")
            return
        if isinstance(exp, dict):
            if not isinstance(got, dict) or list(exp.keys()) != list(got.keys()):
                problems.append(f"{path}: keys {list(exp.keys())[:8]} vs "
                                f"{list(got.keys())[:8] if isinstance(got, dict) else repr(got)[:80]}")
                return
            for k in exp:
                self.same(exp[k], got[k], pid, f"{path}.{k}", problems)
            return
        if isinstance(exp, list):
            if not isinstance(got, list) or len(exp) != len(got):
                problems.append(f"{path}: list len {len(exp)} vs "
                                f"{len(got) if isinstance(got, list) else repr(got)[:80]}")
                return
            for i, (a, b) in enumerate(zip(exp, got)):
                self.same(a, b, pid, f"{path}[{i}]", problems)
            return
        if type(exp) is not type(got) or exp != got:
            problems.append(f"{path}: {exp!r} vs {got!r}")

    def expected_cell(self, values, sql_type, multi):
        """slot_to_sql_value -> ('json', tree) | ('scalar', value)."""
        if len(values) > 1 or multi:
            return "json", values
        v = values[0]
        if v is None:
            return "scalar", None
        if isinstance(v, (Fid, Lstr)):
            return "marker", v
        if isinstance(v, bool):
            if sql_type == "INTEGER":
                return "scalar", int(v)
            if sql_type == "TEXT":
                return "scalar", "true" if v else "false"
            return "json", v
        if isinstance(v, int):
            if sql_type == "INTEGER":
                return "scalar", v
            if sql_type == "REAL":
                return "scalar", float(v)
            return "scalar", str(v)
        if isinstance(v, float):
            if sql_type == "REAL":
                return "scalar", v
            if sql_type == "INTEGER" and v == v and v not in (float("inf"), float("-inf")) and v == int(v):
                return "scalar", int(v)
            if sql_type == "TEXT":
                return "scalar", rust_display(v)
            return "json", v
        if isinstance(v, str):
            if sql_type == "TEXT":
                return "scalar", v
            if sql_type == "INTEGER":
                return "scalar", None  # unresolved placeholder -> NULL
            return "json", v
        return "json", v

    def compare_cell(self, rt, col, values, cell, pid, fid):
        sql_type, lc, multi = self.col_info.get((rt, col), ("TEXT", None, False))
        kind, exp = self.expected_cell(values, sql_type, multi)
        problems = []
        if kind == "scalar":
            if exp is None:
                ok = cell is None
            elif isinstance(exp, float) or isinstance(cell, float):
                ok = isinstance(cell, (int, float)) and not isinstance(cell, bool) and float(cell) == float(exp)
            else:
                ok = cell == exp
            if not ok:
                problems.append(f"{exp!r} vs {cell!r}")
        elif kind == "marker":
            # A resolved formid/lstring is an Int by now; to_sql_value writes
            # an Int into a TEXT column as its decimal text.
            if sql_type == "TEXT" and isinstance(cell, str) and re.fullmatch(r"-?\d+", cell):
                cell = int(cell)
            self.same(exp, cell, pid, col, problems)
        else:
            if isinstance(cell, str):
                try:
                    got = json.loads(cell)
                except ValueError:
                    got = cell
            else:
                got = cell
            self.same(exp, got, pid, col, problems)
        self.cells += 1
        if problems:
            self.mismatch[(rt, col)] += 1
            if len(self.samples[(rt, col)]) < 3:
                self.samples[(rt, col)].append(f"{fid:08X}: " + "; ".join(problems)[:600])

    def check_record(self, rec, pid, ids):
        rt, fid = rec["t"], rec["f"]
        hb = fid >> 24
        idx = hb if hb < self.num_masters.get(pid, 0) and rt != "TES4" else -1  # the header is the plugin's own
        q = ids.get((idx, fid & 0xFFFFFF, rt))
        if not q:
            self.missing_rows[(rt, "no st_formids row")] += 1
            return
        fk_form = q.popleft()
        row = self.db_row(rt, fk_form)
        if row is None:
            self.missing_rows[(rt, "no table row")] += 1
            return
        self.records += 1
        if rec.get("e"):
            self.mismatch[(rt, "<decode error>")] += 1
            if len(self.samples[(rt, "<decode error>")]) < 3:
                self.samples[(rt, "<decode error>")].append(f"{fid:08X}: {rec['e']}")
        slots = collections.OrderedDict()
        if rec.get("h") is not None and (rt, "Record Header") in self.catalog:
            slots[self.catalog[(rt, "Record Header")][0]] = [tag_formids(rec["h"])]
        for fs, val in rec["s"]:
            col, ty, lc, multi = self.catalog[(rt, fs)]
            if lc and isinstance(val, str):
                m = LS_RE.match(val)
                if m:
                    sid = int(m.group(1), 16)
                    lid = None
                    for cls in [lc] + [c for c in LS_FALLBACK if c != lc]:
                        lid = self.lstr.get((pid, cls, sid))
                        if lid is not None:
                            break
                    marker = Lstr(sid, val)
                    marker.lid = lid
                    slots.setdefault(col, []).append(marker)
                    continue
            slots.setdefault(col, []).append(tag_formids(val))
        for col, values in slots.items():
            self.compare_cell(rt, col, values, row.get(col), pid, fid)
        # Packed columns the database filled but the decode did not produce.
        for col, cell in row.items():
            if cell is None or col in slots or (rt, col) not in self.col_info:
                continue
            self.cells += 1
            self.mismatch[(rt, col)] += 1
            if len(self.samples[(rt, col)]) < 3:
                self.samples[(rt, col)].append(f"{fid:08X}: db has {str(cell)[:200]!r}, decode has nothing")

    def check_plugin(self, exe, patterns, path):
        name = os.path.basename(path)
        pid = self.plugins.get(name.lower())
        if pid is None:
            print(f"{name}: not in the database, skipped")
            return
        ids = self.record_ids(pid)
        expected_rows = sum(len(v) for v in ids.values())
        t = time.time()
        before = self.records
        proc = subprocess.Popen([exe, patterns, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for line in proc.stdout:
            self.check_record(json.loads(line), pid, ids)
        err = proc.stderr.read().decode(errors="replace").strip()
        proc.wait()
        left = sum(len(v) for v in ids.values())
        if left:
            self.missing_rows[("*", f"{name}: db rows never matched")] += left
        print(f"{name}: {self.records - before} records of {expected_rows} db rows, "
              f"{time.time() - t:.1f}s | {err}", flush=True)


def main():
    db, exe, patterns, pdir = sys.argv[1:5]
    names = sys.argv[5:] or sorted(os.listdir(pdir))
    ck = Checker(db)
    for n in names:
        if n.lower().endswith((".esm", ".esp", ".esl")):
            ck.check_plugin(exe, patterns, os.path.join(pdir, n))
    print()
    print(f"records compared {ck.records}, cells compared {ck.cells}, "
          f"cells mismatched {sum(ck.mismatch.values())}")
    for k, n in sorted(ck.missing_rows.items()):
        print(f"MISSING {k}: {n}")
    for key, n in ck.mismatch.most_common():
        print(f"MISMATCH {key[0]}.{key[1]}: {n}")
        for s in ck.samples[key]:
            print(f"    {s}")
    sys.exit(1 if ck.mismatch or ck.missing_rows else 0)


if __name__ == "__main__":
    main()
