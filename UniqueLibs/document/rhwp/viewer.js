import * as Engine from './rhwp.js';
import {OpenReadOnlyPages} from './readOnlyAdapter.js';
import {ConnectPrint} from './hostPrint.js';
const Status=document.getElementById('status'),Pages=document.getElementById('pages');
let Controller,Printer,Observer,Frame,Stopped=false,CancelSignal,Shortcut;
const Frames=[],Lifetime=new AbortController();
const StopObservers=()=>{Observer?.disconnect();if(Frame!==undefined)cancelAnimationFrame(Frame);Frame=undefined;
    window.removeEventListener('scroll',Schedule);window.removeEventListener('resize',Schedule);};
const Dispose=()=>{if(Stopped)return;Stopped=true;Lifetime.abort();StopObservers();Controller?.dispose();Printer?.stop();
    document.removeEventListener('keydown',Shortcut,true);CancelSignal?.disconnect(Dispose);window.removeEventListener('pagehide',Dispose);};
const LateFailure=(Error,Page)=>{
    Status.hidden=false;
    if(Stopped)return;const Label=Frames[Page]?.querySelector('.page-placeholder');if(Label)Label.textContent=(Page+1)+' 페이지 표시 실패';
    Status.textContent='일부 페이지를 표시하지 못했습니다. 문서를 다시 열어 주세요.';Dispose();
};
function Schedule(){
    if(Stopped||!Controller||Frame!==undefined)return;
    Frame=requestAnimationFrame(()=>{Frame=undefined;if(Stopped||Controller.disposed)return;
        const Height=window.innerHeight;
        const Visible=Frames.map((Element,Page)=>({Page,Rect:Element.getBoundingClientRect()}))
            .filter(Item=>Item.Rect.bottom>0&&Item.Rect.top<Height);
        const Priorities=Visible.map(Item=>Item.Page);
        for(const Item of Visible)for(const Page of [Item.Page-1,Item.Page+1])if(Page>=0&&Page<Frames.length)Priorities.push(Page);
        Controller.requestPages([...new Set(Priorities)]);
    });
}
try {
    new QWebChannel(qt.webChannelTransport,Channel=>{
        CancelSignal=Channel.objects.loadBridge.CancelRequested;CancelSignal.connect(Dispose);window.addEventListener('pagehide',Dispose);
        OpenReadOnlyPages(Channel.objects.loadBridge,Engine,(Page,Count,Size)=>{
            const Section=document.createElement('section');Section.className='rhwp-page';Section.style.width=Size.width+'px';
            Section.style.aspectRatio=Size.width+'/'+Size.height;Section.dataset.page=Page;Section.setAttribute('aria-busy','true');
            const Label=document.createElement('span');Label.className='page-placeholder';Label.textContent=(Page+1)+' / '+Count+' 페이지';
            const Canvas=document.createElement('canvas');Canvas.width=Canvas.height=0;Canvas.hidden=true;
            Canvas.setAttribute('role','img');Canvas.setAttribute('aria-label',(Page+1)+' / '+Count+' 페이지');
            Section.append(Label,Canvas);Pages.append(Section);Frames.push(Section);return Canvas;
        },{
            signal:Lifetime.signal,
            onPaint:Page=>{if(Stopped)return;const Section=Frames[Page];Section.querySelector('canvas').hidden=false;
                Section.dataset.painted='true';Section.setAttribute('aria-busy','false');},
            onProgress:(Count,Total)=>{if(!Stopped)Status.textContent='읽기 전용 · '+Total+' 페이지 · '+Count+' 페이지 표시';},
            onError:LateFailure
        }).then(async Result=>{
            Controller=Result;if(Stopped){Controller.dispose();return;}
            Printer=await ConnectPrint(Channel.objects.printBridge,Engine,{
                before:()=>Controller.pause(),snapshot:async()=>Controller.snapshotBytes(),after:()=>Controller.resume()
            });
            if(Stopped){Printer.stop();return;}
            Status.hidden=true;
            Shortcut=Event=>{if(!Stopped&&Event.ctrlKey&&!Event.altKey&&!Event.shiftKey&&!Event.isComposing&&Event.key.toLowerCase()==='p'){
                Event.preventDefault();Event.stopImmediatePropagation();void Printer.request();}};
            document.addEventListener('keydown',Shortcut,true);
            Pages.setAttribute('aria-busy','false');
            Observer=new IntersectionObserver(Schedule,{rootMargin:'100% 0px'});Frames.forEach(Element=>Observer.observe(Element));
            window.addEventListener('scroll',Schedule,{passive:true});window.addEventListener('resize',Schedule,{passive:true});Schedule();
        }).catch(()=>{
            if(Stopped)return;Status.hidden=false;Dispose();Pages.replaceChildren();Pages.setAttribute('aria-busy','false');
            Status.textContent='문서를 표시하지 못했습니다. 지원 범위 또는 문서 상태를 확인해 주세요.';
        });
    });
}catch{Dispose();Status.textContent='문서를 표시하지 못했습니다.';}
