"""Toggle only Echo's diagnostic renderer switch, restoring it on exit.

Use --paused for a physics-frozen image comparison; otherwise records A/B/A
progress samples. All RVAs must come from the matching package's PDB.
"""
import argparse,json,os,struct,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('pid',type=int)
p.add_argument('--profile',required=True)
for name in ['state','enabled','counters']:
    p.add_argument('--'+name+'-rva',required=True,type=lambda x:int(x,0))
p.add_argument('--output',required=True)
p.add_argument('--paused',action='store_true')
p.add_argument('--window')
p.add_argument('--seconds',type=float,default=4)
a=p.parse_args()
profile=json.load(open(a.profile));assert profile['pid']==a.pid
base=next(i[0] for i in profile['images'] if i[2]=='decks.echo.dll')
mem=os.open(f'/proc/{a.pid}/mem',os.O_RDWR)
address=base+a.enabled_rva
original=os.pread(mem,1,address);assert original in [b'\0',b'\1']
freeze_address=base+a.state_rva+1256
queue_address=base+a.state_rva+1259
original_freeze=os.pread(mem,1,freeze_address)
original_queue=os.pread(mem,1,queue_address)
def read(address,fmt):return struct.unpack(fmt,os.pread(mem,struct.calcsize(fmt),address))[0]
def sample():
    state=base+a.state_rva
    play=read(read(0x1406c2ed8,'<Q')+0x208,'<Q')
    layer=read(play+0xfe8,'<Q') if play else 0
    children=read(layer+208,'<Q') if layer else 0
    data=read(children+56,'<Q') if children else 0
    return dict(time=time.monotonic(),frame=read(state+328,'<i'),
                tps=read(state+348,'<f'),speed=read(state+428,'<f'),
                children=read(data,'<I') if data else 0,
                sprites=read(base+a.counters_rva,'<Q'),draws=read(base+a.counters_rva+8,'<Q'))
result=[]
output=Path(a.output);output.parent.mkdir(parents=True,exist_ok=True)
try:
    if a.paused:
        os.pwrite(mem,b'\1',freeze_address)
        os.pwrite(mem,b'\0',queue_address)
    for i,enabled in enumerate([False,True,False]):
        os.pwrite(mem,bytes([enabled]),address)
        time.sleep(1.2)
        if a.paused:
            assert a.window,'--paused requires --window'
            before=sample();image=output.with_name(output.stem+f'-{i}.png')
            subprocess.run(['import','-window',a.window,str(image)],check=True)
            after=sample();assert before['frame']==after['frame'],'Physics is not frozen'
            result.append(dict(enabled=enabled,image=str(image),sample=after))
        else:
            samples=[];end=time.monotonic()+a.seconds
            while time.monotonic()<end:
                samples.append(sample());time.sleep(.2)
            duration=samples[-1]['time']-samples[0]['time']
            result.append(dict(enabled=enabled,samples=samples,
                               steps_per_second=(samples[-1]['frame']-samples[0]['frame'])/duration))
finally:
    os.pwrite(mem,original,address)
    if a.paused:
        os.pwrite(mem,original_freeze,freeze_address)
        os.pwrite(mem,original_queue,queue_address)
    os.close(mem)
if a.paused:
    from PIL import Image,ImageChops
    def difference(first,second):
        diff=ImageChops.difference(Image.open(first).convert('RGB'),Image.open(second).convert('RGB'))
        values=list(diff.getdata());return dict(pixels=sum(any(v) for v in values),maximum=max(max(v) for v in values),bounds=diff.getbbox())
    summary=dict(native_repeat=difference(result[0]['image'],result[2]['image']),
                 batched_vs_native=difference(result[0]['image'],result[1]['image']))
else:summary=[dict(enabled=r['enabled'],steps_per_second=r['steps_per_second']) for r in result]
output.write_text(json.dumps(dict(comparison=result,summary=summary),indent=2))
print(json.dumps(summary,indent=2))
