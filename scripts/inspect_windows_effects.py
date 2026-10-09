"""Snapshot sprite rendering state in GD 2.2081's object layer (read-only)."""
import argparse, ctypes, json, os, struct
p=argparse.ArgumentParser();p.add_argument('pid',type=int);args=p.parse_args()
libc=ctypes.CDLL(None,use_errno=True)
libc.ptrace.argtypes=[ctypes.c_uint,ctypes.c_uint,ctypes.c_void_p,ctypes.c_void_p]
libc.ptrace.restype=ctypes.c_long
def trace(op):
    if libc.ptrace(op,args.pid,None,None)==-1:raise OSError(ctypes.get_errno(),'ptrace')
trace(16)
try:
    os.waitpid(args.pid,0)
    mem=os.open(f'/proc/{args.pid}/mem',os.O_RDONLY)
    def read(a,fmt):return struct.unpack(fmt,os.pread(mem,struct.calcsize(fmt),a))
    q=lambda a:read(a,'<Q')[0]
    def children(node):
        array=q(node+208)
        if not array:return []
        data=q(array+56);count=read(data,'<I')[0]
        if count>1000000:raise ValueError('Invalid child count')
        return list(read(q(data+16),'<'+'Q'*count)) if count else []
    play=q(q(0x1406c2ed8)+0x208)
    if not play:raise ValueError('No active PlayLayer')
    layer=q(play+0xfe8);roots=children(layer);samples=[]
    def sprite(node):
        return dict(address=hex(node),vtable=hex(q(node)),texture=hex(q(node+416)),
                    blend=read(node+404,'<II'),shader=hex(q(node+240)),
                    batch=hex(q(node+368)),camera=hex(q(node+192)),grid=hex(q(node+200)))
    for root in roots[-8:]:
        samples.append(dict(root=sprite(root),children=[sprite(c) for c in children(root)[:3]]))
    print(json.dumps(dict(layer=hex(layer),roots=len(roots),samples=samples),indent=2))
    os.close(mem)
finally:trace(17)
