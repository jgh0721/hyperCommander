import MarkdownIt from 'markdown-it';
import footnote from 'markdown-it-footnote';
import tasks from 'markdown-it-task-lists';
import katex from 'katex';
import mermaid from 'mermaid';
import hljs from 'highlight.js/lib/common';

// Only the fixed application shell owns the bridge. Document HTML is disabled.
let revision = 0, options = {}, bridge, suppression = 0, serial = 0;
const root = document.getElementById('content');
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
function emitPosition() {
    const e=anchorAtTop();if(!e)return;
    const headings=[...root.querySelectorAll('h1,h2,h3,h4,h5,h6')];const h=headings.filter(x=>x.getBoundingClientRect().top<100).at(-1)||headings[0];
    report({type:'scroll',line:Number(e.dataset.line),ratio:e.getBoundingClientRect().top/innerHeight,user:Date.now()>suppression,progress:document.documentElement.scrollHeight<=innerHeight?100:Math.round(scrollY/(document.documentElement.scrollHeight-innerHeight)*100),heading:h?.textContent||'',headingLine:Number(h?.dataset.line||0)});
}
function fonts(f={}) {
    root.style.fontFamily=f.enabled&&f.mode==='user'?f.bodyFamily:'';
    root.style.fontSize=f.enabled&&f.mode==='user'?`${f.bodySizePt||11}pt`:'';
    root.style.lineHeight=f.enabled&&f.mode!=='document'?String(f.lineHeight||1.7):'';
    document.querySelectorAll('#content h1,#content h2,#content h3,#content h4,#content h5,#content h6').forEach(e=>e.style.fontFamily=f.enabled&&f.mode!=='document'&&f.headings?root.style.fontFamily:'Segoe UI, Malgun Gothic, sans-serif');
    root.querySelectorAll('pre:not(.mermaid) code, p>code, li>code').forEach(e=>{e.style.fontFamily=f.enabled&&f.mode==='user'?f.codeFamily:'';e.style.fontSize=f.enabled&&f.mode==='user'?`${f.codeSizePt||10}pt`:''});
}
async function render(request) {
    const generation=++serial;revision=request.revision;options=request.options||{};
    const old=anchorAtTop(),position=old?{line:Number(old.dataset.line),ratio:old.getBoundingClientRect().top/innerHeight}:null;
    document.documentElement.dataset.theme=options.dark?'dark':'light';
    document.getElementById('base').href=request.baseUrl||'qrc:/markdown/';
    const md=parser(),tokens=md.parse(request.text,{}),headings=[],slugs=new Map();
    for(let i=0;i<tokens.length;i++){const t=tokens[i];if(t.map){t.attrSet('data-line',String(t.map[0]+1));t.attrSet('data-end',String(t.map[1]+1))}if(t.type==='heading_open'){const title=(tokens[i+1].children||[]).filter(x=>['text','code_inline'].includes(x.type)).map(x=>x.content).join('');const slug=title.trim().toLowerCase().replace(/[^\p{L}\p{N}_ -]/gu,'').replace(/\s+/g,'-')||'heading';const count=slugs.get(slug)||0;slugs.set(slug,count+1);const id=slug+(count?'-'+count:'');t.attrSet('id',id);headings.push({text:title,level:Number(t.tag[1]),line:t.map[0]+1,id});}}
    root.innerHTML=md.renderer.render(tokens,md.options,{});
    root.querySelectorAll('blockquote').forEach(e=>{const p=e.firstElementChild,m=p?.textContent.match(/^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]/);if(m){e.classList.add('alert');e.dataset.kind=m[1];}});
    fonts(options.previewFonts);
    mermaid.initialize({startOnLoad:false,securityLevel:'strict',theme:options.dark?'dark':'default',maxTextSize:50000,maxEdges:500,secure:['securityLevel','maxTextSize','maxEdges','startOnLoad']});
    for(const e of root.querySelectorAll('.mermaid')){if(generation!==serial)return;try{const result=await mermaid.render(`diagram-${generation}-${e.dataset.line}`,e.textContent);if(generation!==serial)return;e.innerHTML=result.svg;}catch(error){if(generation!==serial)return;e.classList.add('problem');e.title=String(error);}}
    await document.fonts.ready;if(generation!==serial)return;
    if(position)scrollToLine(position.line,position.ratio);
    const details=[...root.querySelectorAll('.problem')].map(e=>({kind:e.classList.contains('mermaid')?'Mermaid':'KaTeX',line:Number(e.closest('[data-line]')?.dataset.line||1),message:e.title||e.textContent}));report({type:'rendered',headings,problems:details.length,details});emitPosition();
}
function scrollToLine(line,ratio=0.2){const all=[...root.querySelectorAll('[data-line]')];const e=all.filter(e=>Number(e.dataset.line)<=line).at(-1)||all[0];if(e){suppression=Date.now()+300;scrollTo(0,scrollY+e.getBoundingClientRect().top-innerHeight*ratio);}}
window.hcMarkdown={render,scrollToLine};
let scheduled=false;addEventListener('scroll',()=>{if(scheduled)return;scheduled=true;requestAnimationFrame(()=>{scheduled=false;emitPosition()})});
root.addEventListener('click',e=>{const link=e.target.closest('a');if(link){e.preventDefault();const href=link.getAttribute('href');if(href?.startsWith('#'))document.getElementById(decodeURIComponent(href.slice(1)))?.scrollIntoView();else report({type:'link',url:link.href});return;}const block=e.target.closest('[data-line]');if(block)report({type:'position',line:Number(block.dataset.line)});});
root.addEventListener('load',()=>emitPosition(),true);
if(window.qt)new QWebChannel(qt.webChannelTransport,channel=>{bridge=channel.objects.markdown;bridge.request.connect(s=>{const r=JSON.parse(s);if(r.type==='render')render(r).catch(e=>report({type:'error',message:String(e)}));else if(r.type==='scroll')scrollToLine(r.line,r.ratio);else if(r.type==='position'){const e=[...root.querySelectorAll('[data-line]')].filter(e=>Number(e.dataset.line)<=r.line).at(-1);if(e){root.querySelectorAll('.current').forEach(x=>x.classList.remove('current'));e.classList.add('current');if(!r.nearest||e.getBoundingClientRect().bottom<0||e.getBoundingClientRect().top>innerHeight)scrollToLine(r.line);}}});report({type:'ready'});});
