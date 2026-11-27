"""Private, disposable host v1 catalogs. Invoked by run_m04_host.py --data."""
import ctypes as C
import hashlib
import itertools
import math
import os
from pathlib import Path
import re
import sqlite3
import statistics
import struct
import subprocess as sp
import sys
import time

import psutil

root, driver = map(Path, sys.argv[1:3])
diag = Path(__file__).resolve().parent
dll_path = os.add_dll_directory(str(driver))
api = C.CDLL(str(root / "replay.dll"))
api.startRecord.argtypes = [C.c_int64, C.c_char_p, C.c_int, C.c_int]
api.feedChunk.argtypes = [C.c_char_p, C.c_int]
api.startFile.argtypes = [C.c_wchar_p]
DOC = "eadd0000-0000-4000-8000-000000013792"
CHUNK = 65520
H = struct.Struct("<5If3d4f2d")
P = struct.Struct("<3d3f")
proc = psutil.Process()
clock = time.perf_counter


def need(ok, msg):
    if not ok:
        raise ValueError(msg)


def key(i, n):
    return 3 * (1 + ((7919*i+13792) % n))


def dbfile(n):
    return root / f"canon-{n}.sqlite"


def make(n):
    # Use the actual v1 CREATE TABLE statements, not a faster approximate schema.
    java = (diag.parent / "app/src/main/java/com/infinipaint/storage/DocumentDatabase.java").read_text()
    source = java.split("private static void createSchema")[1].split("db.setVersion")[0]
    db = sqlite3.connect(dbfile(n))
    db.executescript("pragma journal_mode=WAL; pragma synchronous=FULL; pragma foreign_keys=ON;")
    for call in re.findall(r'db\.execSQL\((.*?)\);', source, re.S):
        sql = "".join(re.findall(r'"([^"]*)"', call.replace("CHUNK_BYTES", '"65520"')))
        if sql.startswith("PRAGMA application_id"):
            sql = "pragma application_id=0x49504443"
        db.execute(sql)
    db.execute("pragma user_version=1")
    db.execute("insert into document(uuid,revision,ceiling,count,head,tail) values(?,1,?,?,?,?)",
               (DOC, math.ceil(3*n/65536)*65536, n, key(0,n), key(n-1,n)))
    db.execute("update catalog set current=?", (DOC,))
    for i in range(n):
        x, y = 1024*(i//256)+32*(i%16), 32*((i%256)//16)
        # One offscreen multi-chunk stroke with tied time across the boundary;
        # diagonals cross cells, short pressure paths and post-replay translation.
        count = 1850 if i == n-1 else (65 if i%256 == 0 else 8 if i%32 == 1 else 1)
        h = H.pack(1,1,1,1,2,1.9,1,.007,1,.25,.5,.75,.5,.25,-.5)
        blob = b"".join(P.pack(x+j*(7 if count==65 else .15), y+j*(7 if count==65 else .04)
                              +(.2*math.sin(j) if j else 0),
                              1+(max(0,j-1) if j>=1820 else j)*.005,
                              .3+.1*(j%6),0,0) for j in range(count))
        k = key(i,n)
        db.execute("insert into stroke values(?,?,?,?,1,1,?,?,?)",
                   (DOC,k,key(i-1,n) if i else 0,key(i+1,n) if i+1<n else 0,h,count,
                    hashlib.sha256(h+blob).digest()))
        for pos in range(0,len(blob),CHUNK):
            db.execute("insert into chunk values(?,?,?,?)",(DOC,k,pos//CHUNK,blob[pos:pos+CHUNK]))
    db.commit()
    db.close()


rowcols = "id,prev,next,content_rev,geometry_rev,length(header),substr(header,1,80),samples,length(digest),substr(digest,1,32)"
chunks = "select id,ordinal,length(data),substr(data,1,65520) from chunk where doc=? "


def validate(n, mode, export=False, audit=False):
    db = sqlite3.connect(dbfile(n).as_uri()+"?mode=ro", uri=True)
    # Disposable candidate; never publish a partial root, never update catalog.
    rp = root / f"rank-{mode}.sqlite"
    staging=rp.with_suffix(".partial")
    staging.unlink(missing_ok=True)
    ranks = sqlite3.connect(staging)
    ranks.executescript("pragma journal_mode=WAL; pragma cache_size=-512;"
                        "create table locator(id integer primary key,leaf integer,slot integer);"
                        "create table root(uuid,session,revision);begin")
    spath = root / "spans.bin.partial"
    need(api.startFile(str(spath) if export else None), "open span spool")
    stats = (C.c_uint64*3)()
    base = proc.memory_info().rss
    peak = base
    start = clock()
    io = proc.io_counters()
    native_seconds = 0
    payload = 0
    try:
        db.executescript("pragma cache_size=-512; pragma mmap_size=0; pragma query_only=1; begin")
        need(db.execute("pragma application_id").fetchone()==(0x49504443,) and
             db.execute("pragma user_version").fetchone()==(1,), "format")
        need(db.execute("select current from catalog where singleton=1").fetchone()==(DOC,), "catalog")
        rev,limit,head,tail,count,seq,x,y,z = db.execute(
            "select revision,ceiling,head,tail,count,view_seq,pan_x,pan_y,zoom from document where uuid=?",(DOC,)).fetchone()
        need(rev>=0 and limit>=0 and seq>=0 and math.isfinite(x) and math.isfinite(y)
             and .1<=z<=16 and count>=0, "meta")
        need(db.execute("select count(*) from stroke where doc=?",(DOC,)).fetchone()==(count,), "count")
        need(not db.execute("pragma foreign_key_check").fetchone(),"orphan")

        def links(with_payload):
            cur,prev = head,0
            for rank in range(count):
                cols = rowcols if with_payload else "id,prev,next"
                row = db.execute(f"select {cols} from stroke where doc=? and id=?",(DOC,cur)).fetchone()
                need(0<cur<=limit and row and row[1]==prev, "reciprocal link")
                ranks.execute("insert into locator values(?,?,?)",(cur,rank//128,(rank%128)*2))
                prev,cur=cur,row[2]
                yield row
            need(cur==0 and prev==tail,"tail")

        if mode=="scan":
            for _ in links(False):
                pass
            rows=db.execute(f"select {rowcols} from stroke where doc=? order by id",(DOC,))
            cur=db.execute(chunks+"order by id,ordinal",(DOC,))
            groups=itertools.groupby(cur,key=lambda c:c[0])
        else:
            rows=links(True)
        for idx,(k,_,__,content,geom,hlen,h,samples,dlen,digest) in enumerate(rows):
            need(0<geom<=content<=rev and 1<=samples<=1000000 and hlen==80 and dlen==32,"record")
            t=clock()
            need(api.startRecord(k,h,samples,audit), "header or replay capacity")
            native_seconds+=clock()-t
            sha=hashlib.sha256(h)
            if mode=="scan":
                gid,pieces=next(groups,(None,None))
                need(gid==k,"missing chunk group")
            else:
                pieces=db.execute(chunks+"and id=? order by ordinal",(DOC,k))
            rem=samples*36
            for ordinal,(_,o,length,data) in enumerate(pieces):
                need(rem>0 and o==ordinal and length==min(CHUNK,rem),"chunk shape")
                t=clock()
                need(api.feedChunk(data,length),"numerical domain or reconstruction")
                native_seconds+=clock()-t
                sha.update(data)
                rem-=length
            need(rem==0 and sha.digest()==digest,"digest/length")
            t=clock()
            need(api.endRecord(),"terminal replay / full-v1 oracle")
            native_seconds+=clock()-t
            payload+=80+samples*36
            if idx%1024==0 or samples>1820:
                peak=max(peak,proc.memory_info().rss)
        if mode=="scan":
            need(next(groups,None) is None,"extra chunk group")
        need(api.stopFile(stats),"spool close")
        ranks.execute("insert into root values(?,1,?)",(DOC,rev))
        ranks.commit()
        ranks.close()
        staging.replace(rp)
        if export:
            spath.replace(root/"spans.bin")
        print(f"open n={n} mode={mode} audit={audit} s={clock()-start:.3f} native_s={native_seconds:.3f} "
              f"proc_read_B={proc.io_counters().read_bytes-io.read_bytes} wire={payload} "
              f"spans/quads={stats[0]}/{stats[1]} traceHigh={stats[2]} rssBase/sampledPeak={base}/{peak}",flush=True)
    finally:
        api.stopFile(stats)
        db.close()
        ranks.close()
        staging.unlink(missing_ok=True)
        spath.unlink(missing_ok=True)


def faults(n):
    # Mutate only this generated copy and restore each injected fault.
    db=sqlite3.connect(dbfile(n))
    last=key(n-1,n)
    for name in ("digest","backlink","ordinal","time","recipe","revision","format"):
        src=root/"backup.sqlite"
        save=sqlite3.connect(src);db.backup(save);save.close()
        if name=="digest":
            db.execute("update stroke set digest=zeroblob(32) where id=?",(last,))
        elif name=="backlink":
            db.execute("update stroke set prev=0 where id=?",(last,))
        elif name=="ordinal":
            db.execute("update chunk set ordinal=2 where id=? and ordinal=1",(last,))
        elif name in ("time","recipe"):
            h=db.execute("select header from stroke where id=?",(last,)).fetchone()[0]
            a,b=[r[0] for r in db.execute("select data from chunk where id=? order by ordinal",(last,))]
            if name=="time":
                b=b[:16]+struct.pack("<d",0)+b[24:]
                db.execute("update chunk set data=? where id=? and ordinal=1",(b,last))
            else:
                h=h[:8]+struct.pack("<I",2)+h[12:]
            db.execute("update stroke set header=?,digest=? where id=?",(h,hashlib.sha256(h+a+b).digest(),last))
        elif name=="revision":
            db.execute("update stroke set geometry_rev=2 where id=?",(last,))
        else:
            db.execute("pragma user_version=2")
        db.commit()
        prior=(root/"spans.bin").read_bytes()
        for mode in ("keyed","scan"):
            try:
                validate(n,mode,True)
            except ValueError as e:
                print("rejected",name,mode,e)
            else:
                raise AssertionError(name)
            need((root/"spans.bin").read_bytes()==prior,"partial publication")
        save=sqlite3.connect(src);save.backup(db);save.close();src.unlink()
    db.close()
    validate(n,"scan",True,True)


def order(n):
    db=sqlite3.connect(root/"rank-scan.sqlite")
    db.execute("pragma cache_size=-512")
    hits=[q[0] for q in struct.iter_unpack("<q",(root/"packed_hits.bin").read_bytes())]
    expected=sorted(hits,key=lambda k:((k//3-1-13792)*pow(7919,-1,n))%n)
    def sort(ids,token):
        need(db.execute("select * from root").fetchone()==token,"stale root")
        out=[]
        for k in ids:
            pos=db.execute("select leaf,slot from locator where id=?",(k,)).fetchone()
            need(pos is not None,"missing id")
            out.append((*pos,k))
        return [k for *_,k in sorted(out)]
    token=(DOC,1,1)
    def rejected(ids,t):
        try:
            sort(ids,t)
        except ValueError:
            return
        raise AssertionError("bad order accepted")
    rejected(hits,(DOC,2,1));rejected(hits+[0],token)
    times=[]
    for i in range(32):
        now=clock();out=sort(hits,token);times.append((clock()-now)*1e6)
        need(out==expected,"sparse paint order")
    # Single atomic splice into a reserved slot; not arbitrary relabel/undo.
    first,last=key(0,n),key(n-1,n)
    with db:
        db.execute("update locator set leaf=0,slot=1 where id=?",(last,))
        db.execute("update root set revision=2")
    rejected(hits,token)
    need(sort([last,first],(DOC,1,2))==[first,last],"splice")
    print(f"order n={n} k={len(hits)} firstUs={times[0]:.1f} warmUsMedian={statistics.median(times[1:]):.1f} "
          f"rankSpliceWalBytes={Path(str(root/'rank-scan.sqlite')+'-wal').stat().st_size}")
    db.close()


if __name__=="__main__":
    if sys.argv[3:]==["controls"]:
        make(1000)
        validate(1000,"keyed",True,True)
        faults(1000)
    elif len(sys.argv)>3:
        n=int(sys.argv[3]);mode=sys.argv[4]
        validate(n,mode,mode=="keyed",n==1000)
    else:
        print("sqlite",sqlite3.sqlite_version)
        for n in (1000,10000,100000):
            make(n)
            for mode in ("keyed","scan"):
                sp.run([sys.executable,"-u",__file__,root,driver,str(n),mode],check=True)
            if n==1000:
                faults(n)
                # Restore link-order export after sequential fault recovery.
                validate(n,"keyed",True)
            print("files n",n,"canonical",dbfile(n).stat().st_size,"rank",(root/"rank-scan.sqlite").stat().st_size)
            sp.run([root/"tree.exe",root/"spans.bin",root],check=True)
            order(n)
