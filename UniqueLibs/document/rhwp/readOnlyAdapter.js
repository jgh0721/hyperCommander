// Engine-only read-only adapter. No Studio RPC, document mutation or export surface.
// The enclosing native owner must enforce a deadline and retire the renderer:
// synchronous WASM cannot be interrupted by a JavaScript timer.
export async function OpenReadOnly(Bridge, Engine, Canvas) {
    return OpenDocument(Bridge, Engine, () => Canvas, false);
}

// Admit all page geometry, then paint page 0. The returned controller owns the
// live model until disposed; later pages use a separate native render watchdog.
export async function OpenReadOnlyPages(Bridge, Engine, CreateCanvas, Options = {}) {
    if (typeof CreateCanvas !== 'function') throw new Error('canvas-factory');
    return OpenDocument(Bridge, Engine, CreateCanvas, true, Options);
}

async function OpenDocument(Bridge, Engine, CreateCanvas, AllPages, Options = {}) {
    const Trace=globalThis.__hcRhwpStartupTrace;
    if(Trace)Trace.end('js.moduleToAdapter',0);
    let Identity, Doc, Bytes, Cancelled = false, Retained = false, Disposed = false, Admitted = false, Dispose;
    const Wakes=new Set();
    const Release=()=>{if(Disposed)return;Disposed=true;Bridge.CancelRequested.disconnect(Cancel);Options.signal?.removeEventListener('abort',Cancel);try{Doc?.free();}finally{Doc=undefined;Bytes=undefined;}};
    const Call = (Method, ...Args) => new Promise(Resolve => Bridge[Method](...Args, Resolve));
    const Report = (Type, Fields = {}) => Call('Report', JSON.stringify({...Identity, type:Type, ...Fields}));
    const Check = () => { if (Cancelled) throw new Error('cancelled'); };
    const Cancel = () => {
        Cancelled = true;
        for(const Wake of [...Wakes])Wake();
        if (Identity) void Report('CancelObserved');
        Dispose?.();
    };
    Bridge.CancelRequested.connect(Cancel);
    Options.signal?.addEventListener('abort',Cancel,{once:true});
    if(Options.signal?.aborted)Cancel();
    try {
        const TransferStart=Trace?.begin();
        const Meta = await Call('PendingOpen');
        Check();
        if (Meta.version !== 1 || Meta.readOnly !== true || !['hwp','hwpx'].includes(Meta.format)
            || !Number.isInteger(Meta.byteLength) || Meta.byteLength < 1 || Meta.byteLength > 64 * 1024 * 1024
            || Meta.chunkSize !== 192 * 1024 || !/^[0-9a-f]{64}$/.test(Meta.sha256)) throw new Error('metadata');
        if(AllPages && Meta.renderMode!=='viewport-v1')throw new Error('render-mode');
        Identity = Object.fromEntries(['version','format','documentId','documentGeneration',
            'bindingOwner','bindingGeneration','epoch','operation'].map(Key => [Key, Meta[Key]]));
        Bytes = new Uint8Array(Meta.byteLength);
        for (let Offset = 0; Offset < Bytes.length; Offset += Meta.chunkSize) {
            const Text = atob(await Call('ReadChunk', Meta.operation, Offset));
            Check();
            if (Text.length !== Math.min(Meta.chunkSize, Bytes.length - Offset)) throw new Error('chunk');
            for (let Index = 0; Index < Text.length; ++Index) Bytes[Offset + Index] = Text.charCodeAt(Index);
        }
        Trace?.end('js.inputTransfer',TransferStart);
        const HashStart=Trace?.begin();
        const Digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', Bytes)),
            Byte => Byte.toString(16).padStart(2, '0')).join('');
        Check();
        if (Digest !== Meta.sha256) throw new Error('hash');
        Trace?.end('js.inputHash',HashStart);
        const InitStart=Trace?.begin();
        await Engine.default();
        Trace?.end('js.wasmInit',InitStart);
        Check();
        const ParseStart=Trace?.begin();
        Doc = new Engine.HwpDocument(Bytes);
        if (Doc.getSourceFormat() !== Meta.format) throw new Error('format');
        const Pages = Doc.pageCount();
        if (!Number.isInteger(Pages) || Pages < 1 || Pages > 100000) throw new Error('pages');
        Trace?.end('js.parseLayout',ParseStart);
        const ParsedStart=Trace?.begin();
        if (!await Report('Parsed', {pageCount:Pages})) throw new Error('parsed-rejected');
        Check();
        Trace?.end('js.parsedAck',ParsedStart);
        const PreflightStart=Trace?.begin();
        const Count = AllPages ? Pages : 1;
        if (Count > 128) throw new Error('page-count-budget');
        // Preflight the entire allocation before rendering any page. 64M pixels
        // bounds canvas backing stores (~256 MiB), not total renderer memory.
        let Pixels = 0;
        const Sizes = [];
        for (let Page = 0; Page < Count; ++Page) {
            Check();
            const Info = JSON.parse(Doc.getPageInfo(Page));
            if (!Number.isFinite(Info.width) || !Number.isFinite(Info.height)
                || Info.width <= 0 || Info.height <= 0) throw new Error('page-budget');
            // RHWP reports dimensions rounded to 0.1 CSS px; its canvas uses
            // ceil(original dimension). Reserve the full rounding interval.
            const Width = Math.ceil(Info.width + 0.05);
            const Height = Math.ceil(Info.height + 0.05);
            if (Width > 4096 || Height > 4096 || Width * Height > 16 * 1024 * 1024)
                throw new Error('page-budget');
            Pixels += Width * Height;
            if (Pixels > 64 * 1024 * 1024) throw new Error('document-pixel-budget');
            Sizes.push({width:Width, height:Height});
        }
        Trace?.end('js.preflight',PreflightStart);
        const Frame=()=>new Promise((Resolve,Reject)=>{
            let Handle;
            const Stop=()=>{if(Handle!==undefined)globalThis.cancelAnimationFrame?.(Handle);Wakes.delete(Stop);Reject(new Error('cancelled'));};
            Wakes.add(Stop);
            Handle=requestAnimationFrame(()=>{if(Cancelled){Stop();return;}
                Handle=requestAnimationFrame(()=>{Wakes.delete(Stop);if(Cancelled)Reject(new Error('cancelled'));else Resolve();});});
        });
        const ReserveStart=Trace?.begin();
        const Canvases=Sizes.map((Size,Page)=>{
            Check();const Canvas=CreateCanvas(Page,Pages,Size);
            if(!Canvas||!Canvas.isConnected)throw new Error('canvas');return Canvas;
        });
        Trace?.end('js.reservePages',ReserveStart);
        const Paint=Page=>{
            Check();const Canvas=Canvases[Page];if(!Canvas.isConnected)throw new Error('canvas');
            Doc.renderPageToCanvas(Page,Canvas,1);
            if(!Number.isInteger(Canvas.width)||!Number.isInteger(Canvas.height)||Canvas.width<1||Canvas.height<1
                ||Canvas.width>Sizes[Page].width||Canvas.height>Sizes[Page].height)throw new Error('canvas');
            Check();Options.onPaint?.(Page);return {width:Canvas.width,height:Canvas.height};
        };
        let First;
        for (let Page = 0; Page < 1; ++Page) {
            Check();
            const Canvas = Canvases[Page];
            if (!Canvas || !Canvas.isConnected) throw new Error('canvas');
            const RenderStart=Trace?.begin();
            Doc.renderPageToCanvas(Page, Canvas, 1);
            if (!Number.isInteger(Canvas.width) || !Number.isInteger(Canvas.height)
                || Canvas.width < 1 || Canvas.height < 1
                || Canvas.width > Sizes[Page].width
                || Canvas.height > Sizes[Page].height) throw new Error('canvas');
            if (Page === 0) First = {width:Canvas.width, height:Canvas.height};
            Options.onPaint?.(Page);
            Trace?.end('js.page0Render',RenderStart);
            const FrameStart=Trace?.begin();
            await Frame();
            Trace?.end('js.frameWait',FrameStart);
            Check();
        }
        const AckStart=Trace?.begin();
        if (!await Report('FirstFrame', {page:0,...First})) throw new Error('frame-rejected');
        Trace?.end('js.firstFrameAck',AckStart);
        if(Trace)void Report('StartupTrace',{spans:Trace.finish()}).catch(()=>{});
        Admitted=true;Check();
        if(!AllPages)return {pageCount:Pages,renderedPages:1,printBytes:Bytes,...First};
        Retained=true;
        const Painted=new Set([0]);let Queue=[],Busy=false,Timer,Paused=0,Sequence=0n,Safe,ResolveSafe;
        const Notify=()=>{if(!Cancelled)Options.onProgress?.(Painted.size,Pages);};
        const SafeEnd=()=>{ResolveSafe?.();ResolveSafe=undefined;Safe=undefined;};
        const Fail=async(Error,Page)=>{
            if(!Cancelled){Cancelled=true;Queue=[];for(const Wake of [...Wakes])Wake();
                try{await Report('ViewFailed');}finally{Options.onError?.(Error,Page);}}
        };
        const Pump=async()=>{
            Timer=undefined;if(Cancelled||Busy||Paused||!Queue.length)return;
            const Page=Queue.shift();if(Painted.has(Page)){Schedule();return;}
            Busy=true;Safe=new Promise(Resolve=>{ResolveSafe=Resolve;});const Current=++Sequence;
            try {
                if(!await Report('ViewRenderBegin',{page:Page,sequence:Current.toString()}))throw new Error('render-begin-rejected');
                Check(); // A pending ACK can arrive after disposal; never re-enter WASM.
                const Size=Paint(Page);
                if(!await Report('ViewRenderEnd',{page:Page,sequence:Current.toString(),...Size}))throw new Error('render-end-rejected');
                Check();SafeEnd(); // Print may start now: no view WASM remains in flight.
                await Frame();Check();Painted.add(Page);Notify();
            }catch(Error){await Fail(Error,Page);}
            finally{SafeEnd();Busy=false;if(Cancelled)Release();else Schedule();}
        };
        const Schedule=()=>{if(!Cancelled&&!Busy&&!Paused&&Queue.length&&Timer===undefined)Timer=setTimeout(Pump,0);};
        Dispose=()=>{if(Cancelled&&Disposed)return;Cancelled=true;Queue=[];if(Timer!==undefined)clearTimeout(Timer);Timer=undefined;
            for(const Wake of [...Wakes])Wake();if(!Busy)Release();};
        Notify();
        return {pageCount:Pages,...First,get renderedPages(){return Painted.size;},get disposed(){return Cancelled;},
            snapshotBytes(){Check();return Bytes.slice();}, // One retained source buffer; copy only for a print operation.
            requestPages(Indices){Check();if(!Array.isArray(Indices)||Indices.length>128||Indices.some(P=>!Number.isInteger(P)||P<0||P>=Pages))throw new Error('page-selection');
                Queue=[...new Set(Indices)].filter(P=>!Painted.has(P));Schedule();},
            async pause(){Check();++Paused;await Safe;Check();},
            resume(){if(Cancelled)return;Paused=Math.max(0,Paused-1);Schedule();},dispose:Dispose};
    } catch (Error) {
        if (Identity && !Cancelled) await Report(AllPages&&Admitted?'ViewFailed':'Failed');
        Retained=false;
        throw Error;
    } finally {
        if(!Retained)Release();
    }
}
