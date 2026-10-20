# Generate a 16x32 monospace bitmap font (ASCII 32..126) from DejaVu Sans Mono (free license).
from PIL import Image, ImageDraw, ImageFont
W,H=16,32
f=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",27)
asc,desc=f.getmetrics()
top=(H-(asc+desc))//2
rows=[]
for c in range(32,127):
    im=Image.new("L",(W,H),0); d=ImageDraw.Draw(im)
    d.text((0,top),chr(c),font=f,fill=255)
    px=im.load()
    for y in range(H):
        v=0
        for x in range(W):
            if px[x,y]>=96: v|=1<<(W-1-x)
        rows.append(v)
out=["/* generated from DejaVu Sans Mono (Bitstream Vera license): 16x32, ASCII 32..126, 32 rows of 16 bits per glyph */",
     "#define QF_W 16","#define QF_H 32","static const unsigned short qfont[95*32] = {"]
for i in range(0,len(rows),8):
    out.append("  "+",".join("0x%04x"%v for v in rows[i:i+8])+",")
out.append("};")
open("qfont.h","w").write("\n".join(out)+"\n")
print(len(rows), asc, desc, top)
