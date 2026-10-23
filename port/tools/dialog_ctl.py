"""List or answer Win32 dialogs owned by tekken_recomp.

Message boxes (XamShowMessageBoxUI) render inside the game since 2026-10-12 (sys_ui.h); this
remains for any other native window a run might open.

  python port/tools/dialog_ctl.py                 # list tekken_recomp windows + controls
  python port/tools/dialog_ctl.py --click "Text"  # press the button with this label
"""
import argparse
import ctypes
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, ctypes.c_void_p]
WM_GETTEXT, WM_COMMAND, BN_CLICKED = 0x000D, 0x0111, 0


def text(hwnd):
    buf = ctypes.create_unicode_buffer(8192)
    user32.SendMessageW(hwnd, WM_GETTEXT, 8192, ctypes.cast(buf, ctypes.c_void_p))
    return buf.value


def class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def game_pids():
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq tekken_recomp.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return {int(line.split('","')[1]) for line in out.splitlines() if line.startswith('"tekken_recomp')}


def windows(pids):
    found = []

    def top(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            children = []

            def child(c, _):
                children.append((c, user32.GetDlgCtrlID(c), class_name(c), text(c)))
                return True
            user32.EnumChildWindows(hwnd, EnumProc(child), 0)
            found.append((hwnd, class_name(hwnd), text(hwnd), children))
        return True
    user32.EnumWindows(EnumProc(top), 0)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--click", help="label of the dialog button to press")
    args = parser.parse_args()
    pids = game_pids()
    if not pids:
        raise SystemExit("tekken_recomp.exe is not running")
    wins = windows(pids)
    for hwnd, cls, title, children in wins:
        print(f"WINDOW {hwnd} class={cls} title={title!r}")
        for c, cid, ccls, ctext in children:
            print(f"  id={cid} class={ccls} text={ctext!r}")
    if args.click:
        for hwnd, cls, title, children in wins:
            if cls != "#32770":
                continue
            for c, cid, ccls, ctext in children:
                if ccls == "Button" and ctext.replace("&", "") == args.click:
                    # BM_CLICK is ignored while the dialog is not the active window; send the
                    # button's BN_CLICKED notification (what the dialog procedure handles).
                    user32.SendMessageW(hwnd, WM_COMMAND, (BN_CLICKED << 16) | (cid & 0xFFFF), c)
                    print(f"clicked {args.click!r} in {title!r}")
                    return
        raise SystemExit(f"no dialog button labelled {args.click!r}")


if __name__ == "__main__":
    main()
