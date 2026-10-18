// Installed only at native DocumentCreation when the diagnostic flag is enabled.
// No content, paths, events per chunk, persistence, or readiness side effects.
(() => {
    const Epoch=performance.now(),Rows=[],Aggregates=new Map();let Closed=false,Dropped=0;
    const Valid=new Set(['js.inputTransfer','js.inputHash','js.wasmInit','js.parseLayout','js.preflight',
        'js.editableSetup','js.documentInfo','js.reservePages','js.page0Render','js.frameWait','js.firstFrameAck','js.parsedAck',
        'js.editorLoad','js.studioInitialize','js.connectEditor','js.moduleToAdapter']);
    const Now=()=>performance.now()-Epoch;
    const End=(Name,Start,Aggregate=false)=>{
        if(Closed||!Valid.has(Name)||!Number.isFinite(Start)||Start<0)return;
        const Duration=Now()-Start;if(Duration<0||Duration>120000)return;
        if(Aggregate&&Aggregates.has(Name)){const Row=Aggregates.get(Name);Row.durationMs+=Duration;++Row.count;return;}
        if(Rows.length===32){++Dropped;return;}
        const Row={name:Name,kind:Aggregate?'aggregate':'span',startMs:Start,durationMs:Duration,count:1};Rows.push(Row);
        if(Aggregate)Aggregates.set(Name,Row);
    };
    Object.defineProperty(globalThis,'__hcRhwpStartupTrace',{value:Object.freeze({
        begin:()=>Closed?NaN:Now(),end:End,
        finish:()=>{Closed=true;return Rows.map(Row=>({...Row}));},
        get dropped(){return Dropped;}
    }),configurable:false,writable:false});
})();
