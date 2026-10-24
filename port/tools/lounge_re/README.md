# TTT2 asset research (Fighting Lounge 3D spike)

Python tools reading the installed game's archives (`ttt2fs.GAME`); nothing here contains game data.

- `ttt2fs.py`: `nrfo.bin` (each archive's files by name hash and offset) and an index of every `NDXR` model.
- `ndxr.py`: `NDXR` models (the Xbox 360 variant of Namco's NUD, layout after Smash Forge's NUD.cs; big-endian;
  faces are triangle strips with 0xFFFF restarts even with strip flag 0).
- `render.py`: a flat-shaded bind-pose render to PNG: `python render.py alx.png alx_fac_1p alx_upb_1p alx_lob_1p`.

Findings (2026-10-24): 15,179 NDXR models (characters assembled from `<chr>_fac_1p` / `_upb_1p` / `_lob_1p` and
costume parts; stages from `stg...` pieces), 8,552 `NTXR` textures (NUT), `OMO` (147) and `MOTA` (682) animation
blocks, 6 `VBN` skeletons. Next: NTXR textures, then the character skeleton and motions.
