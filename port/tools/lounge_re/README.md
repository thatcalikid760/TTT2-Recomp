# TTT2 asset research (Fighting Lounge 3D spike)

Python tools reading the installed game's archives (`ttt2fs.GAME`); nothing here contains game data.

- `ttt2fs.py`: `nrfo.bin` (each archive's files by name hash and offset) and an index of every `NDXR` model.
- `ndxr.py`: `NDXR` models (the Xbox 360 variant of Namco's NUD, layout after Smash Forge's NUD.cs; big-endian;
  faces are triangle strips with 0xFFFF restarts even with strip flag 0).
- `ntxr.py`: `NTXR` textures (the Xbox 360 NUT, version 0x100): DXT1/3/5 and 32-bit, data right after each header,
  16-bit words byte-swapped, NOT tiled. Each carries the `GIDX` hash the materials name.
- `render.py`: a bind-pose render to PNG, textured with each material's slot 0 (the colour map):
  `python render.py alx.png alx_fac_1p alx_upb_1p alx_lob_1p`. The first run builds `ndxr_index.json` and
  `ntxr_index.json` (all archives scanned, a few minutes).

Findings (2026-10-24): 15,179 NDXR models (characters assembled from `<chr>_fac_1p` / `_upb_1p` / `_lob_1p` and
costume parts; stages from `stg...` pieces), 8,552 `NTXR` textures (NUT), `OMO` (147) and `MOTA` (682) animation
blocks, 6 `VBN` skeletons. Materials: 4 textures each (colour, normal map, a mask, a fourth); some colours are tinted by material values not read
yet (Alex's gloves come out grey). Next: the character skeleton and motions.
