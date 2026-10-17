# Places 9–16

Custom HUD position artwork supplied by Ricky Gameplays. Original transparent
128×64 PNGs are kept here for future editing. They share the original position
textures' canvas size and use the existing viewport scaling and anchoring.

Run `python tools/import_place_labels.py` with Pillow installed to regenerate
the native I4 resources in `assets/textures/local_multiplayer`, then build the
`GenerateO2R` target. The resources are included in `spaghetti.o2r` on every
platform. No ROM extraction is required for these custom textures.

I4 stores the alpha shape; the HUD supplies the tint. Places 9–16 use the
existing 8th-place orange, and finished positions keep the native color cycle.
