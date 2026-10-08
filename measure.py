import struct,mmap,subprocess,pathlib,json,array,time,resource
BASE=pathlib.Path('/home/pagessin/dev/nodehammer/analysis/fccee/flatbuffers-full')
OUT=pathlib.Path('/tmp/nodehammer-size-investigation/measurements'); OUT.mkdir(exist_ok=True)
def run(name):
 t=time.monotonic(); src=BASE/(name+'.nhb.zst'); raw=OUT/(name+'.nhb')
 with raw.open('wb') as f: subprocess.run(['zstd','-dq','-c',str(src)],stdout=f,check=True)
 with raw.open('rb') as f:
  b=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
  def u32(p): return struct.unpack_from('<I',b,p)[0]
  def u64(p): return struct.unpack_from('<Q',b,p)[0]
  def field(p,i):
   v=p-struct.unpack_from('<i',b,p)[0]; o=struct.unpack_from('<H',b,v+4+2*i)[0] if 4+2*i<struct.unpack_from('<H',b,v)[0] else 0
   return p+o if o else None
  def val(p,i,typ='Q'):
   q=field(p,i); return struct.unpack_from('<'+typ,b,q)[0] if q else 0
  def vec(p,i):
   q=field(p,i)
   if q is None:return (0,0)
   q+=u32(q); return q+4,u32(q)
  def table(p):return p+u32(p)
  def zsize(data):return len(subprocess.run(['zstd','-q','-3','-c'],input=data,stdout=subprocess.PIPE,check=True).stdout)
  root=u32(0); assert b[4:8]==b'NHS9'
  tr,n=vec(root,5); n//=16
  pools=[{}, {}, {}]; refs=[array.array('I') for _ in pools]; blobs=[bytearray() for _ in pools]
  for j in range(n):
   x=b[tr+j*128:tr+(j+1)*128]
   parts=(b''.join(x[k*8:k*8+24] for k in [0,4,8]),x[96:120],b''.join(x[k*8:k*8+8] for k in [3,7,11,15]))
   for k,p in enumerate(parts):
    ix=pools[k].get(p)
    if ix is None: ix=len(pools[k]); pools[k][p]=ix; blobs[k].extend(p)
    refs[k].append(ix)
  split=b''.join(blobs)+b''.join(x.tobytes() for x in refs)
  transforms={'count':n,'linear_unique':len(pools[0]),'translation_unique':len(pools[1]),'last_row_unique':len(pools[2]),'full_raw':n*128,'split_raw':len(split),'full_zstd3':zsize(b[tr:tr+n*128]),'split_zstd3':zsize(split)}
  del pools,refs,blobs,split
  volumes,count=vec(root,8); vc=[array.array('Q') for _ in range(4)]; pc=[array.array('Q'),array.array('I')]; table_bytes=0; pcount=0; shape_types={}
  for j in range(count):
   v=table(volumes+4*j)
   for k,fi in enumerate([0,2,3]):vc[k].append(val(v,fi))
   ds,dn=vec(v,4); vc[3].append(dn)
   vt=v-struct.unpack_from('<i',b,v)[0]; table_bytes+=struct.unpack_from('<H',b,vt+2)[0]+4
   for k in range(dn):
    p=table(ds+4*k); pc[0].append(val(p,1));pc[1].append(val(p,2,'I'))
    vt=p-struct.unpack_from('<i',b,p)[0];table_bytes+=struct.unpack_from('<H',b,vt+2)[0]+4
   pcount+=dn
  shapes,sn=vec(root,7)
  for j in range(sn):
   s=table(shapes+j*4); ty=val(s,1,'B');shape_types[ty]=shape_types.get(ty,0)+1
  current=b''.join(a.tobytes() for a in vc+pc)
  narrow=b''.join(array.array('I',a).tobytes() for a in vc+pc)
  meta=field(root,9); meta+=u32(meta)
  result={'name':name,'raw_bytes':len(b),'compressed_bytes':src.stat().st_size,'transforms':transforms,'volume_count':count,'placements':pcount,'volume_placement_table_bytes_excluding_vtables':table_bytes,'numeric_columns64_raw':len(current),'numeric_columns32_raw':len(narrow),'numeric_columns64_zstd3':zsize(current),'numeric_columns32_zstd3':zsize(narrow),'metadata_counts':[vec(meta,k)[1] for k in range(8)],'shape_types':shape_types,'elapsed_seconds':time.monotonic()-t,'peak_rss_kib':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
  (OUT/(name+'.json')).write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
 raw.unlink()
for name in ['ALLEGRO_o1_v01','IDEA_o1_v03','IDEA_o2_v01']:run(name)
