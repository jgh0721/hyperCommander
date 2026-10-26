import { createSourcePositions, placeSourcePosition } from "./sourcePositions.js";
import { createReadingAnnotations } from "./readingAnnotations.js";

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
export function createReaderTools({ publication, documents, snapshot, restore, book, apply, navigate, remember=()=>{} }) {
    const annotations=createReadingAnnotations({book,documents});
    const sourcePositions=createSourcePositions(snapshot);
    let result = { serial: 0 }, popup = null, dialog = null, focus = null;
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
        if(hooked.has(doc))return;hooked.add(doc);annotations.hook(doc);sourcePositions.hook(doc);
        for(const image of doc.images){image.tabIndex=0;image.setAttribute("role","button");image.setAttribute("aria-label",image.alt||"책 이미지 확대");}
        doc.addEventListener("keydown",event=>{if(!event.isComposing && event.target.matches?.("img") && ["Enter"," "].includes(event.key)){event.preventDefault();event.target.click();}});
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
        const {action,value,serial}=request;result={serial,busy:true};
        try {
            if(["annotations-set","annotation-capture","annotation-resolve"].includes(action)){await annotations.action(request);result={serial,annotation:true};}
            else if(action==="source-position") {const doc=documents()[0];if(!doc)throw Error("본문이 준비되지 않았습니다.");placeSourcePosition(doc,value);sourcePositions.capture(doc);result={serial,located:true};}
            else if(action==="search") {
                const b=book();if(!b)throw Error("EPUB.js 검색을 사용할 수 없습니다.");
                const hits=[];let characters=0,limited=false;
                for(const section of b.spine.spineItems) {
                    if(section.index>=512 || characters>=16*1024*1024 || hits.length>=200) {limited=true;break;}
                    const wasLoaded=!!section.document;
                    try {await section.load(b.load.bind(b));const found=findText(section,value,200-hits.length);characters+=found.characters;hits.push(...found.hits.map(h=>({...h,chapter:section.index})));}
                    finally {if(!wasLoaded)section.unload();}
                    await new Promise(resolve=>setTimeout(resolve,0));
                }
                result={serial,hits,limited:limited||hits.length>=200};
            } else if(["toc","next","previous","back","chapter","next-chapter","previous-chapter"].includes(action)) {annotations.clearSelection();if(popup)close();await navigate(action,value);result={serial,located:true};}
            else if(action==="locate" || action==="preview-location") {if(!validCfi(value))throw Error("잘못된 읽기 위치입니다.");remember();await restore(value);documents().forEach(apply);if(action==="locate")window.hcFocusReading?.();result={serial,located:true};}
            else if(action==="image") {const doc=documents()[0],image=doc?.images[Number(value)||0];if(!image)throw Error("이미지가 없습니다.");const {url}=await resource(image.getAttribute("src"),doc);show("image",{src:url.href,alt:image.alt});result={serial};}
            else if(action==="footnote") {const doc=documents()[0];await footnote(value,doc);result={serial};}
            else if(action==="close-popup") {close();result={serial};}
            else throw Error("지원하지 않는 읽기 명령입니다.");
        } catch(error) {result={serial,error:String(error)};}
    }
    window.hcDismissPopup = () => {if(!dialog)return false;close();return true;};
    return {hook,action,cancelAnnotations:annotations.cancel,state:()=>({...annotations.state(),sourceSelection:sourcePositions.state(),readerAction:result,popup})};
}
