// Deterministic transport fixture for the production C++ capture coordinator.
// Real worker/IndexedDB tests live in tests/browser/.
var crypto={randomUUID:(()=>{let id=0;return ()=> 'capture-fixture-'+(++id);})()};
Module['archiveBlocked']=false;
Module['rtArchive']={
    sessions:new Map(),
    create(id){this.sessions.set(id,{first:0,last:0,bytes:0,total:0,pending:false,view:null,records:[]});},
    state(id){return this.sessions.get(id);},
    clear(id){this.sessions.delete(id);},
    cancel(id){const s=this.state(id);if(s){s.pending=false;s.view=null;}},
    capacity(){return Module['archiveBlocked']?0:1;},
    append(id,buffer){
        const s=this.state(id),a=new Float64Array(buffer);
        for(let p=0;p<a.length;p+=a[p+2])s.records.push(Array.from(a.subarray(p,p+a[p+2])));
        s.bytes+=a.byteLength;s.total=s.bytes;return 1;
    },
    query(){return 0;}
};
