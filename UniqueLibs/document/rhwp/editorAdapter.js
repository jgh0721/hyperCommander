// Native-owned single-document Studio integration. RHWP keeps its own editing
// shortcuts. No renderer path, URL, file chooser or browser download is used.
export async function ConnectEditor(Channel, Hooks) {
    const Trace=globalThis.__hcRhwpStartupTrace,ConnectStart=Trace?.begin();
    Trace?.end('js.moduleToAdapter',0);
    const Load = Channel.objects.loadBridge, Edit = Channel.objects.editBridge;
    const Call = (Object, Method, ...Args) => new Promise(Resolve => Object[Method](...Args, Resolve));
    const State = await Call(Edit, 'State');
    if (State.version !== 1 || !State.binding || State.sequence !== '0' || State.error) throw Error('editor-grant');
    const Binding = State.binding;
    const PrintModule=await import('./hostPrint.js');
    const PrintEngine=Hooks.printEngine;
    const Printer=await PrintModule.ConnectPrint(Channel.objects.printBridge,PrintEngine,{
        snapshot:async Identity=>{
            Hooks.setEnabled(false);
            try {
                await Changes;
                if(Identity.changeSequence!==Sequence.toString())throw Error('print-stale-model');
                const Artifact=await Hooks.export(Meta.format);
                const Loss=Artifact.contentLoss;
                if(!Loss||Loss.schemaVersion!==1||Loss.count!==0||!Array.isArray(Loss.losses)||Loss.losses.length)throw Error('print-content-loss');
                return Artifact.bytes.slice();
            }finally{if(!Stopped)Hooks.setEnabled(true);}
        }
    });
    let Sequence = 0n, Stopped = false, ActiveCommand = '', Changes = Promise.resolve();
    const Report = (Type, Fields = {}) => Call(Edit, 'Report', JSON.stringify({version:1,binding:Binding,type:Type,...Fields}));
    const Hash = async Bytes => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', Bytes)),
        Byte => Byte.toString(16).padStart(2,'0')).join('');
    const Meta = await Call(Load, 'PendingOpen');
    if (Meta.version !== 1 || !['hwp','hwpx'].includes(Meta.format) || Meta.readOnly !== true
        || !Number.isInteger(Meta.byteLength) || Meta.byteLength < 1 || Meta.byteLength > 64*1024*1024
        || Meta.chunkSize !== 192*1024 || !/^[0-9a-f]{64}$/.test(Meta.sha256)
        || Meta.documentId !== Binding.documentId || Meta.documentGeneration !== Binding.generation
        || Meta.bindingOwner !== Binding.bindingOwner || Meta.bindingGeneration !== Binding.bindingGeneration
        || Meta.epoch !== Binding.epoch) throw Error('editor-load-binding');
    const Identity = Object.fromEntries(['version','format','documentId','documentGeneration',
        'bindingOwner','bindingGeneration','epoch','operation'].map(Key => [Key,Meta[Key]]));
    const LoadReport = (Type, Fields={}) => Call(Load,'Report',JSON.stringify({...Identity,type:Type,...Fields}));
    const Cancel = () => { Stopped = true; Printer.stop(); Hooks.setEnabled(false); };
    Load.CancelRequested.connect(Cancel);
    try {
        Hooks.setEnabled(false);
        const TransferStart=Trace?.begin();
        const Bytes = new Uint8Array(Meta.byteLength);
        for (let Offset=0; Offset<Bytes.length; Offset+=Meta.chunkSize) {
            const Chunk=atob(await Call(Load,'ReadChunk',Meta.operation,Offset));
            if (Stopped || Chunk.length !== Math.min(Meta.chunkSize,Bytes.length-Offset)) throw Error('editor-chunk');
            for(let I=0;I<Chunk.length;++I) Bytes[Offset+I]=Chunk.charCodeAt(I);
        }
        Trace?.end('js.inputTransfer',TransferStart);
        const HashStart=Trace?.begin();
        if(await Hash(Bytes)!==Meta.sha256 || Stopped) throw Error('editor-hash');
        Trace?.end('js.inputHash',HashStart);
        const LoadStart=Trace?.begin();
        const Loaded = await Hooks.load(Bytes,Meta.format);
        Trace?.end('js.editorLoad',LoadStart);
        if(!Number.isSafeInteger(Loaded.pageCount) || Loaded.pageCount<1 || Loaded.pageCount>100000
            || !await LoadReport('Parsed',{pageCount:Loaded.pageCount})) throw Error('editor-parse');
        const FrameStart=Trace?.begin();
        const Frame = await Hooks.firstFrame();
        Trace?.end('js.frameWait',FrameStart);
        const AckStart=Trace?.begin();
        if(Stopped || !await LoadReport('FirstFrame',{page:0,width:Frame.width,height:Frame.height})) throw Error('editor-frame');
        Trace?.end('js.firstFrameAck',AckStart);Trace?.end('js.connectEditor',ConnectStart);
        if(Trace)void LoadReport('StartupTrace',{spans:Trace.finish()}).catch(()=>{});
        Hooks.onChanged(() => {
            if(Stopped) return;
            const Next=(++Sequence).toString();
            Changes=Changes.then(async()=>{ if(!await Report('Changed',{sequence:Next})) throw Error('editor-model'); });
            Changes.catch(Cancel);
        });
        Hooks.onAction?.(Action => {
            return Changes.then(() => { if (!Stopped) return Action==='print'?Printer.request():Report('UserAction',{action:Action}); }).catch(Cancel);
        });
        Hooks.setEnabled(true);
        const Poll = async () => {
            if(Stopped) return;
            let Phase='command';
            try {
                const Command=await Call(Edit,'PendingCommand');
                if(!Command.id) return;
                if(Command.id===ActiveCommand) return;
                ActiveCommand=Command.id;
                if(Command.version!==1) throw Error('editor-command');
                await Changes;
                if(Command.type==='Synchronize') {
                    Hooks.setEnabled(false);
                    await new Promise(Resolve=>setTimeout(Resolve,0));
                    await Changes;
                    if(!await Report('Synchronized',{id:Command.id})) throw Error('editor-sync');
                    return;
                }
                if(Command.type==='Resume' || Command.type==='Discard') {
                    if(Command.type==='Resume') Hooks.setEnabled(true);
                    if(!await Report('CommandObserved',{id:Command.id})) throw Error('editor-command-ack');
                    return;
                }
                if(Command.type==='Saved') {
                    // Do not clear newer changes when an old commit notification
                    // arrives after a user keystroke.
                    if(Command.sequence===Sequence.toString()) await Hooks.saved();
                    if(!await Report('SavedObserved',{id:Command.id})) throw Error('editor-saved');
                    return;
                }
                if(Command.type!=='Export' || !['hwp','hwpx'].includes(Command.format)
                    || Command.identity?.changeSequence!==Sequence.toString()) throw Error('editor-export-state');
                Hooks.setSaving?.(true);
                const Snapshot=Sequence;
                Phase='serialize';
                const Artifact=await Hooks.export(Command.format);
                Phase='bytes';
                if(!(Artifact.bytes instanceof Uint8Array) || !Artifact.bytes.length || Artifact.bytes.length>64*1024*1024)
                    throw Error('editor-export-size');
                Phase='hash';
                const Sha=await Hash(Artifact.bytes);
                Phase='transfer';
                const Send=async Message=>{
                    for(let Retry=0;Retry<3000;++Retry){
                        if(Stopped || Sequence!==Snapshot) throw Error('editor-export-stale');
                        if(await Report('ExportMessage',{id:Command.id,message:{version:1,identity:Command.identity,...Message}})) return;
                        const Current=await Call(Edit,'State');
                        if(Current.error || Current.retiring || !Current.binding) throw Error('editor-export-rejected');
                        await new Promise(Resolve=>setTimeout(Resolve,10));
                    }
                    throw Error('editor-export-timeout');
                };
                await Send({type:'ExportBegin',format:Command.format,byteLength:Artifact.bytes.length,sha256:Sha,loss:Artifact.contentLoss});
                for(let Offset=0;Offset<Artifact.bytes.length;Offset+=192*1024){
                    let Binary=''; for(const Byte of Artifact.bytes.subarray(Offset,Offset+192*1024)) Binary+=String.fromCharCode(Byte);
                    await Send({type:'ExportChunk',offset:Offset,data:btoa(Binary)});
                }
                await Send({type:'ExportCommit'});
            } catch {
                if(ActiveCommand) await Report('Failed',{id:ActiveCommand,phase:Phase});
            } finally { Hooks.setSaving?.(false); if(!Stopped) setTimeout(Poll,50); }
        };
        void Poll(); return {stop:Cancel};
    } catch(Error) {
        Cancel(); await LoadReport('Failed'); throw Error;
    }
}
