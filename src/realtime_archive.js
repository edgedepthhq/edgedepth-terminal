// Browser-local observed RT sessions. Embedded in index.js so hosted and OSS
// asset delivery share one owner. No network, persistence prompt, or backfill.
(function () {
'use strict';
function archiveWorker() {
    const BUDGET = 256 * 1024 * 1024, TARGET = 1800000, CHUNK = 1024 * 1024;
    const sessions = new Map();
    const request = r => new Promise((resolve, reject) => {
        r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error);
    });
    const complete = tx => new Promise((resolve, reject) => {
        tx.oncomplete = resolve; tx.onerror = tx.onabort = () => reject(tx.error || Error('Storage aborted'));
    });
    let dbPromise;
    function database() {
        if (!dbPromise) dbPromise = new Promise((resolve, reject) => {
            const r = indexedDB.open('edgedepth-rt-observed-v1', 1);
            r.onupgradeneeded = () => {
                r.result.createObjectStore('data');
                const m = r.result.createObjectStore('meta', {keyPath: 'key'});
                m.createIndex('session', 'session'); m.createIndex('created', 'created');
                r.result.createObjectStore('budget');
            };
            r.onsuccess = () => resolve(r.result);
            r.onerror = () => reject(r.error); r.onblocked = () => reject(Error('Storage blocked'));
        });
        return dbPromise;
    }
    async function codec(buffer, decompress) {
        const stream = new Blob([buffer]).stream().pipeThrough(decompress ?
            new DecompressionStream('gzip') : new CompressionStream('gzip'));
        return new Response(stream).arrayBuffer();
    }
    async function metadata(id) {
        const db = await database();
        return request(db.transaction('meta').objectStore('meta').index('session').getAll(id));
    }
    async function remove(id) {
        const db = await database();
        const tx = db.transaction(['data','meta','budget'], 'readwrite'), done = complete(tx);
        const meta = tx.objectStore('meta'), data = tx.objectStore('data'), budget = tx.objectStore('budget');
        let total = (await request(budget.get('bytes'))) || 0;
        await new Promise((resolve, reject) => {
            const r = meta.index('session').openCursor(id);
            r.onerror = () => reject(r.error);
            r.onsuccess = () => { const c = r.result;
                if (!c) { resolve(); return; }
                total -= c.value.bytes; data.delete(c.value.key); c.delete(); c.continue();
            };
        });
        budget.put(Math.max(0,total), 'bytes'); await done;
    }
    async function flush(s) {
        if (!s.used) return;
        const raw = s.raw.slice(0,s.used), min = s.min, max = s.max;
        s.used = 0; s.min = Infinity; s.max = 0;
        const packed = await codec(raw.buffer, false);
        const db = await database();
        const tx = db.transaction(['data','meta','budget'], 'readwrite'), done = complete(tx);
        const meta = tx.objectStore('meta'), data = tx.objectStore('data'), budget = tx.objectStore('budget');
        let total = (await request(budget.get('bytes'))) || 0;
        const row = {key:s.id + ':' + (++s.seq), session:s.id, min, max,
            seq:s.seq, bytes:packed.byteLength + 256, created:Date.now()};
        total += row.bytes;
        // The read/write transaction serializes this origin-wide budget across tabs.
        await new Promise((resolve, reject) => {
            const r = meta.index('created').openCursor();
            r.onerror = () => reject(r.error);
            r.onsuccess = () => { const c = r.result;
                if (!c) { resolve(); return; }
                const v = c.value;
                if (total > BUDGET || (v.session === s.id && v.max < (s.clock || max) - TARGET) || v.created < Date.now()-86400000) {
                    total -= v.bytes; data.delete(v.key); c.delete();
                }
                c.continue();
            };
        });
        data.put(packed,row.key); meta.put(row); budget.put(total,'bytes'); await done;
        s.total = total; s.chunks++;
    }
    async function status(s) {
        const rows = await metadata(s.id);
        postMessage({type:'status', id:s.id, first:rows.length ? Math.min(...rows.map(r=>r.min)) : 0,
            last:rows.length ? Math.max(...rows.map(r=>r.max)) : 0,
            bytes:rows.reduce((n,r)=>n+r.bytes,0), total:s.total || 0, chunks:rows.length});
    }
    function state(id) {
        let s = sessions.get(id);
        if (!s) { s = {id, seq:0, raw:new Float64Array(CHUNK/8), used:0, min:Infinity,max:0,chunks:0}; sessions.set(id,s); }
        return s;
    }
    async function append(m) {
        const s = state(m.id), a = new Float64Array(m.buffer);
        if(Number.isFinite(m.clock))s.clock=m.clock;
        // Records: kind,time,...; depth length=7+2*n, trade length=6, gap length=3.
        for (let p=0;p<a.length;) {
            const n = a[p+2];
            if (!Number.isInteger(n) || n<3 || p+n>a.length || n>s.raw.length) throw Error('Invalid archive record');
            if (s.used+n>s.raw.length || (s.used && a[p+1]-s.min>=5000)) await flush(s);
            s.raw.set(a.subarray(p,p+n),s.used); s.used+=n;
            s.min=Math.min(s.min,a[p+1]); s.max=Math.max(s.max,a[p+1]); p+=n;
        }
        s.clock=Number.isFinite(m.clock) ? m.clock : s.max;
        // Each acknowledged batch is durable, including quiet tails and pause.
        await flush(s); await status(s);
    }
    async function query(m) {
        const s = state(m.id); await flush(s);
        const rows = (await metadata(m.id)).filter(r=>r.max>=m.from-m.step && r.min<=Math.min(m.to,m.cutoff))
            .sort((a,b)=>a.seq-b.seq);
        const db = await database(), trades = [], tradeBins = new Map();
        // A single aggregation bin and a bounded typed output. Never materialize
        // the full archive as per-observation JS objects/maps.
        const out=new Float64Array(2048*2055+120000); let used=0, columns=0;
        let current=null, previous=-Infinity, tradeCount=0, depthCount=0;
        function flushBin() {
            const bin=current;current=null;
            if (!bin || bin.gap || !bin.count) {previous=-Infinity;return;}
            if (++columns>2048) return;
            out.set([1,bin.t,7+bin.values.size*2,bin.boundary || bin.b-previous>m.step ? 1:0,
                bin.bid,bin.ask,bin.values.size],used);used+=7;
            for (const [k,v] of bin.values) {out[used++]=k*m.tick;out[used++]=v/bin.count;}
            previous=bin.b;
        }
        for (const row of rows) {
            const packed = await request(db.transaction('data').objectStore('data').get(row.key));
            if (!packed) {flushBin();previous=-Infinity;continue;}
            const a = new Float64Array(await codec(packed,true));
            for (let p=0;p<a.length;p+=a[p+2]) {
                const kind=a[p], t=a[p+1], n=a[p+2];
                if (t<m.from || t>m.to || t>m.cutoff) continue;
                if (kind===2) {
                    ++tradeCount;trades.push([t,a[p+3],a[p+4],a[p+5]]);
                    const key=Math.floor(t/m.step)*2+(a[p+5]?1:0);
                    let total=tradeBins.get(key);
                    if(!total && tradeBins.size<4096) {total=[t,0,0,a[p+5]];tradeBins.set(key,total);}
                    if(total) {total[0]=Math.max(total[0],t);total[1]+=a[p+3]*a[p+4];total[2]+=a[p+4];}
                    if(trades.length===40000) {trades.sort((a,b)=>a[0]-b[0]);trades.splice(0,20000);}
                    continue;
                }
                const b=Math.floor(t/m.step)*m.step;
                if(current && b!==current.b)flushBin();
                if(!current)current={b,t,bid:0,ask:0,count:0,gap:false,values:new Map()};
                const bin=current;
                if (kind===3) {bin.gap=true;continue;}
                if (kind!==1) throw Error('Unknown archive record');
                ++depthCount;
                // Mean observed quantity; absent rows count as zero. Exclude a
                // coarse bin crossing any synchronization/capture boundary.
                if (a[p+3] && bin.count) bin.gap=true;
                bin.boundary=bin.boundary || !!a[p+3];
                bin.t=t;bin.bid=a[p+4];bin.ask=a[p+5];++bin.count;
                const center=(bin.bid+bin.ask)/2, origin=Math.floor(center/m.tick)-512;
                for (let i=p+7;i<p+n;i+=2) {
                    const k=Math.floor(a[i]/m.tick+1e-7);
                    if (k<origin || k>=origin+1024) continue;
                    bin.values.set(k,(bin.values.get(k)||0)+a[i+1]);
                }
                if (bin.values.size>1024) for (const k of bin.values.keys())
                    if (k<origin || k>=origin+1024) bin.values.delete(k);
            }
        }
        flushBin();trades.sort((a,b)=>a[0]-b[0]);
        const grouped=tradeCount>20000;
        const visibleTrades=grouped ? [...tradeBins.values()].map(t=>[t[0],t[1]/t[2],t[2],t[3]]).sort((a,b)=>a[0]-b[0]) : trades;
        for (const t of visibleTrades) {out.set([2,t[0],6,t[1],t[2],t[3]],used);used+=6;}
        const buffer=out.buffer.slice(0,used*8);
        postMessage({type:'view',id:m.id,request:m.request,step:m.step,depthCount,tradeCount,grouped,buffer},[buffer]);
    }
    let chain=Promise.resolve();
    onmessage = e => {
        const m=e.data;
        chain=chain.then(async()=>{
            if (m.type==='append') await append(m);
            else if (m.type==='query') await query(m);
            else if (m.type==='clear') {sessions.delete(m.id);await remove(m.id);}
        }).catch(error=>postMessage({type:'error',id:m.id,message:String(error.name || 'Error')+': '+error.message}))
        .then(()=>postMessage({type:'ack',id:m.id,bytes:m.buffer ? m.buffer.byteLength:0,request:m.request || 0}));
    };
}
const states=new Map(); let worker, inflight=0, pendingQueries=0, failed='';
function start() {
    if (worker || failed) return;
    try {
        worker=new Worker(URL.createObjectURL(new Blob(['('+archiveWorker.toString()+')()'],{type:'text/javascript'})));
        worker.onmessage=e=>{
            const m=e.data,s=states.get(m.id);
            if (m.type==='ack') {inflight=Math.max(0,inflight-m.bytes);if(m.request){pendingQueries=Math.max(0,pendingQueries-1);if(s && m.request===s.request)s.pending=false;}return;}
            if (!s) return;
            if(m.type==='status') Object.assign(s,m);
            if(m.type==='error') {s.error=m.message;s.pending=false;}
            if(m.type==='view' && m.request===s.request) {s.view=m;s.pending=false;}
        };
        worker.onerror=()=>{failed='Archive worker failed';inflight=0;for(const s of states.values()){s.error=failed;s.pending=false;}};
    } catch(e) {failed=String(e);}
}
Module['rtArchive']={
    create(id) {start();const limit=states.size>=4 ? 'Archive limit: four active markets' : '';states.set(id,{first:0,last:0,bytes:0,total:0,error:failed||limit,pending:false,request:0,view:null});},
    capacity(id,bytes) {
        const s=states.get(id);
        return !s || s.error || failed ? -1 : inflight+bytes>2*1024*1024 ? 0 : 1;
    },
    append(id,buffer,clock) {
        const s=states.get(id);
        if(!s || s.error || failed) return -1;
        if(inflight+buffer.byteLength>2*1024*1024) return 0;
        inflight+=buffer.byteLength;worker.postMessage({type:'append',id,buffer,clock},[buffer]);return 1;
    },
    query(id,from,to,cutoff,step,tick) {
        const s=states.get(id);if(!s||s.error||failed||s.pending||pendingQueries>=4)return 0;
        s.pending=true;s.view=null;++s.request;++pendingQueries;
        worker.postMessage({type:'query',id,from,to,cutoff,step,tick,request:s.request});return s.request;
    },
    cancel(id) {const s=states.get(id);if(s){++s.request;s.view=null;s.pending=false;}},
    clear(id) {states.delete(id);if(worker&&!failed)worker.postMessage({type:'clear',id});},
    state(id) {return states.get(id);},
    diagnostics() {return {inflight,failed,sessions:[...states].map(([id,s])=>({id,first:s.first,last:s.last,bytes:s.bytes,total:s.total,pending:s.pending,error:s.error}))};}
};
})();
