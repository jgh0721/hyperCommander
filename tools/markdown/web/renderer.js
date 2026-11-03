import MarkdownIt from 'markdown-it';
import footnote from 'markdown-it-footnote';
import tasks from 'markdown-it-task-lists';
import katex from 'katex';
import mermaid from 'mermaid';
import hljs from 'highlight.js/lib/common';

// Only the fixed application shell owns the bridge. Document HTML is disabled.
let revision = 0, options = {}, bridge, suppression = 0, serial = 0, renderedTextRows = 0, displayedRevision = -1, sourceAnchor = null, appliedAnchor = null, currentPosition = 0, inputUntil = 0;
const root = document.getElementById('content');
root.classList.add('markdown-content');
for(const type of ['wheel','keydown','pointerdown','touchmove'])addEventListener(type,()=>{inputUntil=Date.now()+500},{capture:true,passive:true});
const report = value => bridge?.report(JSON.stringify({ ...value, revision }));
const escaped = s => s.replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function math(source, display) {
    try { return katex.renderToString(source, { displayMode: display, throwOnError: true, trust: false, maxExpand: 1000, output: 'htmlAndMathml' }); }
    catch (e) { return `<span class="problem" title="${escaped(e.message)}">${escaped(source)}</span>`; }
}
function parser() {
    const md = new MarkdownIt({ html: false, linkify: true, typographer: false, highlight(code, lang) {
        return options.codeHighlight !== false && hljs.getLanguage(lang) ? hljs.highlight(code, { language: lang, ignoreIllegals: true }).value : escaped(code);
    }}).use(footnote).use(tasks);
    md.block.ruler.before('hr','frontmatter',(state,start,end,silent)=>{
        if(start!==0||state.src.slice(state.bMarks[0],state.eMarks[0]).trim()!=='---')return false;
        for(let n=1;n<end;n++){if(/^(---|\.\.\.)\s*$/.test(state.src.slice(state.bMarks[n],state.eMarks[n]))){if(!silent)state.line=n+1;return true;}}
        return false;
    });
    md.inline.ruler.before('escape', 'math', (state, silent) => {
        if(options.math === false) return false;
        const at = state.pos, text = state.src;
        const opener = text.startsWith('\\(', at) ? '\\(' : options.inlineDollar !== false && text[at] === '$' && text[at+1] !== '$' ? '$' : '';
        if (!opener || /\s/.test(text[at+opener.length] || ' ')) return false;
        const closer = opener === '$' ? '$' : '\\)';
        let end = text.indexOf(closer, at+opener.length);
        while (end > 0 && text[end-1] === '\\') end = text.indexOf(closer, end+closer.length);
        if (end < 0 || /\s/.test(text[end-1]) || (closer === '$' && /\d/.test(text[end+1] || ''))) return false;
        if(!silent) { const t = state.push('math_inline','',0); t.content = text.slice(at+opener.length,end); }
        state.pos = end+closer.length; return true;
    });
    md.renderer.rules.math_inline = (ts,i) => math(ts[i].content,false);
    md.block.ruler.before('fence','math_block',(state,start,end,silent)=>{
        if(options.math === false) return false;
        const line=state.src.slice(state.bMarks[start]+state.tShift[start],state.eMarks[start]).trim();
        const open=line.startsWith('$$')?'$$':line.startsWith('\\[')?'\\[':'';
        if(!open)return false; const close=open==='$$'?'$$':'\\]';
        let body=line.slice(2),next=start+1,done=body.endsWith(close);
        if(done)body=body.slice(0,-2);else for(;next<end;next++){const l=state.src.slice(state.bMarks[next],state.eMarks[next]);const n=l.indexOf(close);if(n>=0){body+='\n'+l.slice(0,n);next++;done=true;break;}body+='\n'+l;}
        if(!done)return false;if(silent)return true;const t=state.push('math_block','div',0);t.content=body;t.map=[start,next];state.line=next;return true;
    });
    md.renderer.rules.math_block=(ts,i)=>`<div data-line="${ts[i].map[0]+1}" data-end="${ts[i].map[1]+1}">${math(ts[i].content,true)}</div>`;
    const fence=md.renderer.rules.fence;
    md.renderer.rules.fence=(ts,i,o,env,self)=>{
        const t=ts[i],lang=t.info.trim().split(/\s/)[0],attr=`data-line="${t.map[0]+1}" data-end="${t.map[1]+1}"`;
        if(lang==='mermaid' && options.mermaid!==false)return `<pre class="mermaid" ${attr}>${escaped(t.content)}</pre>`;
        if(lang==='math' && options.math!==false)return `<div ${attr}>${math(t.content,true)}</div>`;
        return `<div ${attr}>${fence(ts,i,o,env,self)}</div>`;
    };
    const image=md.renderer.rules.image;
    md.renderer.rules.image=(ts,i,o,env,self)=>{const t=ts[i],src=t.attrGet('src')||'';if(/^https?:/i.test(src)&&!options.remoteImages)return `<span class="blocked">[외부 이미지: ${escaped(t.content)}]</span>`;if(/^file:|^[a-z]:|^\\|^\/\//i.test(src))return '<span class="blocked">[허용되지 않은 이미지]</span>';return image(ts,i,o,env,self)};
    return md;
}
function anchorAtTop() {const blocks=[...root.querySelectorAll('[data-line]')];return blocks.find(e=>e.getBoundingClientRect().bottom>0)||blocks.at(-1);}
// Both directions interpolate through the same measured blocks. Long wrapped
// paragraphs must not snap the preview to the paragraph's first source line.
function scrollAnchors() {
    const points=new Map();
    for(const e of root.querySelectorAll('[data-line]')) {
        const box=e.getBoundingClientRect(),line=Number(e.dataset.line);
        if(!(line>0)||box.height<=0)continue;
        const point={line,end:Math.max(line,Number(e.dataset.end)||line),y:scrollY+box.top,height:box.height};
        if(!points.has(line)||box.height<points.get(line).height)points.set(line,point);
    }
    const table=[];
    for(const p of [...points.values()].sort((a,b)=>a.y-b.y||a.line-b.line)) {
        const last=table.at(-1);if(!last||(p.line>last.line&&p.y>last.y))table.push(p);
    }
    const last=table.at(-1);if(last)table.push({line:Math.max(last.line+1,last.end),y:last.y+last.height});
    return table;
}
function mapScroll(key,value,other) {
    const points=scrollAnchors();if(!points.length)return 0;
    if(value<=points[0][key])return points[0][other];
    for(let i=1;i<points.length;++i)if(value<points[i][key]) {
        const a=points[i-1],b=points[i];return a[other]+(b[other]-a[other])*(value-a[key])/(b[key]-a[key]);
    }
    return points.at(-1)[other];
}
function positionElement(line) {
    const all=[...root.querySelectorAll('[data-line]')];
    return all.filter(e=>Number(e.dataset.line)<=line && Number(e.dataset.end)>line).at(-1)
        || all.filter(e=>Number(e.dataset.line)<=line).at(-1)||all[0];
}
function caretY(line) {
    const e=positionElement(line);if(!e)return null;
    const rect=e.getBoundingClientRect(),start=Number(e.dataset.line),end=Math.max(start+1,Number(e.dataset.end)||start+1);
    return scrollY+rect.top+rect.height*Math.max(0,Math.min(.999,(line-start)/(end-start)));
}
function emitPosition() {
    if(!root.children.length || displayedRevision!==revision)return;
    const headings=[...root.querySelectorAll('h1,h2,h3,h4,h5,h6')];const h=headings.filter(x=>x.getBoundingClientRect().top<100).at(-1)||headings[0];
    const e=positionElement(currentPosition),box=e?.getBoundingClientRect();
    const point=appliedAnchor?.caret?caretY(appliedAnchor.line):null;
    const anchor=appliedAnchor?{...appliedAnchor,actualRatio:point===null?null:(point-scrollY)/innerHeight,visible:point!==null&&point>=scrollY&&point<=scrollY+innerHeight}:null;
    report({type:'scroll',textRows:renderedTextRows,offset:scrollY,line:mapScroll('y',scrollY+innerHeight*.5,'line'),ratio:.5,
        user:Date.now()>suppression&&Date.now()<inputUntil,top:scrollY<=1,bottom:scrollY>=document.documentElement.scrollHeight-innerHeight-1,
        appliedAnchor:anchor,position:{line:currentPosition,visible:!!box&&box.bottom>0&&box.top<innerHeight},
        progress:document.documentElement.scrollHeight<=innerHeight?100:Math.round(scrollY/(document.documentElement.scrollHeight-innerHeight)*100),heading:h?.textContent||'',headingLine:Number(h?.dataset.line||0)});
}
function applyScroll(request) {
    if(!Number.isFinite(request.line))return;
    suppression=Date.now()+300;
    const y=request.caret?caretY(request.line):mapScroll('line',request.line,'y');
    const target=request.end?document.documentElement.scrollHeight:!request.caret&&request.line<=1?0:(y??0)-innerHeight*request.ratio;
    scrollTo(0,target);appliedAnchor={line:request.line,ratio:request.ratio,caret:!!request.caret,
        clamped:Math.abs(scrollY-target)>2};
}
function fonts(f={},target=root) {
    target.style.fontFamily=f.enabled&&f.mode==='user'?f.bodyFamily:'';
    target.style.fontSize=f.enabled&&f.mode==='user'?`${f.bodySizePt||11}pt`:'';
    target.style.lineHeight=f.enabled&&f.mode!=='document'?String(f.lineHeight||1.7):'';
    target.querySelectorAll('h1,h2,h3,h4,h5,h6').forEach(e=>e.style.fontFamily=f.enabled&&f.mode!=='document'&&f.headings?target.style.fontFamily:'Segoe UI, Malgun Gothic, sans-serif');
    target.querySelectorAll('pre:not(.mermaid) code, p>code, li>code').forEach(e=>{e.style.fontFamily=f.enabled&&f.mode==='user'?f.codeFamily:'';e.style.fontSize=f.enabled&&f.mode==='user'?`${f.codeSizePt||10}pt`:''});
}
async function render(request) {
    const generation=++serial;revision=request.revision;options=request.options||{};
    sourceAnchor=Number.isFinite(request.scroll?.line)?request.scroll:null;
    document.documentElement.dataset.theme=options.dark?'dark':'light';
    document.getElementById('base').href=request.baseUrl||'qrc:/markdown/';
    const md=parser(),tokens=md.parse(request.text,{}),headings=[],slugs=new Map();
    for(let i=0;i<tokens.length;i++){const t=tokens[i];if(t.map){t.attrSet('data-line',String(t.map[0]+1));t.attrSet('data-end',String(t.map[1]+1))}if(t.type==='heading_open'){const title=(tokens[i+1].children||[]).filter(x=>['text','code_inline'].includes(x.type)).map(x=>x.content).join('');const slug=title.trim().toLowerCase().replace(/[^\p{L}\p{N}_ -]/gu,'').replace(/\s+/g,'-')||'heading';const count=slugs.get(slug)||0;slugs.set(slug,count+1);const id=slug+(count?'-'+count:'');t.attrSet('id',id);headings.push({text:title,level:Number(t.tag[1]),line:t.map[0]+1,id});}}
    // Keep the displayed document intact while asynchronous diagrams and resources settle.
    const next=document.createElement('main');next.className='markdown-content';next.dataset.previewStaging='true';
    const bounds=root.getBoundingClientRect();
    Object.assign(next.style,{position:'fixed',left:`${bounds.left}px`,top:'0',width:`${bounds.width}px`,visibility:'hidden',pointerEvents:'none'});
    next.innerHTML=md.renderer.render(tokens,md.options,{});document.body.append(next);
    const deadline=promise=>Promise.race([promise,new Promise((_,reject)=>{const timer=setTimeout(()=>reject(Error('Preview resources did not settle')),10000);promise.finally(()=>clearTimeout(timer)).catch(()=>{});})]);
    try {
        next.querySelectorAll('blockquote').forEach(e=>{const p=e.firstElementChild,m=p?.textContent.match(/^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]/);if(m){e.classList.add('alert');e.dataset.kind=m[1];}});
        fonts(options.previewFonts,next);
        mermaid.initialize({startOnLoad:false,securityLevel:'strict',theme:options.dark?'dark':'default',maxTextSize:50000,maxEdges:500,secure:['securityLevel','maxTextSize','maxEdges','startOnLoad']});
        for(const e of next.querySelectorAll('.mermaid')) {
            if(generation!==serial)return;
            const source=e.textContent;const kept=[...root.querySelectorAll('.mermaid')].find(n=>n.dataset.source===source && n.dataset.dark===String(!!options.dark) && n.querySelector('svg'));
            e.dataset.source=source;e.dataset.dark=String(!!options.dark);
            if(kept){e.innerHTML=kept.innerHTML;continue;}
            try{const result=await mermaid.render(`diagram-${generation}-${e.dataset.line}`,source);if(generation!==serial)return;e.innerHTML=result.svg;}
            catch(error){if(generation!==serial)return;e.classList.add('problem');e.title=String(error);}
        }
        await deadline(Promise.all([...next.querySelectorAll('img')].map(img=>img.decode().catch(()=>{}))));
        next.getBoundingClientRect(); // Start font loads before observing FontFaceSet.ready.
        await deadline(document.fonts.ready);if(generation!==serial)return;
        // Chromium can suspend animation callbacks in an occluded native preview.
        await new Promise(resolve=>{const timer=setTimeout(resolve,100);requestAnimationFrame(()=>requestAnimationFrame(()=>{clearTimeout(timer);resolve();}));});
        if(generation!==serial)return;
        // Reuse unchanged top-level nodes, including already rendered diagrams and loaded images.
        const available=[...root.childNodes];const children=[...next.childNodes].map(node=>{const at=available.findIndex(old=>old.isEqualNode(node));return at<0?node:available.splice(at,1)[0];});
        // Capture the displayed position at commit time: the user may have
        // scrolled while images and diagrams for this revision were loading.
        const previous=scrollY<=1?{line:1,ratio:0}:{line:mapScroll('y',scrollY+innerHeight*.5,'line'),ratio:.5};
        suppression=Date.now()+300;
        root.replaceChildren(...children);fonts(options.previewFonts);
        displayedRevision=revision;applyScroll(sourceAnchor||previous);
        const details=[...root.querySelectorAll('.problem')].map(e=>({kind:e.classList.contains('mermaid')?'Mermaid':'KaTeX',line:Number(e.closest('[data-line]')?.dataset.line||1),message:e.title||e.textContent}));
        renderedTextRows=0;
        for(const block of root.querySelectorAll('p,pre code')) {
            if(!block.textContent.trim())continue;
            const range=document.createRange();range.selectNodeContents(block);
            renderedTextRows=Math.max(renderedTextRows,new Set([...range.getClientRects()].filter(r=>r.height>0).map(r=>Math.round(r.top))).size);
        }
        report({type:'rendered',headings,problems:details.length,details});emitPosition();
    } catch(error) {if(generation===serial)throw error;} finally {next.remove();}
}
function scrollToLine(line,ratio=0.2,end=false){applyScroll({line,ratio,end});}
window.hcMarkdown={render,scrollToLine};
let scheduled=false;addEventListener('scroll',()=>{if(scheduled)return;scheduled=true;requestAnimationFrame(()=>{scheduled=false;emitPosition()})});
root.addEventListener('click',e=>{const link=e.target.closest('a');if(link){e.preventDefault();const href=link.getAttribute('href');if(href?.startsWith('#'))document.getElementById(decodeURIComponent(href.slice(1)))?.scrollIntoView();else report({type:'link',url:link.href});return;}const block=e.target.closest('[data-line]');if(block)report({type:'position',line:Number(block.dataset.line)});});
root.addEventListener('load',()=>emitPosition(),true);
if(window.qt)new QWebChannel(qt.webChannelTransport,channel=>{
    bridge=channel.objects.markdown;
    bridge.request.connect(s=>{
        const r=JSON.parse(s);
        if(r.type==='render')render(r).catch(e=>report({type:'error',message:String(e)}));
        else if(r.revision!==revision)return;
        else if(r.type==='scroll'){
            sourceAnchor=r;if(displayedRevision===revision){applyScroll(r);emitPosition();}
        } else if(r.type==='position'){
            currentPosition=r.line;
            if(displayedRevision!==revision)return;
            const e=positionElement(r.line);
            if(e){root.querySelectorAll('.current').forEach(x=>x.classList.remove('current'));e.classList.add('current');if(r.reveal!==false)scrollToLine(r.line);}
            emitPosition();
        }
    });
    report({type:'ready'});
});
