"""Convert transparent 128x64 place PNGs to native I4 OTR resources.

Run with Python and Pillow after editing custom-assets/place-labels/*.png,
then build the GenerateO2R target to package the resources.
The game's primitive color supplies the orange tint, just as for places 1-8.
"""
from pathlib import Path
import struct
from PIL import Image

root = Path(__file__).resolve().parents[1]
source = root / 'custom-assets/place-labels'
destination = root / 'assets/textures/local_multiplayer'
destination.mkdir(parents=True, exist_ok=True)
for place in range(9, 17):
    with Image.open(source / f'{place}th-place.png') as image:
        if image.size != (128, 64):
            raise ValueError(f'{place}th-place.png must be 128x64')
        if image.mode != 'RGBA':
            raise ValueError(f'{place}th-place.png must have an alpha channel')
        alpha = image.getchannel('A').tobytes()
    # I4 uses each four-bit intensity value as the texture alpha. Rounded
    # quantization retains the supplied edges without an opaque backdrop.
    values = [(value + 8) // 17 for value in alpha]
    payload = bytes((values[i] << 4) | values[i + 1] for i in range(0, len(values), 2))
    header = struct.pack('<5I', 0, 0x4F544558, 0, 0xDEADBEEF, 0xDEADBEEF) + bytes(44)
    texture = struct.pack('<4I', 5, 128, 64, len(payload))
    (destination / f'hud_{place}th').write_bytes(header + texture + payload)
    print(f'Imported place {place}: 128x64 I4')
