import EpubCFI from "epubjs/src/epubcfi.js";

const cfi = value => typeof value === "string" && value.length <= 2048 && /^epubcfi\([^\u0000-\u001f\u007f-\u009f\u2028\u2029]+\)$/u.test(value);
const normalize = text => text.replace(/\s+/gu, " ").trim();
function textIndex(doc) {
    if (!doc?.body) throw Error("본문을 확인할 수 없습니다.");
    const walker=doc.createTreeWalker(doc.body,4),nodes=[];let node,text="";
    while((node=walker.nextNode())) {
        if(node.parentElement?.closest("script,style,head,svg,math"))continue;
        if(nodes.length>=32768 || text.length+node.textContent.length>2*1024*1024)throw Error("메모 문맥 검색은 장마다 2 MiB까지 지원합니다.");
        nodes.push({node,start:text.length});text+=node.textContent;
    }
    return {doc,nodes,text};
}
function rangeAt(index,start,end) {
    const first=index.nodes.findLast(item=>item.start<=start),last=index.nodes.findLast(item=>item.start<end);
    if(!first || !last || end<=start)throw Error("본문 범위를 확인할 수 없습니다.");
    const range=index.doc.createRange();range.setStart(first.node,start-first.start);range.setEnd(last.node,end-last.start);return range;
}
function selectionData(range,section) {
    if(!range || range.collapsed)throw Error("본문에서 메모할 문장을 먼저 선택하세요.");
    const index=textIndex(range.startContainer.ownerDocument);let start=-1,end=-1;
    for(const item of index.nodes)if(range.intersectsNode(item.node)) {
        const from=item.start+(range.startContainer===item.node?range.startOffset:0),to=item.start+(range.endContainer===item.node?range.endOffset:item.node.length);
        if(to>from){if(start<0)start=from;end=to;}
    }
    const quote=index.text.slice(start,end),locator=section.cfiFromRange(range);
    if(start<0 || !quote.trim() || quote.length>512 || !cfi(locator))throw Error("인용은 512자, 읽기 위치는 2048자 이내로 선택하세요.");
    return {locator,quote,prefix:index.text.slice(Math.max(0,start-128),start),suffix:index.text.slice(end,end+128),chapter:section.index};
}
function normalizedIndex(text) {
    const positions=new Int32Array(text.length),normalized=text.replace(/\s+/gu," "),whitespace=/\s/u;let at=0,space=false;
    for(let i=0;i<text.length;++i){const white=whitespace.test(text[i]);if(white && space)continue;positions[at++]=i;space=white;}
    return {text:normalized,positions};
}
export function createReadingAnnotations({book,documents}) {
    let entries=[],marked=new Set(),errors=[],cached=null,epoch=0,result={serial:0},clicked={serial:0,id:""};
    const hooked=new WeakSet(),rendition=()=>book()?.rendition;
    function hook(doc) {
        if(hooked.has(doc))return;hooked.add(doc);
        const capture=()=>{
            const selection=doc.getSelection();if(!selection?.rangeCount || selection.isCollapsed){cached=null;return;}
            const view=rendition()?.manager?.views?.all().find(item=>item.document===doc);
            if(!view)return;
            try {cached=selectionData(selection.getRangeAt(0),view.section);}catch {cached=null;}
        };
        doc.addEventListener("pointerup",capture);doc.addEventListener("keyup",capture);
    }
    async function capture(target) {
        if(target!==undefined && target!==null) {
            if(!cfi(target))throw Error("선택 위치가 올바르지 않습니다.");
            const b=book(),parsed=new EpubCFI(target),section=b?.spine.get(parsed.spinePos);if(!section)throw Error("선택한 장을 찾을 수 없습니다.");
            const loaded=!!section.document;
            try {await section.load(b.load.bind(b));return selectionData(parsed.toRange(section.document),section);}finally {if(!loaded)section.unload();}
        }
        for(const doc of documents()) {
            const selection=doc.getSelection(),view=rendition()?.manager?.views?.all().find(item=>item.document===doc);
            if(view && selection?.rangeCount && !selection.isCollapsed)return selectionData(selection.getRangeAt(0),view.section);
        }
        if(cached && rendition()?.manager?.views?.all().some(view=>view.section.index===cached.chapter))return {...cached};
        throw Error("본문에서 메모할 문장을 먼저 선택하세요.");
    }
    function setEntries(value) {
        if(!Array.isArray(value) || value.length>32)throw Error("형광펜·메모 개수 제한을 초과했습니다.");
        const r=rendition();if(!r)throw Error("EPUB.js 리더에서 형광펜을 사용할 수 있습니다.");
        for(const locator of marked)r.annotations.remove(locator,"highlight");marked=new Set();errors=[];entries=value;
        const report=(row)=>{if(errors.length<32 && !errors.some(error=>error.id===row.id))errors.push({id:row.id,error:"메모의 읽기 위치와 인용 문장이 일치하지 않습니다. 다시 연결하세요."});};
        for(const row of entries) {
            if(row.unresolved)continue;
            if(typeof row.id!=="string" || !/^[a-f0-9]{32}$/.test(row.id) || !cfi(row.locator) || !/^#[a-f0-9]{6}$/i.test(row.color)){errors.push({id:row.id,error:"지원하지 않는 메모 위치 또는 색상입니다."});continue;}
            try {
                const parsed=new EpubCFI(row.locator);if(!parsed.range || !book().spine.get(parsed.spinePos))throw Error("range");
                const valid=view=>{try {const range=parsed.toRange(view.document);return !!range && !range.collapsed && normalize(range.toString())===normalize(row.quote);}catch{return false;}};
                const views=r.manager.views.all().filter(view=>view.index===parsed.spinePos);
                if(views.some(view=>!valid(view))){report(row);continue;}
                if(marked.has(row.locator))r.annotations.remove(row.locator,"highlight");
                const annotation=r.annotations.highlight(row.locator,{id:row.id},()=>{clicked={serial:clicked.serial+1,id:row.id};},"hc-reading-highlight",{fill:row.color,"fill-opacity":"0.35"});
                const attach=annotation.attach.bind(annotation);annotation.attach=view=>{if(!valid(view)){report(row);return;}try{return attach(view);}catch{report(row);}};
                marked.add(row.locator);
            }catch {try{r.annotations.remove(row.locator,"highlight");}catch{}report(row);}
        }
    }
    async function resolve(value,token) {
        if(!value || typeof value.quote!=="string" || !value.quote.trim() || value.quote.length>512 || ![value.prefix,value.suffix].every(v=>typeof v==="string" && v.length<=128))throw Error("메모의 문장 또는 문맥이 올바르지 않습니다.");
        const b=book();if(!b)throw Error("EPUB.js 리더에서 판본을 연결할 수 있습니다.");
        const query=normalize(value.quote),prefix=normalize(value.prefix),suffix=normalize(value.suffix);let total=0,seen=0,strong=[],single="",limited=false;
        for(const section of b.spine.spineItems) {
            if(token!==epoch)throw Error("판본 연결을 취소했습니다.");
            if(section.index>=512 || total>=16*1024*1024){limited=true;break;}
            const loaded=!!section.document;
            try {
                await section.load(b.load.bind(b));const doc=section.document;if(token!==epoch)throw Error("판본 연결을 취소했습니다.");
                const index=textIndex(doc);total+=index.text.length;if(total>16*1024*1024){limited=true;break;}
                const normalized=normalizedIndex(index.text);let at=-1;
                while((at=normalized.text.indexOf(query,at+1))>=0) {
                    if(++seen>256){limited=true;break;}
                    const range=rangeAt(index,normalized.positions[at],normalized.positions[at+query.length-1]+1),locator=section.cfiFromRange(range);
                    if(seen===1)single=locator;
                    const before=normalized.text.slice(Math.max(0,at-prefix.length-2),at).trimEnd(),after=normalized.text.slice(at+query.length,at+query.length+suffix.length+2).trimStart();
                    if((!prefix || before.endsWith(prefix)) && (!suffix || after.startsWith(suffix)))strong.push(locator);
                    if(strong.length>1)break;
                }
                if(limited || strong.length>1)break;
            }finally {if(!loaded)section.unload();}
            await new Promise(done=>setTimeout(done,0));
        }
        if(limited)return {locator:"",error:"문맥 검색 한도에 도달해 자동으로 연결하지 않았습니다."};
        const locator=strong.length===1?strong[0]:strong.length===0 && seen===1?single:"";
        return {locator:cfi(locator)?locator:"",matches:seen};
    }
    async function action(request) {
        const {action,value,serial}=request;const token=epoch;result={serial,busy:true};
        try {
            if(action==="annotations-set"){setEntries(value);result={serial,count:entries.length};}
            else if(action==="annotation-capture")result={serial,selection:await capture(value)};
            else if(action==="annotation-resolve")result={serial,...await resolve(value,token)};
            else throw Error("지원하지 않는 메모 명령입니다.");
        }catch(error){result={serial,error:error.message || String(error)};}
    }
    function visibleMarks() {
        const bounds=document.getElementById("reader").getBoundingClientRect();let count=0;
        for(const view of rendition()?.manager?.views?.all() || [])for(const value of Object.values(view.highlights || {})) {
            const element=value.mark?.element || value.element;if(!element?.getBoundingClientRect)continue;const rect=element.getBoundingClientRect();
            if(rect.width>0 && rect.height>0 && Math.min(rect.right,bounds.right)>Math.max(rect.left,bounds.left) && Math.min(rect.bottom,bounds.bottom)>Math.max(rect.top,bounds.top))++count;
        }return count;
    }
    return {hook,action,cancel:()=>{++epoch;},clearSelection:()=>{cached=null;},state:()=>({annotationAction:result,annotationClicked:clicked,
        annotationState:{count:entries.length,marked:marked.size,visible:visibleMarks(),rendered:(rendition()?.manager?.views?.all() || []).reduce((sum,view)=>sum+Object.keys(view.highlights || {}).length,0),errors}})};
}
