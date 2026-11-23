import sys
from PIL import Image
v=open(sys.argv[1],'rb').read(); out=sys.argv[2]
rows=int(sys.argv[3]) if len(sys.argv)>3 else 64
im=Image.new('1',(240,rows))
for y in range(rows):
    for bx in range(30):
        b=v[y*30+bx]
        for k in range(8):
            im.putpixel((bx*8+k,y),0 if (b>>(7-k))&1 else 1)   # T6963: bit7 = leftmost; 1 = dark
im=im.resize((240*4,rows*4),Image.NEAREST); im.save(out)
