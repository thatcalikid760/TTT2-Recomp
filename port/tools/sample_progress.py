"""Read native progress counters without pausing the game or writing memory.

Frontbuffer resolves are a progress measure, not a certified displayed FPS.
Requires the linker map from the exact running executable (checked by PE stamp).
"""
import argparse
import json
from pathlib import Path
import struct
import time

from inspect_guest import ReadProcess, parse_map, read_config, read_menu_state, read_title_state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--map', required=True, type=Path)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--interval', type=float, default=5)
    args = parser.parse_args()
    if not 0 < args.interval <= args.seconds <= 3600:
        parser.error('Require 0 < interval <= seconds <= 3600')
    root = Path(__file__).resolve().parents[2]
    config = read_config(root/'re/ppc/ppc_config.h')
    linker = parse_map(args.map.read_text(encoding='utf-8-sig'))
    proc = ReadProcess(args.pid)
    try:
        guest = proc.resolve_guest_base(linker)
        fields = {'g_resolvesToFront': '<I', 'g_xaPumped': '<Q', 'g_xaSubmitted': '<Q'}
        addresses = {name: proc.module['base']+linker['symbols'][name] for name in fields}
        # Older maps lack the targeted-wait counter. Resolve anonymous-namespace
        # C++ names too, without requiring it for historical checkpoints.
        for name in ('g_dispatchGeneration', 'g_dispatchNotifications'):
            matches = {rva for symbol, rva in linker['symbols'].items()
                       if symbol == name or symbol.startswith('?'+name+'@')}
            if len(matches) == 1:
                fields[name] = '<Q'
                addresses[name] = proc.module['base']+matches.pop()
        began = time.monotonic()
        previous = None
        while True:
            now = time.monotonic()
            sample = {'elapsed': round(now-began, 3)}
            for name, fmt in fields.items():
                sample[name] = struct.unpack(fmt, proc.read(addresses[name], struct.calcsize(fmt)))[0]
                if previous:
                    bits = struct.calcsize(fmt)*8
                    delta = (sample[name]-previous[name]) % (1 << bits)
                    sample[name+'_per_second'] = round(delta/(now-previous_time), 3)
            read = lambda address, size: proc.read(guest+address, size)
            for label, inspect in [('title', read_title_state), ('menu', read_menu_state)]:
                state = inspect(read, config)
                sample[label] = {key: state[key] for key in
                                 ('status', 'current_mode', 'requested_mode', 'state_name', 'state_clock') if key in state}
            print(json.dumps(sample), flush=True)
            previous, previous_time = sample, now
            remaining = args.seconds-(time.monotonic()-began)
            if remaining <= 0: break
            time.sleep(min(args.interval, remaining))
    finally:
        proc.close()


if __name__ == '__main__':
    main()
