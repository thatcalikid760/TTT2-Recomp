"""Prototype the PC menu edits on a dumped BX01 menu document (development only).

Writes C:/ttt2/port/menu_override/<fnv1a of the original>.bin, which the runtime substitutes when
TTT2_MENU_OVERRIDE=1 (kernel/menu_ext.cpp). The shipped edits are done by the runtime itself;
this script is for fast iteration on layout and behaviour. Edits (see port/MENU_PLAN.md):
  - Display Settings: DISPLAY MODE + RESOLUTION rows before DEFAULT (scripts cloned from MOTION
    BLUR, natives renamed), value lists, help texts renumbered, OnLoad icon arrays 5 -> 7;
  - main menu: STORE -> EXIT opening FrameExitTheGameDialog01 (cloned from the return-to-title
    confirmation), visibility through lib::IsExitMenuItemVisible.

  python port/tools/menu_proto.py C:/ttt2/port/menu_bin/007_82346088_unnamed
"""
import copy
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bx01  # noqa: E402

OUT = Path("C:/ttt2/port/menu_override")
RESOLUTIONS = ["1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"]   # = display_settings.cpp
MODES = ["WINDOWED", "BORDERLESS", "FULLSCREEN"]


def fnv1a(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def by_name(nodes, name):
    for n in bx01.walk(nodes):
        if n.get("name") == name:
            return n
    raise KeyError(name)


def parent_of(nodes, child):
    for n in bx01.walk(nodes):
        if any(c is child for c in n.children):
            return n
    raise KeyError(child.get("name"))


def event(node, name):
    return [c for c in node.children if c.name == name][0]


def set_attr(node, key, value, ai):
    for i, (k, v) in enumerate(node.attrs):
        if k == key:
            if value is None:
                del node.attrs[i]
                del node.attr_index[i]
            else:
                node.attrs[i] = (k, value.encode("utf-8") + b"\0")
            return
    if value is not None:
        node.attrs.append((key, value.encode("utf-8") + b"\0"))
        node.attr_index.append(ai[key])


def xtal_rename(blob, renames):
    """Rename identifiers (0x0E) / string constants (0x0B) of a compiled Xtal blob."""
    for old, new in renames.items():
        hit = False
        for tag in (b"\x0e", b"\x0b"):
            pat = tag + struct.pack(">I", len(old)) + old.encode()
            if pat in blob:
                blob = blob.replace(pat, tag + struct.pack(">I", len(new)) + new.encode())
                hit = True
        assert hit, f"{old!r} not in script"
    return blob


def xtal_extend_arrays(blob, per_const, size):
    """Grow each array literal of a straight-line OnLoad script to `size` elements.

    An array is `42 01 02` (make), then per element `02 00 <k>` (load constant k) + `43 01 02`
    (append), every element after the first preceded by `02`: elements = occurrences of
    02 00 k 43 01 02, one more = the unit 02 02 00 k 43 01 02 after the last. Code size at
    blob[8:12]; the debug tail stores the last pc (code size - 1)."""
    code_size = struct.unpack_from(">I", blob, 8)[0]
    code, tail = blob[12:12 + code_size], blob[12 + code_size:]
    for k in per_const:
        unit = bytes([0x02, 0x02, 0x00, k, 0x43, 0x01, 0x02])
        have = code.count(unit[1:])
        assert have <= size, f"array of constant {k} already has {have} elements"
        last = code.rfind(unit)
        code = code[:last + 7] + unit * (size - have) + code[last + 7:]
    new_size = len(code)
    old_last, new_last = struct.pack(">I", code_size - 1), struct.pack(">I", new_size - 1)
    assert tail.count(old_last) == 1, "debug tail pc not unique"
    tail = tail.replace(old_last, new_last)
    return blob[:8] + struct.pack(">I", new_size) + code + tail


def main():
    src = Path(sys.argv[1]).read_bytes()
    nodes, elems, attrs, _ = bx01.parse(src)
    ai = {v: k for k, v in attrs.items()}

    # ---- Display Settings
    menu = by_name(nodes, "MenuMode007-04-01")
    frame = parent_of(nodes, menu)
    # Rows cloned from 3D DEPTH: its OnTrigger passes the value cursor as an int,
    #   if (lib::DisplaySettingIsMonitor3DCapable()) {
    #     lib::MenuOptionDisplay3DPallarax(lib::MenuSelectValueGetCursor(sv));      // preview, setter(int)
    #     lib::DisplaySettingSet3DDepth(lib::MenuSelectValueGetCursor(sv).to_i()); // store, setter(int)
    #   }
    # (MOTION BLUR's passes `cursor == 1` as a bool). Natives renamed to the PC ones, same lengths.
    depth = by_name(nodes, "MenuItemMode007-04-04")
    default = by_name(nodes, "MenuItemMode007-04-05")
    rows = []
    for n, label, natives in ((6, "DISPLAY MODE", ("MenuOptionDisplayWindowMode", "DisplaySettingSetWinMode")),
                              (7, "RESOLUTION", ("MenuOptionDisplayResolution", "DisplaySettingSetDispRes"))):
        row = copy.deepcopy(depth)
        set_attr(row, "name", f"MenuItemMode007-04-0{n}", ai)
        set_attr(row, "text", label, ai)
        set_attr(row, "textid", None, ai)
        set_attr(row, "value", f"SelectValueMenuItemMode007-04-0{n}", ai)
        trig = event(row, "OnTrigger")
        trig.text = xtal_rename(trig.text, {"DisplaySettingIsMonitor3DCapable": "DisplaySettingIsPCDisplayCapable",
                                            "MenuOptionDisplay3DPallarax": natives[0],
                                            "DisplaySettingSet3DDepth": natives[1],
                                            "SelectValueMenuItemMode007-04-04": f"SelectValueMenuItemMode007-04-0{n}"})
        rows.append(row)
    i = menu.children.index(default)
    menu.children[i:i] = rows
    onload = event(menu, "OnLoad")
    onload.text = xtal_extend_arrays(onload.text, per_const=(1, 2), size=7)

    sv = by_name(nodes, "SelectValueMenuItemMode007-04-02")
    after = by_name(nodes, "SelectValueMenuItemMode007-04-04")
    for n, values in ((7, RESOLUTIONS), (6, MODES)):          # inserted after -04: -06 ends first
        new = copy.deepcopy(sv)
        set_attr(new, "name", f"SelectValueMenuItemMode007-04-0{n}", ai)
        items = []
        for k, value in enumerate(values):
            item = copy.deepcopy(sv.children[0])
            item.text = value.encode() + b"\0"
            item.attrs, item.attr_index = [], []
            if k == 0:
                set_attr(item, "default", "true", ai)
            items.append(item)
        new.children = items
        frame.children.insert(frame.children.index(after) + 1, new)

    desc_default = by_name(nodes, "StaticMode007-04-05")
    for n, text in ((5, "Play in a window, a borderless window or exclusive fullscreen."),
                    (6, "Set the resolution the game renders at.")):
        d = copy.deepcopy(desc_default)
        set_attr(d, "name", f"StaticMode007-04-0{n}", ai)
        set_attr(d, "text", text, ai)
        set_attr(d, "textid", None, ai)
        frame.children.insert(frame.children.index(desc_default), d)
    set_attr(desc_default, "name", "StaticMode007-04-07", ai)

    # ---- Main menu: STORE -> EXIT
    main_menu = by_name(nodes, "MenuMainMenu")
    main_frame = parent_of(nodes, main_menu)
    store = by_name(nodes, "MenuItemMode009")
    set_attr(store, "text", "EXIT", ai)
    set_attr(store, "textid", None, ai)
    set_attr(store, "textid360", None, ai)
    enter = event(store, "OnEnter")
    enter.text = xtal_rename(event(main_frame, "OnCancel").text,
                             {"FrameReturnToTitleDialog": "FrameExitTheGameDialog01",
                              "SoundIdMenuCancel": "SoundIdMenuDecide"})
    desc = by_name(nodes, "StaticMode009")
    set_attr(desc, "text", "Exit the game.", ai)
    set_attr(desc, "textid", None, ai)
    set_attr(desc, "textid360", None, ai)
    focus = event(main_menu, "OnFocus")
    focus.text = xtal_rename(focus.text, {"IsOnlineStoreUnlocked": "IsExitMenuItemVisible"})

    # ---- Exit confirmation: a copy of the return-to-title dialog
    rtt = by_name(nodes, "FrameReturnToTitleDialog")
    dlg = copy.deepcopy(rtt)
    set_attr(dlg, "name", "FrameExitTheGameDialog01", ai)
    f = event(dlg, "OnFocus")
    assert f.text.count(b"\x07\x00\x00\x06\x34") == 1
    f.text = f.text.replace(b"\x07\x00\x00\x06\x34", b"\x07\x00\x00\x7f\x01")      # message id 0x634 -> 0x7F01
    t = event(dlg, "OnTick")
    t.text = xtal_rename(t.text, {"WindowGetValue": "ExitGameWindow"})
    top = nodes[0].children
    top.insert(top.index(rtt) + 1, dlg)

    out = bx01.serialize(src, nodes)
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{fnv1a(src):08X}.bin"
    path.write_bytes(out)
    check, _, _, _ = bx01.parse(out)
    print(f"{path}: {len(src)} -> {len(out)} bytes, re-parse ok ({sum(1 for _ in bx01.walk(check))} nodes)")


if __name__ == "__main__":
    main()
