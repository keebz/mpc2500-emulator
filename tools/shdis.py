import sys,struct
from capstone import *
d=open(sys.argv[1],'rb').read(); a=int(sys.argv[2],0); b=int(sys.argv[3],0)
md=Cs(CS_ARCH_SH,CS_MODE_SH4|CS_MODE_LITTLE_ENDIAN)
for i in md.disasm(d[a:b],a):
    note=''
    if i.mnemonic.startswith('mov.l') and 'pc' in i.op_str.lower() or (i.mnemonic in('mov.l','mov.w') and i.op_str.startswith('0x')):
        try:
            t=int(i.op_str.split(',')[0],16)
            if i.mnemonic=='mov.l': note='  ; =%08x'%struct.unpack_from('<I',d,t)[0]
            else: note='  ; =%04x'%struct.unpack_from('<H',d,t)[0]
        except Exception: pass
    print('%05x  %-8s %s%s'%(i.address,i.mnemonic,i.op_str,note))
