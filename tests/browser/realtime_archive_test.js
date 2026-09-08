/* Run via realtime_archive.html, served from the repository root, or pass
   ?source=/path/to/realtime_archive.js for an existing WASM build server. */
'use strict';
document.querySelector('#run').onclick=async()=>{
    const result=document.querySelector('#result');result.textContent='Running\n';
    const log=s=>result.textContent+=s+'\n';
    const check=(ok,msg)=>{if(!ok)throw Error(msg);log('PASS '+msg);};
    let worker;
    try {
        const source=await (await fetch(new URLSearchParams(location.search).get('source') || '../../src/realtime_archive.js')).text();
        const fn=source.slice(source.indexOf('function archiveWorker()'),source.indexOf('const states=new Map();'));
        const name='edgedepth-rt-test-'+Date.now();
        function make(extra='',budget=256*1024*1024) {
            const code=fn.replace('edgedepth-rt-observed-v1',name).replace('256 * 1024 * 1024',String(budget));
            return new Worker(URL.createObjectURL(new Blob([extra+'\n('+code+')()'],{type:'text/javascript'})));
        }
        worker=make();
        const send=m=>new Promise((resolve,reject)=>{
            const messages=[]; const timeout=setTimeout(()=>reject(Error('Worker timeout')),60000);
            worker.onmessage=e=>{messages.push(e.data);if(e.data.type==='ack'){clearTimeout(timeout);resolve(messages);}};
            worker.onerror=e=>{clearTimeout(timeout);reject(Error(e.message));};worker.postMessage(m);
        });
        const append=(id,records)=>send({type:'append',id,buffer:new Float64Array(records).buffer});
        const query=(id,from,to,step=100,cutoff=to)=>send({type:'query',id,from,to,step,cutoff,tick:1,request:1});
        const depth=(t,q=2,boundary=0)=>[1,t,11,boundary,100,101,2,100,q,101,3];
        const trade=t=>[2,t,6,100,1.234567890123,1];
        let messages=await append('roundtrip',[...depth(1001,2,1),...trade(1002),...trade(1002),...depth(1101,4)]);
        check(!messages.some(m=>m.type==='error'),'compressed chunk committed in real IndexedDB');
        messages=await query('roundtrip',1000,1200);
        let view=messages.find(m=>m.type==='view'),a=new Float64Array(view.buffer);
        check(view.tradeCount===2 && a.length===34,'same-timestamp trade multiplicity survives round-trip');
        check(a[9]===101 && a[10]===3 && a[26]===1.234567890123,'depth and trade quantities retain Float64 precision');
        messages=await query('roundtrip',1000,1200,100,1050);
        view=messages.find(m=>m.type==='view');
        check(view.depthCount===1,'replay cutoff excludes future observations');
        await append('roundtrip',[3,1101,3,...depth(1401,6,1)]);
        messages=await query('roundtrip',1000,1500);a=new Float64Array(messages.find(m=>m.type==='view').buffer);
        check(a[12]===1401 && a[14]===1,'interruption removes final unknown bin; next seed starts a new segment');
        messages=await query('roundtrip',1000,1500,500);a=new Float64Array(messages.find(m=>m.type==='view').buffer);
        check(a[0]===2,'coarse bin crossing a gap stays absent');
        await send({type:'clear',id:'roundtrip'});
        messages=await query('roundtrip',1000,1500);
        check(messages.find(m=>m.type==='view').buffer.byteLength===0,'clear prevents stale session reads');
        // Thirty minutes of 100ms, 1024-level books and 300 records/second.
        // Deliberately labelled synthetic; not a hosted-throughput claim.
        const t0=10000000, begin=performance.now(), timings=[];let rawBytes=0,status;
        for(let chunk=0;chunk<360;chunk++) {
            const data=new Float64Array(50*(2055+30*6));let p=0;
            for(let j=0;j<50;j++) {
                const sample=chunk*50+j,t=t0+sample*100;
                data.set([1,t,2055,sample===0?1:0,10000,10001,1024],p);p+=7;
                for(let k=0;k<1024;k++){data[p++]=9489+k;data[p++]=1+((sample*17+k*31)%997)/10;}
                for(let k=0;k<30;k++){data.set([2,t+k,6,10000+(k%3),1+(k%7),k%2],p);p+=6;}
            }
            rawBytes+=data.byteLength;const before=performance.now();
            messages=await send({type:'append',id:'busy',buffer:data.buffer});timings.push(performance.now()-before);
            if(messages.some(m=>m.type==='error'))throw Error(JSON.stringify(messages));
            status=messages.find(m=>m.type==='status');
            if(chunk%60===0)log('Busy fixture: '+(chunk/12).toFixed(0)+' minutes');
        }
        check(status.last-status.first>=1799900,'30 minutes of observed market time retained within actual budget');
        const qstart=performance.now();messages=await query('busy',t0,t0+1800000,1000);
        view=messages.find(m=>m.type==='view');a=new Float64Array(view.buffer);
        let columns=0,trades=0,qty=0;for(let p=0;p<a.length;p+=a[p+2]){if(a[p]===1)columns++;else {trades++;qty+=a[p+4];}}
        check(columns===1800 && view.depthCount===18000,'30-minute overview has exactly 1800 aggregated columns');
        check(view.tradeCount===540000 && view.grouped && trades<=3602,'all 540000 trade records contribute to bounded dense view');
        check(qty===2070000,'dense trade aggregation preserves total base quantity');
        const overviewMs=performance.now()-qstart;
        messages=await query('busy',t0,t0+999,100);view=messages.find(m=>m.type==='view');
        check(view.depthCount===10 && view.tradeCount===300 && !view.grouped,'earliest detail restores original trade records');
        timings.sort((a,b)=>a-b);
        log(JSON.stringify({rawBytes,archivedBytes:status.bytes,retainedMs:status.last-status.first,
            compressionRatio:rawBytes/status.bytes,appendP50Ms:timings[180],appendP95Ms:timings[342],
            appendMaxMs:timings[359],overviewMs,totalMs:performance.now()-begin},null,2));
        await send({type:'clear',id:'busy'});worker.terminate();
        // Force a quota error at the real storage write boundary.
        worker=make("const originalPut=IDBObjectStore.prototype.put;IDBObjectStore.prototype.put=function(...args){if(this.name==='data')throw new DOMException('Fixture quota','QuotaExceededError');return originalPut.apply(this,args);};");
        messages=await append('quota',depth(2000));
        check(messages.some(m=>m.type==='error' && m.message.includes('QuotaExceededError')),'quota error is explicit and acknowledged');
        worker.terminate();worker=make('',1000);
        for(let i=0;i<10;i++)messages=await append('budget',depth(1000+i*100));
        status=messages.find(m=>m.type==='status');
        check(status.total<=1000 && status.first>1000,'global byte budget evicts oldest complete chunks');
        await send({type:'clear',id:'budget'});worker.terminate();
        log('ALL BROWSER ARCHIVE TESTS PASSED');
    } catch(e) {log('FAIL '+e.stack);if(worker)worker.terminate();}
};
