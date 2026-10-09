#!/usr/bin/env python3
"""Asagi Stream SDK: packs an app folder into an .asagiapp file, or checks a finished one.

    python3 pack_app.py <app folder> [-o <file.asagiapp> | -d <output folder>]
    python3 pack_app.py --check <file.asagiapp>

The app folder holds manifest.json, the app DLL that its "entry" names and usually web/ (the
app's pages, index.html first). The package is a ZIP whose files are stored without compression,
in a fixed order and with fixed dates, so the same folder always gives the same bytes.

Everything Asagi Stream checks when the package is added (platform/AppArchive.hpp and
platform/AppManifest.hpp) is checked here first, plus that the DLL is a 64-bit Windows DLL that
exports asagi_app_get. Hidden files (names starting with ".") and Thumbs.db / desktop.ini are left
out. Without -o the file is named after the app: "Asagi Sample" 0.1.0 -> AsagiSample-0.1.0.asagiapp.

Python 3.8+, standard library only. Exit code 0 = OK, 1 = the folder / package cannot be used.

Copyright (c) 2026 Asagi Studio. All rights reserved.
"""
import argparse
import binascii
import io
import json
import math
import os
import re
import struct
import sys
import tempfile
import zipfile

ABI = 1                  # ASAGI_APP_ABI_VERSION
MAX_BYTES = 64 << 20     # the whole package
MAX_FILES = 2000
MAX_LEGACY_PATHS = 16
MAX_PATH_CHARS = 120     # a file's name inside the package, folders included (WebPath.hpp)
MAX_JSON_DEPTH = 64      # common/Json.hpp
SKIPPED = {"thumbs.db", "desktop.ini"}
DEVICE_NAMES = {"con", "prn", "aux", "nul", "conin$", "conout$", "clock$"}
SEGMENT = re.compile(r"[A-Za-z0-9._-]{1,64}")


class Unusable(Exception):
    """The folder or package cannot be used; the message says why (for the user)."""


# ---- the rules of platform/WebPath.hpp and platform/AppManifest.hpp --------------------------------

def reserved_device_name(base):
    b = base.lower()
    return b in DEVICE_NAMES or (len(b) == 4 and b[:3] in ("com", "lpt") and b[3].isdigit())


def safe_relative_path(p):
    if not p or len(p) > MAX_PATH_CHARS:
        return False
    for seg in p.split("/"):
        if not SEGMENT.fullmatch(seg) or seg[0] == "." or seg[-1] == ".":
            return False
        if reserved_device_name(seg.split(".")[0]):
            return False
    return True


def valid_app_id(s):
    return (isinstance(s, str) and re.fullmatch(r"[a-z0-9-]{1,32}", s) is not None and s[0] != "-" and s[-1] != "-"
            and not reserved_device_name(s))


def valid_entry(s):
    return (isinstance(s, str) and 5 <= len(s) <= 64 and s[0] != "." and re.fullmatch(r"[A-Za-z0-9._-]+", s) is not None
            and s.lower().endswith(".dll"))


def valid_legacy_path(p):
    if not isinstance(p, str) or not 2 <= len(p) <= 64 or p[0] != "/" or "//" in p or ".." in p:
        return False
    if re.fullmatch(r"[A-Za-z0-9._/-]+", p) is None:
        return False
    for r in ("/api", "/apps", "/index.html", "/favicon.ico"):
        if p == r or p.startswith(r + "/"):
            return False
    return True


def _no_duplicates(pairs):
    keys = [k for k, _ in pairs]
    for k in keys:
        if keys.count(k) > 1:
            raise Unusable(f"manifest.json に同じ項目が 2 回あります（{k}）")
    return dict(pairs)


def _finite(text):
    v = float(text)
    if not math.isfinite(v):
        raise Unusable(f"manifest.json に大きすぎる数があります（{text}）")
    return v


def _no_constant(name):
    raise Unusable(f"manifest.json に {name} は書けません")


def _depth(v, d=0):
    if d > MAX_JSON_DEPTH:
        raise Unusable("manifest.json の入れ子が深すぎます")
    for c in (v.values() if isinstance(v, dict) else v if isinstance(v, list) else ()):
        _depth(c, d + 1)


def check_manifest(data):
    """manifest.json bytes -> the parsed manifest (dict). Raises Unusable.

    Never more lenient than Asagi Stream's own reader (common/Json.hpp): duplicate keys, NaN /
    Infinity and numbers too large for a double are refused here."""
    if len(data) > 1 << 20:
        raise Unusable("manifest.json が大きすぎます")
    try:
        m = json.loads(data.decode("utf-8-sig"), object_pairs_hook=_no_duplicates, parse_float=_finite,
                       parse_int=lambda s: int(_finite(s)), parse_constant=_no_constant)
    except (UnicodeDecodeError, ValueError, RecursionError) as e:
        raise Unusable(f"manifest.json を読めません（{e}）")
    _depth(m)
    if not isinstance(m, dict):
        raise Unusable("manifest.json が JSON のオブジェクトではありません")
    if not valid_app_id(m.get("id")):
        raise Unusable("manifest.json の id が使えません（英小文字・数字・- で 32 文字まで、先頭と末尾に - は不可、"
                       "con や nul などの名前も不可）")
    if not valid_entry(m.get("entry")):
        raise Unusable("manifest.json の entry が使えません（.dll のファイル名を書いてください）")
    abi = m.get("abi")
    if isinstance(abi, bool) or not isinstance(abi, (int, float)) or abi != ABI:
        raise Unusable(f"manifest.json の abi は {ABI} にしてください（今は {abi!r}）")
    for key in ("name", "version", "description"):
        if key in m and not isinstance(m[key], str):
            raise Unusable(f"manifest.json の {key} は文字列にしてください")
    legacy = m.get("legacyPaths", [])
    if not isinstance(legacy, list):
        raise Unusable("manifest.json の legacyPaths はパスの配列にしてください")
    if len(legacy) > MAX_LEGACY_PATHS:
        raise Unusable(f"manifest.json の legacyPaths が多すぎます（{MAX_LEGACY_PATHS} 個まで）")
    for p in legacy:
        if not valid_legacy_path(p):
            raise Unusable(f"manifest.json の legacyPaths に使えないパスがあります（{p!r}）")
    dock = m.get("dock", {})
    if not isinstance(dock, dict):
        raise Unusable("manifest.json の dock はオブジェクトにしてください")
    return m


# ---- the DLL -----------------------------------------------------------------------------------------

def dll_info(data):
    """(machine, is_dll, exported names) of a PE file. Raises ValueError when it is not one."""
    if data[:2] != b"MZ":
        raise ValueError("not MZ")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not PE")
    machine, nsec, _, _, _, optsize, chars = struct.unpack_from("<HHIIIHH", data, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    export_rva = struct.unpack_from("<I", data, dirs)[0]
    sections = []
    for i in range(nsec):
        _, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", data, opt + optsize + 40 * i)
        sections.append((va, max(vsize, rawsize), rawptr))

    def at(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError("bad RVA")

    names = []
    if export_rva:
        e = at(export_rva)
        nnames, _, names_rva = struct.unpack_from("<III", data, e + 24)
        table = at(names_rva)
        for i in range(min(nnames, 100000)):
            o = at(struct.unpack_from("<I", data, table + 4 * i)[0])
            names.append(data[o:data.index(b"\0", o)].decode("ascii", "replace"))
    return machine, bool(chars & 0x2000), names


def check_dll(name, data):
    try:
        machine, is_dll, names = dll_info(data)
    except (ValueError, struct.error, IndexError):
        raise Unusable(f"{name} は Windows の DLL ではありません")
    if machine == 0x14C:
        raise Unusable(f"{name} は 32 ビットの DLL です。64 ビット（x64）でビルドしてください")
    if machine != 0x8664:
        raise Unusable(f"{name} は x64 の DLL ではありません（machine 0x{machine:04x}）")
    if not is_dll:
        raise Unusable(f"{name} は DLL ではありません（exe かもしれません）")
    if "asagi_app_get" not in names:
        raise Unusable(f"{name} が asagi_app_get を書き出していません（ASAGI_BUILDING_APP を定義して asagi_app.h を読み込み、"
                       "extern \"C\" で定義してください）")


# ---- packing and checking ----------------------------------------------------------------------------

def collect(folder):
    """{zip name: absolute path} of the files to pack, checked. Raises Unusable."""
    files, skipped = {}, []
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        rel_dir = os.path.relpath(dirpath, folder)
        for f in sorted(filenames):
            rel = f if rel_dir == "." else os.path.join(rel_dir, f).replace(os.sep, "/")
            if f.startswith(".") or f.lower() in SKIPPED:
                skipped.append(rel)
                continue
            path = os.path.join(dirpath, f)
            if os.path.islink(path) or not os.path.isfile(path):
                raise Unusable(f"普通のファイルではありません: {rel}")
            if not safe_relative_path(rel):
                raise Unusable(f"使えないファイル名です: {rel!r}（英数字と . _ - だけ、各部分 64 文字・全体 {MAX_PATH_CHARS} 文字まで、"
                               "先頭や末尾に . は不可、CON や NUL などの名前も不可）")
            files[rel] = path
    lower = {}
    for rel in files:
        if rel.lower() in lower:
            raise Unusable(f"大文字・小文字だけが違う名前があります（Windows では同じ名前です）: {lower[rel.lower()]} / {rel}")
        lower[rel.lower()] = rel
    if len(files) > MAX_FILES:
        raise Unusable(f"ファイルが多すぎます（{MAX_FILES} 個まで）")
    return files, skipped


def check_contents(names, read):
    """Checks a package's file list; read(name) gives a file's bytes. Returns the manifest."""
    by_lower = {n.lower(): n for n in names}
    if "manifest.json" not in by_lower:
        raise Unusable("manifest.json がありません（アプリのフォルダの一番上に置いてください）")
    m = check_manifest(read(by_lower["manifest.json"]))
    entry = by_lower.get(m["entry"].lower())
    if entry is None:
        raise Unusable(f"manifest.json の entry（{m['entry']}）がありません（manifest.json と同じ場所に置いてください）")
    check_dll(entry, read(entry))
    return m


def package_name(m):
    base = re.sub(r"[^A-Za-z0-9._-]", "", m.get("name", "")) or m["id"]
    version = re.sub(r"[^A-Za-z0-9._-]", "", m.get("version", "")) or "0.0.0"
    return f"{base}-{version}.asagiapp"


def pack(folder, out=None, out_dir=None):
    if not os.path.isdir(folder):
        raise Unusable(f"フォルダがありません: {folder}")
    files, skipped = collect(folder)

    def read(name):
        with open(files[name], "rb") as f:
            return f.read()

    m = check_contents(list(files), read)
    warnings = []
    if "web/index.html" not in {n.lower() for n in files}:
        warnings.append("web/index.html がありません（アプリの画面とドックが出ません）")
    if skipped:
        warnings.append("入れなかったファイル: " + ", ".join(skipped))

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 0      # MS-DOS
            info.external_attr = 0x20   # "archive", a plain file
            z.writestr(info, read(name))
    data = buf.getvalue()
    if len(data) > MAX_BYTES:
        raise Unusable(f"パッケージが大きすぎます（{len(data) / 1048576:.1f} MB、64 MB まで）")
    check_package_bytes(data)  # the result passes the same reader rules

    if out is None:
        out = os.path.join(out_dir or ".", package_name(m))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".pack-", dir=os.path.dirname(os.path.abspath(out)))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.chmod(tmp, 0o644)  # mkstemp makes it private
    os.replace(tmp, out)
    return out, m, len(files), len(data), warnings


def read_archive(data):
    """platform/AppArchive.hpp readArchive(), step for step: [(name, offset, size)] of the files
    (folder entries skipped). Raises Unusable with the same reasons."""
    not_package = "アプリのファイル（.asagiapp）ではありません"
    broken = "アプリのファイルが壊れています"
    n = len(data)
    if n > MAX_BYTES:
        raise Unusable("アプリのファイルが大きすぎます（64 MB まで）")
    if n < 22 + 30 or data[:4] != b"PK\x03\x04":
        raise Unusable(not_package)
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    eocd = None
    lowest = n - 22 - 65535 if n > 22 + 65535 else 0
    for i in range(n - 22, lowest - 1, -1):
        if u32(i) == 0x06054B50 and i + 22 + u16(i + 20) == n:
            eocd = i
            break
    if eocd is None:
        raise Unusable(not_package)
    disk, cd_disk, on_disk, entries = u16(eocd + 4), u16(eocd + 6), u16(eocd + 8), u16(eocd + 10)
    cd_size, cd_offset = u32(eocd + 12), u32(eocd + 16)
    if disk or cd_disk or on_disk != entries or entries == 0xFFFF or cd_size == 0xFFFFFFFF or cd_offset == 0xFFFFFFFF:
        raise Unusable("この形の ZIP（分割・ZIP64）には対応していません")
    if entries == 0 or entries > MAX_FILES:
        raise Unusable(broken if entries == 0 else f"ファイルの数が多すぎます（{MAX_FILES} 個まで）")
    if cd_offset > eocd or cd_size > eocd - cd_offset:
        raise Unusable(broken)
    files, names, total = [], set(), 0
    pos, cd_end = cd_offset, cd_offset + cd_size
    for _ in range(entries):
        if pos + 46 > cd_end or u32(pos) != 0x02014B50:
            raise Unusable(broken)
        flags, method, crc = u16(pos + 8), u16(pos + 10), u32(pos + 16)
        packed, size = u32(pos + 20), u32(pos + 24)
        name_len, extra_len, comment_len = u16(pos + 28), u16(pos + 30), u16(pos + 32)
        local = u32(pos + 42)
        if pos + 46 + name_len + extra_len + comment_len > cd_end or name_len == 0:
            raise Unusable(broken)
        raw = data[pos + 46:pos + 46 + name_len]
        name = raw.decode("latin-1")  # only ASCII passes the name rules anyway
        pos += 46 + name_len + extra_len + comment_len
        if flags & 1:
            raise Unusable("暗号化された ZIP には対応していません")
        if name.endswith("/"):
            if size != 0 or not safe_relative_path(name[:-1]):
                raise Unusable(f"使えないフォルダ名があります（{name!r}）")
            continue
        if method != 0:
            raise Unusable("圧縮された ZIP には対応していません（SDK のパック用ツールで作ってください）")
        if not safe_relative_path(name):
            raise Unusable(f"使えないファイル名があります（{name!r}）")
        if packed != size or local > cd_offset or cd_offset - local < 30 or u32(local) != 0x04034B50:
            raise Unusable(broken)
        local_name_len, local_extra_len = u16(local + 26), u16(local + 28)
        start_of_data = local + 30 + local_name_len + local_extra_len
        if (local_name_len != name_len or start_of_data > cd_offset or size > cd_offset - start_of_data
                or data[local + 30:local + 30 + name_len] != raw):
            raise Unusable(broken)
        if binascii.crc32(data[start_of_data:start_of_data + size]) & 0xFFFFFFFF != crc:
            raise Unusable(f"ファイルが壊れています（{name} の CRC が合いません）")
        total += size
        if total > MAX_BYTES:
            raise Unusable("アプリのファイルが大きすぎます（64 MB まで）")
        if name.lower() in names:
            raise Unusable(f"同じ名前のファイルが 2 つあります（{name}）")
        names.add(name.lower())
        files.append((name, start_of_data, size))
    for name, _, _ in files:
        parts = name.lower().split("/")
        for k in range(1, len(parts)):
            if "/".join(parts[:k]) in names:
                raise Unusable(f"ファイルとフォルダの名前がぶつかっています（{'/'.join(parts[:k])}）")
    return files


def check_package_bytes(data):
    """What Asagi Stream checks when the package is added, plus the DLL check. Returns the manifest."""
    files = read_archive(data)
    where = {name: (offset, size) for name, offset, size in files}
    return check_contents(list(where), lambda name: data[where[name][0]:where[name][0] + where[name][1]])


def main(argv):
    ap = argparse.ArgumentParser(description="Asagi Stream のアプリのフォルダを .asagiapp にまとめます")
    ap.add_argument("folder", nargs="?", help="アプリのフォルダ（manifest.json があるところ）")
    ap.add_argument("-o", "--output", help="書き出すファイル（.asagiapp）")
    ap.add_argument("-d", "--output-dir", help="書き出すフォルダ（ファイル名はアプリの名前とバージョンから）")
    ap.add_argument("--check", metavar="FILE", help="できあがった .asagiapp を確かめるだけ")
    a = ap.parse_args(argv)
    try:
        if a.check:
            with open(a.check, "rb") as f:
                m = check_package_bytes(f.read())
            print(f"OK: {a.check}（{m['id']} {m.get('version', '')}）")
            return 0
        if not a.folder:
            ap.error("アプリのフォルダを指定してください")
        out, m, count, size, warnings = pack(a.folder, a.output, a.output_dir)
        for w in warnings:
            print(f"注意: {w}", file=sys.stderr)
        print(f"{out}: {m['id']} {m.get('version', '')}, {count} files, {size / 1024:.0f} KB")
        return 0
    except Unusable as e:
        print(f"エラー: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
