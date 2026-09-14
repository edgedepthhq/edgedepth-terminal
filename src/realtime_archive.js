// Browser-local observed RT sessions. Embedded in index.js so hosted and OSS
// asset delivery share one owner. Validated startup seeds arrive through the
// C++ coordinator; this worker never fetches market data.
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
            seq:s.seq, seed:s.seed || false, bytes:packed.byteLength + 256, created:Date.now()};
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
        if(m.seed) {await flush(s);s.seed=m.seed;}
        if(Number.isFinite(m.clock))s.clock=m.clock;
        // Records: kind,time,...; depth length=7+2*n, trade length=6, grouped trade length=9, gap length=3.
        for (let p=0;p<a.length;) {
            const n = a[p+2];
            if (!Number.isInteger(n) || n<3 || p+n>a.length || n>s.raw.length) throw Error('Invalid archive record');
            if (s.used+n>s.raw.length || (s.used && a[p+1]-s.min>=5000)) await flush(s);
            s.raw.set(a.subarray(p,p+n),s.used); s.used+=n;
            s.min=Math.min(s.min,a[p+1]); s.max=Math.max(s.max,a[p+1]); p+=n;
        }
        s.clock=Number.isFinite(m.clock) ? m.clock : s.max;
        // Each acknowledged batch is durable, including quiet tails and pause.
        await flush(s);s.seed=false; await status(s);
    }
    async function query(m) {
        const s = sessions.get(m.id); // Earlier append acknowledgements are durable.
        if (!s) {
            postMessage({type:'view',id:m.id,request:m.request,step:m.step,source_bucket_ticks:1,depthCount:0,tradeCount:0,grouped:false,buffer:new ArrayBuffer(0)});
            return;
        }
        const rows = (await metadata(m.id)).filter(r=>r.max>=m.from-m.step && r.min<=Math.min(m.to,m.cutoff))
            .sort((a,b)=>Number(!!b.seed)-Number(!!a.seed) || a.seq-b.seq);
        const recovery=rows.filter(r=>r.seed===2).sort((a,b)=>a.min-b.min || a.seq-b.seq);
        const original=rows.filter(r=>r.seed!==2);
        const db = await database(), trades = [], tradeBins = new Map();
        // A single aggregation bin and a bounded typed output. Never materialize
        // the full archive as per-observation JS objects/maps.
        const out=new Float64Array(2048*2055+180000); let used=0, columns=0;
        let current=null, previous=-Infinity, tradeCount=0, depthCount=0, sourceGrouped=false;
        function flushBin() {
            const bin=current;current=null;
            if (!bin || bin.gap || !bin.count) {previous=-Infinity;return;}
            if (++columns>2048) return;
            // The boundary clips rendering within this bin. Keep its first observed
            // timestamp, not the last sample used by the aggregate.
            const boundary=bin.boundary || bin.b-previous>m.step;
            out.set([1,boundary?bin.first:bin.t,7+bin.values.size*2,boundary?1:0,
                bin.bid,bin.ask,bin.values.size],used);used+=7;
            for (const [k,v] of bin.values) {out[used++]=k*m.tick;out[used++]=v/bin.count;}
            previous=bin.b;
        }
        function consumeDepth(a,p) {
            const kind=a[p],t=a[p+1],n=a[p+2];
            const b=Math.floor(t/m.step)*m.step;
            if(current && b!==current.b)flushBin();
            if(!current)current={b,t,first:t,bid:0,ask:0,count:0,gap:false,values:new Map()};
            // A new segment owns the remainder of its bin. Do not average
            // across the boundary or discard valid post-boundary samples.
            if (kind===1 && a[p+3] && (current.count || current.gap))
                current={b,t,first:t,bid:0,ask:0,count:0,gap:false,values:new Map()};
            const bin=current;
            if (kind===3) {bin.gap=true;return;}
            if (kind!==1) throw Error('Unknown archive record');
            ++depthCount;
            // Mean observed quantity within this segment; absent rows count as zero.
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
        // Recovery chunks are chronological depth-only observations. Stream one
        // decompressed chunk at a time into the original timeline, never retain
        // every raw observation or reorder trades already recorded locally.
        let recoveryIndex=0,recoveryArray=null,recoveryPosition=0;
        async function recoveryRecord() {
            for(;;) {
                if(recoveryArray && recoveryPosition<recoveryArray.length) {
                    const p=recoveryPosition;recoveryPosition+=recoveryArray[p+2];
                    const t=recoveryArray[p+1];
                    if(t<m.from || t>m.to || t>m.cutoff)continue;
                    return recoveryArray.subarray(p,recoveryPosition);
                }
                if(recoveryIndex===recovery.length)return null;
                const row=recovery[recoveryIndex++];
                const packed=await request(db.transaction('data').objectStore('data').get(row.key));
                recoveryArray=packed?new Float64Array(await codec(packed,true)):null;recoveryPosition=0;
            }
        }
        let nextRecovery=await recoveryRecord();
        for (const row of original) {
            if (sessions.get(m.id)!==s) return;
            const packed = await request(db.transaction('data').objectStore('data').get(row.key));
            if (!packed) {flushBin();previous=-Infinity;continue;}
            const a = new Float64Array(await codec(packed,true));
            for (let p=0;p<a.length;p+=a[p+2]) {
                const kind=a[p], t=a[p+1], n=a[p+2];
                if (t<m.from || t>m.to || t>m.cutoff) continue;
                if (kind===4 && t+100>Math.min(m.to,m.cutoff)) continue;
                if (kind===2 || kind===4) {
                    const lo=kind===4?a[p+6]:a[p+3], hi=kind===4?a[p+7]:a[p+3], count=kind===4?a[p+8]:1;
                    sourceGrouped=sourceGrouped || kind===4;
                    tradeCount+=count;trades.push([t,a[p+3],a[p+4],a[p+5],lo,hi,count]);
                    const key=Math.floor(t/m.step)*2+(a[p+5]?1:0);
                    let total=tradeBins.get(key);
                    if(!total && tradeBins.size<4096) {total=[t,0,0,a[p+5],lo,hi,0];tradeBins.set(key,total);}
                    if(total) {total[0]=Math.max(total[0],t);total[1]+=a[p+3]*a[p+4];total[2]+=a[p+4];total[4]=Math.min(total[4],lo);total[5]=Math.max(total[5],hi);total[6]+=count;}
                    if(trades.length===40000) {trades.sort((a,b)=>a[0]-b[0]);trades.splice(0,20000);}
                    continue;
                }
                while(nextRecovery && nextRecovery[1]<=t) {consumeDepth(nextRecovery,0);nextRecovery=await recoveryRecord();}
                consumeDepth(a,p);
            }
        }
        while(nextRecovery) {consumeDepth(nextRecovery,0);nextRecovery=await recoveryRecord();}
        flushBin();trades.sort((a,b)=>a[0]-b[0]);
        const grouped=sourceGrouped || tradeCount>20000;
        const visibleTrades=grouped ? [...tradeBins.values()].map(t=>[t[0],t[1]/t[2],t[2],t[3],t[4],t[5],t[6]]).sort((a,b)=>a[0]-b[0]) : trades;
        for (const t of visibleTrades) {
            if(grouped) {out.set([4,t[0],9,t[1],t[2],t[3],t[4],t[5],t[6]],used);used+=9;}
            else {out.set([2,t[0],6,t[1],t[2],t[3]],used);used+=6;}
        }
        const buffer=out.buffer.slice(0,used*8);
        if (sessions.get(m.id)!==s) return;
        postMessage({type:'view',id:m.id,request:m.request,step:m.step,
            source_bucket_ticks:m.native_tick>0 ? Math.max(1,Math.round(m.tick/m.native_tick)):1,
            depthCount,tradeCount,grouped,buffer},[buffer]);
    }
    // Capture stays ordered and durable, but decompression of a historical
    // view must not hold up new observations. Queries have a separate serial
    // lane and wait for the writes already queued when they were requested.
    let chain=Promise.resolve(), queries=Promise.resolve();
    onmessage = e => {
        const m=e.data;
        const finish = promise => promise
            .catch(error=>postMessage({type:'error',id:m.id,message:String(error.name || 'Error')+': '+error.message}))
            .then(()=>postMessage({type:'ack',id:m.id,bytes:m.buffer ? m.buffer.byteLength:0,request:m.request || 0}));
        if (m.type==='query') {
            const captured=chain;
            queries=finish(queries.then(()=>captured).then(()=>query(m)));
        } else {
            chain=finish(chain.then(async()=>{
                if (m.type==='append') await append(m);
                else if (m.type==='clear') {sessions.delete(m.id);await remove(m.id);}
            }));
        }
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
        return !s || s.error || failed ? -1 : inflight+bytes>4*1024*1024 ? 0 : 1;
    },
    append(id,buffer,clock,seed=false) {
        const s=states.get(id);
        if(!s || s.error || failed) return -1;
        if(inflight+buffer.byteLength>4*1024*1024) return 0;
        inflight+=buffer.byteLength;worker.postMessage({type:'append',id,buffer,clock,seed},[buffer]);return 1;
    },
    query(id,from,to,cutoff,step,tick,native_tick=0) {
        const s=states.get(id);if(!s||s.error||failed||s.pending||pendingQueries>=4)return 0;
        s.pending=true;s.view=null;++s.request;++pendingQueries;
        worker.postMessage({type:'query',id,from,to,cutoff,step,tick,native_tick,request:s.request});return s.request;
    },
    cancel(id) {const s=states.get(id);if(s){++s.request;s.view=null;s.pending=false;}},
    clear(id) {states.delete(id);if(worker&&!failed)worker.postMessage({type:'clear',id});},
    state(id) {return states.get(id);},
    diagnostics() {return {inflight,failed,sessions:[...states].map(([id,s])=>({id,first:s.first,last:s.last,bytes:s.bytes,total:s.total,pending:s.pending,error:s.error}))};}
};
})();
