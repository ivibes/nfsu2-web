# HD vinyls: making remasters

With `hd_vinyls = 1024` or `2048` (settings.ini, or the Mods tab's HD Vinyls row) the port paints each car's paint
again at that size from remastered textures and shows it in place of the game's 512x512 paint
(`runtime/paintblend.c`, `runtime/hdskin.c`, `runtime/hdgpu.c`). The game itself still paints its own 512 copy; only
the GPU shows the larger one. A car none of whose layers has a remaster keeps the game's own texture.

The remasters come from one pack in the game folder, `TEXPACKS/hdvinyls.pak`, which the server serves to the browser
like the other texture packs (the page fetches it ahead only while the setting is on). Build it from a folder of DDS
files:

    tools/hdvinyls.py <game folder> <remaster folder>        # writes <game folder>/TEXPACKS/hdvinyls.pak
    tools/hdvinyls.py --out <file.pak> <remaster folder>      # anywhere else

Restart the game (the browser: press Play again) after changing the pack.

## One file per texture

- **Name:** the texture's full name as the game knows it, then `.dds`: `SKYLINE_UNIQUE_7_DRAGON.dds` and its mask
  `SKYLINE_UNIQUE_7_DRAGON_MASK.dds`, `SKYLINE_DOORLINE.dds`, `350Z_ART_F_6.dds`. Case does not matter. Each car has
  its own copy of a design (`<CAR>_<DESIGN>`); a remaster replaces only the car it is named for.
- **Format:** uncompressed 32-bit DDS, A8R8G8B8 (the usual "BGRA8" / "ARGB 8888"), A8B8G8R8 or X8R8G8B8, top row first.
  Only level 0 is read; mipmaps in the file are ignored. Compressed DDS (DXT/BC) is refused.
- **Size:** square, 2048x2048 for the best result (4 times the stock 512). One 2048 set serves both settings: at 1024
  the runtime box-filters it down. A 1024 file is scaled up (nearest) for 2048. Other sizes that are not a power of
  two are refused by the tool.
- **Placement:** keep the drawing where the stock texture has it, at the same place on the canvas, only sharper: the
  game maps the texture onto the car by its coordinates, so anything moved lands elsewhere on the body.

## Masks

A vinyl's mask says where it covers the paint: its **green channel**. 0 = the paint shows through; 1 to 127 = the
vinyl mixes in at value/128; 128 and up = the vinyl fully covers, as the game's blend does. A grey mask works (green
is read). Remaster a design's mask with its art, at the same size, or the stock mask's blocky edge stays.

The art's colour counts wherever the mask is above 0, so carry the colours a few pixels past the mask's edge: black
under a soft edge shows as a dark fringe.

## Colours: plain vinyls and colourisable ones

The game has two kinds of layer:

- **Plain** (doorlines, sponsor logos, most Art Factory designs): the vinyl's pixels are the colours you see. Draw the
  final art.
- **Colourisable** (the designs the player recolours in the shop): the vinyl is a **weight map**, not art. Its red,
  green and blue are how much of the player's first, second and third colour each pixel gets. A remaster of such a
  vinyl must be a weight map like the stock texture, or the shop's colours stop working. Start from the stock
  texture's channels and keep their meaning.

Which kind a texture is shows in a blend trace (`NFSU2_BLEND_TRACE=1`, `runtime/paintblend.c`): `ncol=00000001` is
colourisable, `00000000` plain.

## Checking

The log has one `[hd-vinyls]` line per paint: `remastered=N` counts the textures taken from the pack (a vinyl and its
mask are two). `skipped` means no layer of that paint has a remaster. A file the runtime cannot use is named once
("refused") and the stock texture is used instead. For development, `NFSU2_HD_VINYL_DIR=<folder>` reads the DDS files
straight from a folder (they come before the pack's), with no pack to build.
