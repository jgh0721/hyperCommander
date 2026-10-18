import {RenderPrintSnapshot} from './printSnapshot.js';
// A dedicated copied byte snapshot is reparsed; never print the viewport or disk.
export async function ConnectPrint(Bridge, Engine, Hooks) {
    const Call=(Method,...Args)=>new Promise(Resolve=>Bridge[Method](...Args,Resolve));
    const State=await Call('State'),Binding=State.binding;
    if(State.version!==1||!Binding)throw Error('print-grant');
    const Report=(Type,Fields={})=>Call('Report',JSON.stringify({version:1,binding:Binding,type:Type,...Fields}));
    let Active='',Stopped=false;
    const Request=()=>Stopped?Promise.resolve(false):Report('UserPrint');
    const Poll=async()=>{
        if(Stopped)return;
        try {
            const Command=await Call('PendingCommand');
            if(Stopped)return;
            if(Command.cancel){await Report('PrintRetired',{id:Command.id});return;}
            if(!Command.id||Command.id===Active)return;
            Active=Command.id;
            const Current=async()=>{const C=await Call('PendingCommand');return !Stopped&&C.id===Command.id&&!C.cancel;};
            const Send=async Message=>{
                for(let Retry=0;Retry<3000;++Retry){
                    if(!await Current())throw Error('print-cancelled');
                    if(await Report('PrintMessage',{id:Command.id,message:{version:1,identity:Command.identity,...Message}}))return;
                    await new Promise(R=>setTimeout(R,10));
                }
                throw Error('print-transfer-timeout');
            };
            try {
                await Hooks.before?.();
                if(Stopped)throw Error('print-stopped');
                const Bytes=await Hooks.snapshot(Command.identity);
                const Snapshot=await RenderPrintSnapshot(Engine,Bytes,Command.pages.length?Command.pages:undefined,
                    ()=>document.createElement('canvas'),async()=>!await Current());
                await Send({type:'PrintBegin',totalPages:Snapshot.totalPages});
                for(const Page of Snapshot.pages){
                    const Bytes=Uint8Array.from(atob(Page.png),C=>C.charCodeAt(0));
                    const Sha=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',Bytes)),B=>B.toString(16).padStart(2,'0')).join('');
                    await Send({type:'PrintPageBegin',page:Page.page,width:Page.width,height:Page.height,byteLength:Bytes.length,sha256:Sha});
                    for(let Offset=0;Offset<Bytes.length;Offset+=192*1024){
                        let Binary='';for(const B of Bytes.subarray(Offset,Offset+192*1024))Binary+=String.fromCharCode(B);
                        await Send({type:'PrintChunk',offset:Offset,data:btoa(Binary)});
                    }
                    await Send({type:'PrintPageEnd'});
                }
                await Send({type:'PrintCommit'});
            }catch{if(await Current())await Send({type:'PrintFailed'});else await Report('PrintRetired',{id:Command.id});}
            finally {await Hooks.after?.();}
        }finally{if(!Stopped)setTimeout(Poll,50);}
    };
    void Poll();return {request:Request,stop:()=>{Stopped=true;}};
}
