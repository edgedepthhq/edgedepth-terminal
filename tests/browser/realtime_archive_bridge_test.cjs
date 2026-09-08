// No browser emulation: this tests only the production main-thread transport
// state machine. IndexedDB/compression are tested in realtime_archive.html.
const fs=require('node:fs'), vm=require('node:vm'), assert=require('node:assert/strict');
const path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../../src/realtime_archive.js'),'utf8');
let worker;
class FakeWorker {
    constructor(){worker=this;this.messages=[];}
    postMessage(m){this.messages.push(m);}
    emit(data){this.onmessage({data});}
}
const context={Module:{},Worker:FakeWorker,Blob,URL,Map,Float64Array};
vm.runInNewContext(source,context);const bridge=context.Module.rtArchive;
bridge.create('one');
assert.equal(bridge.append('one',new ArrayBuffer(1024*1024)),1);
assert.equal(bridge.append('one',new ArrayBuffer(1024*1024)),1);
assert.equal(bridge.append('one',new ArrayBuffer(8)),0);
assert.equal(worker.messages.length,2,'full queue must not enqueue');
assert.equal(bridge.capacity('one',8),0,'reject before copying WASM bytes');
worker.emit({type:'ack',id:'one',bytes:1024*1024});
assert.equal(bridge.append('one',new ArrayBuffer(8)),1);
assert.equal(bridge.query('one',0,1000,1000,100,1),1);
assert.equal(bridge.query('one',0,2000,2000,100,1),0,'one pending request per market');
bridge.cancel('one');
worker.emit({type:'view',id:'one',request:1,buffer:new ArrayBuffer(8)});
assert.equal(bridge.state('one').view,null,'pause cancels an in-flight display replacement');
bridge.clear('one');bridge.create('two');
worker.emit({type:'view',id:'one',request:1,buffer:new ArrayBuffer(8)});
assert.equal(bridge.state('two').view,null,'reset rejects old-session responses');
bridge.query('two',1000,2000,1500,100,1);
assert.equal(worker.messages.at(-1).cutoff,1500);
worker.emit({type:'error',id:'two',message:'QuotaExceededError'});
assert.equal(bridge.append('two',new ArrayBuffer(8)),-1,'storage failure stops archiving explicitly');
bridge.create('three');worker.onerror();
assert.equal(bridge.append('three',new ArrayBuffer(8)),-1);
assert.equal(bridge.state('three').pending,false);
assert.match(bridge.state('three').error,/worker failed/);
console.log('PASS: bounded transport, backpressure, reset races, replay cutoffs, storage and worker failure');
