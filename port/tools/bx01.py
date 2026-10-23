"""Decode / re-encode TTT2's BX01 binary-XML menu documents.

Layout (from the game's reader sub_82A8C170 / sub_82A8BFB0, all big-endian u32):
  header  "BX01" | version u32 | elementNameTable offset | attributeNameTable offset
  body    node list = count, then per element:
            nameIndex, len, text[len] (padded to 4)    - text is the element's content
            attrCount, then per attribute: nameIndex, len, value[len] (padded to 4)
            child node list (recursive)
  tables  count, then per name: offset, length, index; then the NUL-terminated strings
Event elements (OnFocus, OnLeft, ...) carry compiled Xtal code ("xtal" blobs) as their text.
serialize() rebuilds a document from (possibly edited) nodes; parse+serialize is byte-identical
for every menu document the game loads.

  python port/tools/bx01.py menu.bin [--find Mode007-04] [--out menu.xml]
"""
import argparse
import struct
from pathlib import Path


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def name_table(b, off):
    n = u32(b, off)
    entries = [struct.unpack_from(">III", b, off + 4 + 12 * i) for i in range(n)]
    strings = off + 4 + 12 * n
    names = {}
    for so, ln, idx in entries:
        names[idx] = b[strings + so: strings + so + ln].split(b"\0")[0].decode("latin-1")
    return names


class Node:
    def __init__(self, name, text, attrs, children, offset, index=None, attr_index=None):
        self.name, self.text, self.attrs, self.children, self.offset = name, text, attrs, children, offset
        self.index = index                    # element name index
        self.attr_index = attr_index or []    # attribute name index per attribute

    def get(self, key):
        for k, v in self.attrs:
            if k == key:
                return v.rstrip(b"\0").decode("latin-1")
        return None


def read_list(b, p, elems, attrs):
    count = u32(b, p)
    p += 4
    nodes = []
    for _ in range(count):
        start = p
        ni, ln = u32(b, p), u32(b, p + 4)
        text = b[p + 8: p + 8 + ln]
        p += 8 + ((ln + 3) & ~3)
        ac = u32(b, p)
        p += 4
        al, ail = [], []
        for _ in range(ac):
            ai, vl = u32(b, p), u32(b, p + 4)
            al.append((attrs.get(ai, f"attr{ai}"), b[p + 8: p + 8 + vl]))
            ail.append(ai)
            p += 8 + ((vl + 3) & ~3)
        children, p = read_list(b, p, elems, attrs)
        nodes.append(Node(elems.get(ni, f"elem{ni}"), text, al, children, start, ni, ail))
    return nodes, p


def parse(b):
    assert b[:4] == b"BX01", "not a BX01 document"
    elems, attrs = name_table(b, u32(b, 8)), name_table(b, u32(b, 12))
    nodes, end = read_list(b, 16, elems, attrs)
    return nodes, elems, attrs, end


def write_list(nodes):
    out = bytearray(struct.pack(">I", len(nodes)))
    for n in nodes:
        out += struct.pack(">II", n.index, len(n.text)) + n.text + b"\0" * (-len(n.text) % 4)
        out += struct.pack(">I", len(n.attrs))
        for (k, v), ai in zip(n.attrs, n.attr_index):
            out += struct.pack(">II", ai, len(v)) + v + b"\0" * (-len(v) % 4)
        out += write_list(n.children)
    return out


def serialize(original, nodes):
    """Rebuild a document: same header fields and name tables, new body."""
    elem_off, attr_off = u32(original, 8), u32(original, 12)
    body = write_list(nodes)
    new_elem = 16 + len(body)
    head = original[:8] + struct.pack(">II", new_elem, new_elem + (attr_off - elem_off))
    return bytes(head + body + original[elem_off:])


def show_value(v):
    v = v.rstrip(b"\0")
    if v.startswith(b"xtal"):
        return f"[xtal {len(v)} bytes]"
    try:
        return v.decode("utf-8")
    except UnicodeDecodeError:
        return v.decode("latin-1")


def dump(nodes, out, depth=0):
    for n in nodes:
        attrs = " ".join(f'{k}="{show_value(v)}"' for k, v in n.attrs)
        text = n.text.rstrip(b"\0")
        body = "" if not text.strip() else show_value(text)
        out.append(f"{'  ' * depth}<{n.name}{(' ' + attrs) if attrs else ''}> @{n.offset:X} {body[:120]!r}")
        dump(n.children, out, depth + 1)


def walk(nodes):
    for n in nodes:
        yield n
        yield from walk(n.children)


def find(nodes, needle, path=()):
    for n in nodes:
        here = path + (n,)
        if any(needle in show_value(v) for _, v in n.attrs):
            yield here
        yield from find(n.children, needle, here)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--find")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    b = args.file.read_bytes()
    nodes, elems, attrs, end = parse(b)
    print(f"elements: {elems}\nattributes: {attrs}\nbody ends at {end:X}")
    out = []
    if args.find:
        for path in find(nodes, args.find):
            top = path[0] if len(path) < 2 else path[1]
            dump([top], out)
            break
    else:
        dump(nodes, out)
    text = "\n".join(out)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)


if __name__ == "__main__":
    main()
