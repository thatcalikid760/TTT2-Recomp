import struct, sys
data = open("re/decrypted/Default_patched_tu3.bin", "rb").read()
for t in sys.argv[1:]:
    needle = struct.pack(">I", int(t, 16))
    i = data.find(needle)
    while i >= 0:
        print("%s found at %08X" % (t, 0x82000000 + i))
        # print the surrounding table entries
        start = max(0, (i & ~3) - 0x60)
        words = struct.unpack(">%dI" % (0xC0 // 4), data[start:start + 0xC0])
        print("   " + " ".join("%08X" % w for w in words))
        i = data.find(needle, i + 1)
