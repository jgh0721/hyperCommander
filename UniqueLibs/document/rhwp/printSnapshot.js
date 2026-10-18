// Render a dedicated immutable byte snapshot, including off-screen pages.
// The caller binds these frames to its native Printing ticket before PDF creation.
export async function RenderPrintSnapshot(Engine, Bytes, Selection, CreateCanvas, Cancelled=()=>false) {
    if (!(Bytes instanceof Uint8Array) || !Bytes.length || Bytes.length>64*1024*1024)
        throw new Error('print-input');
    await Engine.default();
    let Doc;
    const Check=async()=>{if(await Cancelled())throw new Error('print-cancelled');};
    try {
        await Check();Doc=new Engine.HwpDocument(Bytes);const Count=Doc.pageCount();
        if(!Number.isInteger(Count)||Count<1||Count>128)throw new Error('print-page-count');
        const Selected=Selection===undefined?Array.from({length:Count},(_,I)=>I):Selection;
        if(!Array.isArray(Selected)||!Selected.length||Selected.length>128
            ||Selected.some((Page,I)=>!Number.isInteger(Page)||Page<0||Page>=Count||(I&&Page<=Selected[I-1])))
            throw new Error('print-page-range');
        let Pixels=0,Encoded=0;const Sizes=Selected.map(Page=>{
            const Info=JSON.parse(Doc.getPageInfo(Page));
            const Width=Math.ceil((Info.width+0.05)*2),Height=Math.ceil((Info.height+0.05)*2);
            if(!Number.isFinite(Info.width)||!Number.isFinite(Info.height)||Info.width<=0||Info.height<=0
                ||Width>4096||Height>4096||(Pixels+=Width*Height)>64*1024*1024)throw new Error('print-pixel-budget');
            return {width:Info.width,height:Info.height,pixelWidth:Width,pixelHeight:Height};
        });
        const Pages=[];
        for(let Index=0;Index<Selected.length;++Index){
            await Check();const Canvas=CreateCanvas();
            try {
                Doc.renderPageToCanvas(Selected[Index],Canvas,2);const Size=Sizes[Index];
                if(!Number.isInteger(Canvas.width)||!Number.isInteger(Canvas.height)||Canvas.width<1||Canvas.height<1
                    ||Canvas.width>Size.pixelWidth||Canvas.height>Size.pixelHeight)throw new Error('print-canvas');
                const Url=Canvas.toDataURL('image/png');
                if(!Url.startsWith('data:image/png;base64,'))throw new Error('print-png');
                const Png=Url.slice(22);if(!Png.length||(Encoded+=Png.length)>Math.ceil(64*1024*1024/3)*4)
                    throw new Error('print-byte-budget');
                Pages.push({page:Selected[Index],width:Size.width,height:Size.height,png:Png});
            } finally { Canvas.width=Canvas.height=0;Canvas.remove?.(); }
            await new Promise(Resolve=>setTimeout(Resolve,0));await Check();
        }
        return {totalPages:Count,pages:Pages};
    } finally {Doc?.free();}
}
