import { createSourcePositions, placeSourcePosition } from "./sourcePositions.js";
import { createReadingAnnotations } from "./readingAnnotations.js";
import { createPreviewScroll } from "./previewScroll.js";

// Trusted reader controls; all publication markup remains passive.
export function findText(section, query, limit = 200) {
    if (typeof query !== "string" || !query.trim() || query.length > 128) throw Error("검색어는 1~128자입니다.");
    const doc = section.document, nodes = [], walker = doc.createTreeWalker(doc.body || doc.documentElement, 4);
    let node, text = "";
    while ((node = walker.nextNode())) {
        if (node.parentElement?.closest("script,style,head,svg,math")) continue;
        if (nodes.length >= 32768 || text.length + node.textContent.length > 2 * 1024 * 1024) throw Error("장 검색 크기 제한을 초과했습니다.");
        nodes.push({ node, start: text.length }); text += node.textContent;
    }
    const escaped = [...query].map(c => ".*+?^${}()|[]".includes(c) || c.charCodeAt(0)===92 ? String.fromCharCode(92)+c : c).join("");
    // Case-insensitive literal regex retains offsets in the original Unicode string.
    const pattern = new RegExp(escaped, "giu"), hits = [];
    for (const match of text.matchAll(pattern)) {
        const start = match.index, end = start + match[0].length;
        const first = nodes.findLast(n => n.start <= start), last = nodes.findLast(n => n.start < end);
        if (!first || !last) continue;
        const range = doc.createRange(); range.setStart(first.node, start - first.start); range.setEnd(last.node, end - last.start);
        hits.push({ cfi: section.cfiFromRange(range), excerpt: text.slice(Math.max(0,start-45),Math.min(text.length,end+65)).replace(/\s+/g," ") });
        if (hits.length >= limit) break;
    }
    return { hits, characters: text.length };
}
export function validCfi(value) { return typeof value === "string" && value.length <= 8192 && /^epubcfi\([^\u0000-\u001f\u007f-\u009f\u2028\u2029]+\)$/u.test(value); }
// scroll: {container, chapter, layout} for source scroll synchronisation (previewScroll.js).
export function createReaderTools({ publication, documents, snapshot, restore, book, apply, navigate, remember=()=>{}, scroll }) {
    const annotations=createReadingAnnotations({book,documents});
    const sourcePositions=createSourcePositions(snapshot,scroll.container);
    const previewScroll=createPreviewScroll({documents,...scroll});
    let result = { serial: 0 }, popup = null, dialog = null, focus = null;
    // Editing previews report right clicks; the host process shows the menu.
    let contextMenus = false, contextRequest = { serial: 0 }, contextSerial = 0;
    function requestContext(event, doc) {
        if(!contextMenus)return;event.preventDefault();event.stopImmediatePropagation();
        const frame=doc.defaultView?.frameElement, offset=frame?frame.getBoundingClientRect():{left:0,top:0};
        const keyboard=event.type==="keydown", target=keyboard?doc.getSelection()?.anchorNode?.parentElement||doc.body:event.target;
        const rect=keyboard?target?.getBoundingClientRect?.()||{left:0,top:0,height:0}:null;
        const x=Math.round(offset.left+(keyboard?rect.left:event.clientX)), y=Math.round(offset.top+(keyboard?rect.top+rect.height:event.clientY));
        const selection=doc.getSelection()?.toString().slice(0,4096)||"";const image=target?.closest?.("img");
        if(!keyboard && target && !doc.getSelection()?.containsNode(target,true)) {const range=doc.caretRangeFromPoint?.(event.clientX,event.clientY);if(range && !selection){const s=doc.getSelection();s.removeAllRanges();s.addRange(range);}}
        sourcePositions.capture(doc);const anchor=sourcePositions.state();
        let path="";if(image){try{const url=new URL(image.getAttribute("src"),doc.baseURI);if(url.protocol==="epub:" && url.pathname.startsWith("/book/"))path=decodeURIComponent(url.pathname.slice(6));}catch{}}
        contextRequest={serial:++contextSerial,x,y,selection,anchor,target:image?{kind:"image",path}:{kind:"text"}};
    }
    let searchResult = { serial: 0 }, searchController;
    const cancelSearch = () => { searchController?.abort(); searchController = null; };
    async function search(request) {
        cancelSearch();
        const controller = new AbortController(); searchController = controller;
        const { serial, value } = request;
        result = searchResult = { serial, busy: true };
        let completed;
        try {
            if(typeof value!=="string" || !value.trim() || value.length>128) throw Error("검색어는 1~128자입니다.");
            const b=book(); if(!b) throw Error("본문이 준비되지 않았습니다.");
            const hits=[];let characters=0,limited=false;
            for(const section of b.spine.spineItems) {
                if(controller.signal.aborted)return;
                if(section.index>=512 || characters>=16*1024*1024 || hits.length>=200) {limited=true;break;}
                // Search owns its document. It must never unload a section that
                // an asynchronous page turn is currently displaying.
                const response=await fetch(section.url,{signal:controller.signal});
                if(!response.ok)throw Error("검색 리소스를 읽지 못했습니다.");
                const document=new DOMParser().parseFromString(await response.text(),"application/xhtml+xml");
                if(controller.signal.aborted)return;
                const found=findText({document,cfiFromRange:range=>section.cfiFromRange(range)},value,200-hits.length);
                characters+=found.characters;hits.push(...found.hits.map(hit=>({...hit,chapter:section.index})));
                await new Promise(resolve=>setTimeout(resolve,0));
            }
            completed={serial,hits,limited:limited||hits.length>=200};
        } catch(error) { completed={serial,error:String(error)}; }
        if(controller.signal.aborted || searchController!==controller)return;
        searchResult=completed;
        if(result.serial===serial)result=completed;
    }
    const hooked = new WeakSet();
    const close = () => { dialog?.remove(); dialog=null; popup=null; if(focus?.isConnected)focus.focus();else window.hcFocusReading?.(); };
    function show(kind, value) {
        close(); focus=document.activeElement; dialog=document.createElement("dialog");
        dialog.style.cssText="max-width:90vw;max-height:85vh;padding:16px;color:inherit;background:inherit;border:1px solid currentColor;overflow:auto";
        dialog.setAttribute("aria-label",kind==="image"?"이미지 확대":"각주");
        const exit=document.createElement("button");exit.textContent="닫기 (Esc)";exit.onclick=close;dialog.append(exit);
        dialog.addEventListener("cancel",e=>{e.preventDefault();close();});
        popup={kind,...value};
        if(kind==="footnote") {const text=document.createElement("p");text.textContent=value.text;dialog.append(text);}
        else {
            const toolbar=document.createElement("div"), panel=document.createElement("div"), image=document.createElement("img");
            panel.style.cssText="overflow:auto;max-height:65vh;max-width:80vw";image.src=value.src;image.alt=value.alt||"책 이미지";
            let zoom=100;const label=document.createElement("span");
            const resize=()=>{image.style.width=`${image.naturalWidth*zoom/100}px`;image.style.maxWidth="none";label.textContent=` ${zoom}% `;popup.zoom=zoom;};
            for(const [name,change] of [["축소",-25],["확대",25],["원본",0]]) {const button=document.createElement("button");button.textContent=name;button.onclick=()=>{zoom=change?Math.max(25,Math.min(400,zoom+change)):100;resize();};toolbar.append(button);}
            toolbar.append(label);dialog.append(toolbar,panel);panel.append(image);image.onload=resize;popup.zoom=100;
        }
        document.body.append(dialog);dialog.showModal();exit.focus();
    }
    async function resource(href, doc) {
        const url=new URL(href,doc.baseURI);
        if(url.origin!=="null" && url.origin!==new URL("epub://app/").origin) throw Error("외부 링크는 열 수 없습니다.");
        if(url.protocol!=="epub:" || url.host!=="app" || !url.pathname.startsWith("/book/")) throw Error("책 내부 리소스가 아닙니다.");
        const path=decodeURIComponent(url.pathname.slice(6));
        const item=publication().resources.find(r=>r.path===path);if(!item)throw Error("책에 없는 리소스입니다.");
        return {url,item};
    }
    async function footnote(href, doc) {
        const {url,item}=await resource(href,doc);if(item.mediaType!=="application/xhtml+xml" || !url.hash)throw Error("각주 대상이 올바르지 않습니다.");
        const html=await fetch(url.href.split("#")[0]).then(r=>r.text());
        const parsed=new DOMParser().parseFromString(html,"application/xhtml+xml");
        const target=parsed.getElementById(decodeURIComponent(url.hash.slice(1)));if(!target)throw Error("각주를 찾을 수 없습니다.");
        show("footnote",{text:target.textContent.trim().slice(0,8192)});
    }
    function hook(doc) {
        if(hooked.has(doc))return;hooked.add(doc);annotations.hook(doc);sourcePositions.hook(doc);previewScroll.hook(doc);
        for(const image of doc.images){image.tabIndex=0;image.setAttribute("role","button");image.setAttribute("aria-label",image.alt||"책 이미지 확대");}
        doc.addEventListener("keydown",event=>{if(!event.isComposing && event.target.matches?.("img") && ["Enter"," "].includes(event.key)){event.preventDefault();event.target.click();}
            else if(contextMenus && (event.key==="ContextMenu" || (event.key==="F10" && event.shiftKey)))requestContext(event,doc);});
        doc.addEventListener("contextmenu",event=>requestContext(event,doc),true);
        doc.addEventListener("click",e=>{
            const anchor=e.target.closest?.("a"), image=e.target.closest?.("img");
            if(anchor)remember();
            if(anchor && ((anchor.getAttribute("epub:type")||"").split(/\s+/).includes("noteref") || anchor.getAttribute("role")==="doc-noteref")) {
                e.preventDefault();e.stopImmediatePropagation();footnote(anchor.getAttribute("href"),doc).catch(error=>{result={...result,error:String(error)};});
            } else if(image) {e.preventDefault();e.stopImmediatePropagation();resource(image.getAttribute("src"),doc).then(({url,item})=>{
                if(!item.mediaType.startsWith("image/"))throw Error("이미지 리소스가 아닙니다.");show("image",{src:url.href,alt:image.alt});
            }).catch(error=>{result={...result,error:String(error)};});}
        },true);
    }
    async function action(request) {
        if(request.action==="search")return search(request);
        const {action,value,serial}=request;result={serial,busy:true};
        try {
            if(["annotations-set","annotation-capture","annotation-resolve"].includes(action)){await annotations.action(request);result={serial,annotation:true};}
            else if(action==="source-position") {const doc=documents()[0];if(!doc)throw Error("본문이 준비되지 않았습니다.");placeSourcePosition(doc,value);sourcePositions.capture(doc);result={serial,located:true};}
            else if(["toc","next","previous","back","chapter","next-chapter","previous-chapter"].includes(action)) {
                // Toolbar and key page moves are the user's scroll; DOM input listeners never see Qt buttons.
                if(["next","previous","back"].includes(action))previewScroll.markIntent();
                annotations.clearSelection();if(popup)close();await navigate(action,value);result={serial,located:true};}
            else if(action==="locate" || action==="preview-location") {if(!validCfi(value))throw Error("잘못된 읽기 위치입니다.");remember();await restore(value);documents().forEach(apply);if(action==="locate")window.hcFocusReading?.();result={serial,located:true};}
            else if(action==="image") {const doc=documents()[0],image=doc?.images[Number(value)||0];if(!image)throw Error("이미지가 없습니다.");const {url}=await resource(image.getAttribute("src"),doc);show("image",{src:url.href,alt:image.alt});result={serial};}
            else if(action==="footnote") {const doc=documents()[0];await footnote(value,doc);result={serial};}
            else if(action==="close-popup") {close();result={serial};}
            else if(action==="context-menu") {contextMenus=value===true;result={serial};}
            else if(action==="preview-width") {const width=Math.max(0,Math.min(4096,Number(value)||0));const root=document.documentElement;root.style.setProperty("--hc-preview-width",width?`${width}px`:"none");document.body.classList.toggle("hc-preview-width",width>0);window.dispatchEvent(new Event("resize"));result={serial};}
            else throw Error("지원하지 않는 읽기 명령입니다.");
        } catch(error) {result={serial,error:String(error)};}
    }
    window.hcDismissPopup = () => {if(!dialog)return false;close();return true;};
    return {hook,action,cancelSearch,cancelAnnotations:annotations.cancel,previewScroll,state:()=>({...annotations.state(),sourceSelection:sourcePositions.state(),previewScroll:previewScroll.state(),readerAction:result,readerSearch:searchResult,popup,contextRequest})};
}
