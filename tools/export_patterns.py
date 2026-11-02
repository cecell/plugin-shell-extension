"""export_patterns.py <master.db> <out.json>

Writes the pattern catalog a reference database was built with, in the
working_patterns.json shape ("<RT>.<SUBRECORD>" -> pattern string), so the
C++ engine decodes with exactly the patterns the database used.
"""
import json
import sqlite3
import sys


def main():
    db, out = sys.argv[1], sys.argv[2]
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = con.execute(
        "SELECT rt.sig, c.fs_sig, p.pattern_json FROM st_rt_fs_columns c "
        "JOIN st_record_types rt ON rt.id = c.fk_record_type "
        "JOIN sp_patterns p ON p.fk_rt_fs = c.id "
        "WHERE c.status = 'active' AND p.pattern_json IS NOT NULL ORDER BY rt.sig, c.fs_sig")
    cat = {"_source": db}
    for rt, fs, pat in rows:
        cat[f"{rt}.{fs}"] = pat
    # Which strings file (strings / dlstrings / ilstrings) each localized
    # field's ID refers to.
    cat["_lstrings"] = {f"{rt}.{fs}": lc for rt, fs, lc in con.execute(
        "SELECT rt.sig, c.fs_sig, c.lstring_class FROM st_rt_fs_columns c "
        "JOIN st_record_types rt ON rt.id = c.fk_record_type "
        "WHERE c.status = 'active' AND c.lstring_class IS NOT NULL ORDER BY rt.sig, c.fs_sig")}
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cat, f, ensure_ascii=True, indent=1)
    print(f"{len(cat) - 1} patterns -> {out}")


if __name__ == "__main__":
    main()
