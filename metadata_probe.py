import mmap,struct,subprocess,pathlib,json
base=pathlib.Path('/home/pagessin/dev/nodehammer/analysis/fccee/flatbuffers-full')
for name in ['ALLEGRO_o1_v01','IDEA_o1_v03','IDEA_o2_v01']:
 raw=pathlib.Path('/tmp/nodehammer-size-investigation/measurements/meta.nhb')
 with raw.open('wb') as f:subprocess.run(['zstd','-dq','-c',str(base/(name+'.nhb.zst'))],stdout=f,check=True)
 with raw.open('rb') as f:
  b=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
  def u32(p):return struct.unpack_from('<I',b,p)[0]
  def field(p,i):
   v=p-struct.unpack_from('<i',b,p)[0];o=struct.unpack_from('<H',b,v+4+2*i)[0] if 4+2*i<struct.unpack_from('<H',b,v)[0] else 0
   return p+o if o else None
  def ref(p):return p+u32(p)
  def vec(p,i):
   q=field(p,i)
   if q is None:return 0,0
   q=ref(q);return q+4,u32(q)
  rt=u32(0); meta=ref(field(rt,9));paths=set();npaths=0;components=0;maxcomponent=0
  for slot in [3,4,5,6,7,9]:
   v,n=vec(meta,slot)
   for i in range(n):
    p,c=vec(ref(v+i*4),0);s=b[p:p+c*8];paths.add(s);npaths+=1;components+=c
    if c:maxcomponent=max(maxcomponent,max(struct.unpack('<'+'Q'*c,s)))
  result={'name':name,'path_vectors':npaths,'unique_paths':len(paths),'path_components':components,'unique_path_components':sum(len(p)//8 for p in paths),'max_component':maxcomponent}
  pathlib.Path('measurements',name+'.metadata.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
 raw.unlink()
