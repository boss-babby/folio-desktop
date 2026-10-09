import urllib.request, time, os
HOST="127.0.0.1"
def port(): return int(os.environ.get("FPORT","8080"))
def send(rec):
    urllib.request.urlopen("http://%s:%d/input"%(HOST,port()), data=rec.encode(), timeout=10).read()
def click(x,y,btn=0,mods=0):
    send("d,%d,%d,%d,%d"%(x,y,btn,mods)); time.sleep(0.05)
    send("u,%d,%d,%d,%d"%(x,y,btn,mods)); time.sleep(0.35)
def key(name,mods=0,text=""):
    r="k,%s,1,%d"%(name,mods)
    if text: r+=","+text
    send(r); time.sleep(0.05); send("k,%s,0,%d"%(name,mods)); time.sleep(0.25)
def resize(w,h): send("r,%d,%d"%(w,h)); time.sleep(1.0)
# --- touch/pen transport: p=down q=move r=up ; type 0=mouse 1=touch 2=pen
def tdown(x,y,pid=1,ty=1,pr=1.0): send("p,%.1f,%.1f,0,0,%d,%d,%.3f"%(x,y,ty,pid,pr))
def tmove(x,y,pid=1,ty=1,pr=1.0): send("q,%.1f,%.1f,0,0,%d,%d,%.3f"%(x,y,ty,pid,pr))
def tup(x,y,pid=1,ty=1):          send("s,%.1f,%.1f,0,0,%d,%d,0.000"%(x,y,ty,pid))
def _frames(n, timeout=25):
    r=urllib.request.urlopen("http://%s:%d/stream"%(HOST,port()), timeout=timeout)
    buf=b""; out=[]
    while len(out)<n:
        c=r.read(16384)
        if not c: break
        buf+=c
        while True:
            a=buf.find(b"\xff\xd8"); b=buf.find(b"\xff\xd9", a+2) if a>=0 else -1
            if a<0 or b<0: break
            out.append(buf[a:b+2]); buf=buf[b+2:]
    r.close(); return out
def cap(path, skip=2):
    f=_frames(skip+1)
    if not f: raise RuntimeError("no frame")
    open(path,"wb").write(f[-1]); return path
