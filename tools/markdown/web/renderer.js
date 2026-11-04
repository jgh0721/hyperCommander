import MarkdownIt from 'markdown-it';
import footnote from 'markdown-it-footnote';
import tasks from 'markdown-it-task-lists';
import katex from 'katex';
import mermaid from 'mermaid';
import hljs from 'highlight.js/lib/common';

// Only the fixed application shell owns the bridge. Document HTML is disabled.
let revision = 0, options = {}, bridge, suppression = 0, serial = 0, displayedRevision = -1;
// Source of the displayed revision: anchors are (line, column) in this text.
let sourceText = '', lineStarts = [0];
// sourceAnchor: latest source request for the rendering revision, applied at commit.
// pinned: last applied source request, re-applied when late images or fonts move the layout.
let sourceAnchor = null, pinned = null, appliedAnchor = null, appliedSerial = 0, currentPosition = 0;
const root = document.getElementById('content');
root.classList.add('markdown-content');
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

// ---- User intent -------------------------------------------------------------------------
// Same events and rule as tools/epub/web/previewScroll.js: only a scroll soon after the user's
// own input moves the editor. The page also scrolls by itself while images and fonts settle.
const INTENT_TAIL_MS = 500, LAYOUT_SETTLE_MS = 250;
const INTENT_KIND = {wheel:'wheel', keydown:'key', mousedown:'pointer', pointerdown:'pointer', pointermove:'pointer', touchstart:'touch', touchmove:'touch'};
const intent = {until: 0, kind: ''};
let layoutAt = 0;
for (const type of Object.keys(INTENT_KIND)) addEventListener(type, event => {
    if (type === 'pointermove' && !event.buttons) return;
    intent.until = Date.now() + INTENT_TAIL_MS; intent.kind = INTENT_KIND[type];
    // The user now places the preview; layout changes no longer re-apply the editor's anchor.
    pinned = null;
}, {capture: true, passive: true});
// What moved the page: '' for this script or settling layout, otherwise the user's input.
// Dragging or pressing the viewport scrollbar and autoscroll deliver no page events; a
// scroll that no request, input or recent layout change explains is one of those.
function scrollCause() {
    const now = Date.now();
    if (now <= suppression) return '';
    if (now < intent.until) return intent.kind;
    if (now - layoutAt <= LAYOUT_SETTLE_MS) return '';
    pinned = null;
    return 'scrollbar';
}
addEventListener('focus', () => report({type: 'focus', focused: true}));
addEventListener('blur', () => report({type: 'focus', focused: false}));

// ---- Source lines --------------------------------------------------------------------------
function setSource(text) {
    sourceText = text || ''; lineStarts = [0];
    for (let at = sourceText.indexOf('\n'); at >= 0; at = sourceText.indexOf('\n', at + 1)) lineStarts.push(at + 1);
}
// 1-based line and UTF-16 column -> offset in sourceText.
function sourceOffset(line, column) {
    const index = Math.max(0, Math.min(lineStarts.length - 1, Math.floor(line) - 1));
    const lineEnd = index + 1 < lineStarts.length ? lineStarts[index + 1] - 1 : sourceText.length;
    return Math.min(lineEnd, lineStarts[index] + Math.max(0, column || 0));
}
function sourceLineColumn(offset) {
    let low = 0, high = lineStarts.length - 1;
    while (low < high) { const middle = (low + high + 1) >> 1; if (lineStarts[middle] <= offset) low = middle; else high = middle - 1; }
    return {line: low + 1, column: offset - lineStarts[low]};
}

// ---- Blocks and the anchor table ----------------------------------------------------------
// [data-line] elements of the displayed revision in document order, which is also start-line order.
let blocks = [], blockOf = new Map(), headings = [], alignments = new WeakMap();
let anchorTable = null, anchorKey = '';
const counters = {anchorBuilds: 0, anchorMs: 0, alignments: 0};
function collectBlocks() {
    blocks = [...root.querySelectorAll('[data-line]')].map(e => {
        const line = Number(e.dataset.line);
        return {e, line, end: Math.max(line + 1, Number(e.dataset.end) || 0), top: e.parentElement === root};
    }).filter(b => b.line > 0);
    blockOf = new Map(blocks.map(b => [b.e, b]));
    headings = [...root.querySelectorAll('h1,h2,h3,h4,h5,h6')];
    relayout();
}
// Layout moved: measured positions and text boxes are stale.
function relayout() { anchorTable = null; alignments = new WeakMap(); layoutAt = Date.now(); }
// The innermost block containing the line, or the last block starting before it.
function blockFor(line) {
    let low = 0, high = blocks.length - 1, at = -1;
    while (low <= high) { const middle = (low + high) >> 1; if (blocks[middle].line <= line) { at = middle; low = middle + 1; } else high = middle - 1; }
    if (at < 0) return blocks[0] || null;
    // Walking back reaches the containing ancestors. An earlier top-level block that ends before
    // the line ends the walk: nothing before it can contain the line.
    for (let i = at; i >= 0; --i) { const b = blocks[i]; if (b.end > line) return b; if (b.top) break; }
    return blocks[at];
}
// Both directions interpolate through the same measured blocks, in document coordinates, so the
// table only changes with the layout. Rebuilt when the revision, height or width changes.
function anchors() {
    const key = `${displayedRevision}|${document.documentElement.scrollHeight}|${root.clientWidth}`;
    if (anchorTable && key === anchorKey) return anchorTable;
    const started = performance.now(), points = new Map();
    for (const b of blocks) {
        const box = b.e.getBoundingClientRect();
        if (box.height <= 0) continue;
        const point = {line: b.line, end: b.end, y: scrollY + box.top, height: box.height};
        // Several blocks can start on one line; the smallest locates it best.
        if (!points.has(b.line) || box.height < points.get(b.line).height) points.set(b.line, point);
    }
    const table = [];
    for (const p of [...points.values()].sort((a, b) => a.y - b.y || a.line - b.line)) {
        const last = table.at(-1); if (!last || (p.line > last.line && p.y > last.y)) table.push(p);
    }
    const last = table.at(-1); if (last) table.push({line: Math.max(last.line + 1, last.end), y: last.y + last.height});
    anchorKey = key; anchorTable = table; ++counters.anchorBuilds; counters.anchorMs = performance.now() - started;
    return table;
}
function interpolate(key, value, other) {
    const points = anchors(); if (!points.length) return 0;
    if (value <= points[0][key]) return points[0][other];
    let low = 0, high = points.length - 1;
    if (value >= points[high][key]) return points[high][other];
    while (high - low > 1) { const middle = (low + high) >> 1; if (points[middle][key] <= value) low = middle; else high = middle; }
    const a = points[low], b = points[high];
    return a[other] + (b[other] - a[other]) * (value - a[key]) / Math.max(1e-6, b[key] - a[key]);
}

// ---- Source characters <-> rendered characters -------------------------------------------
// markdown-it keeps no source columns for inline text, so a block's rendered text is aligned with
// its source text in order. Markup, link destinations and math sources never appear as text.
const NO_TEXT = '.katex,.mermaid,svg,img,script,style,input';
const ALIGN_WINDOW = 96;
function sourceGaps(text) {
    const skip = new Uint8Array(text.length);
    const mark = (from, to) => { for (let i = from; i < to && i < text.length; ++i) skip[i] = 1; };
    for (const m of text.matchAll(/!\[[^\]\n]*\]\([^)\n]*\)/g)) mark(m.index, m.index + m[0].length);
    for (const m of text.matchAll(/\]\([^)\n]*\)/g)) mark(m.index + 1, m.index + m[0].length);
    for (const m of text.matchAll(/\$\$[\s\S]*?\$\$|\$[^$\n]+\$|\\\([\s\S]*?\\\)|\\\[[\s\S]*?\\\]/g)) mark(m.index, m.index + m[0].length);
    return skip;
}
function textNodes(element) {
    const out = [];
    const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT, {acceptNode: node => {
        const hidden = node.parentElement?.closest(NO_TEXT);
        return hidden && element.contains(hidden) ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT;
    }});
    for (let node; (node = walker.nextNode());) out.push(node);
    return out;
}
// For each rendered character of the block, the source offset it came from. Unmatched characters
// (list markers, generated footnote numbers) inherit the previous offset, keeping the map monotonic.
function alignment(block) {
    let a = alignments.get(block.e); if (a) return a;
    const from = lineStarts[Math.min(lineStarts.length - 1, block.line - 1)] ?? sourceText.length;
    const to = block.end - 1 < lineStarts.length ? lineStarts[block.end - 1] : sourceText.length;
    const source = sourceText.slice(from, to), skip = sourceGaps(source), nodes = [];
    let total = 0; for (const node of textNodes(block.e)) { nodes.push({node, start: total}); total += node.data.length; }
    const offsets = new Int32Array(total), same = (s, r) => s === r || (/\s/.test(s) && /\s/.test(r));
    let i = 0, last = from;
    for (const {node, start} of nodes) {
        const text = node.data;
        for (let j = 0; j < text.length; ++j) {
            let k = i;
            while (k < source.length && k - i < ALIGN_WINDOW && (skip[k] || !same(source[k], text[j]))) ++k;
            if (k < source.length && k - i < ALIGN_WINDOW) { last = from + k; i = k + 1; }
            offsets[start + j] = last;
        }
    }
    a = {nodes, offsets, total}; alignments.set(block.e, a); ++counters.alignments;
    return a;
}
function nodeAt(a, index) {
    let low = 0, high = a.nodes.length - 1;
    while (low < high) { const middle = (low + high + 1) >> 1; if (a.nodes[middle].start <= index) low = middle; else high = middle - 1; }
    return a.nodes[low];
}
// The box of one rendered character. A line feed has no box of its own; its neighbour has.
function charBox(node, at) {
    const text = node.data;
    if (text[at] === '\n') at = at + 1 < text.length ? at + 1 : Math.max(0, at - 1);
    const code = text.charCodeAt(at), end = Math.min(text.length, at + (code >= 0xd800 && code < 0xdc00 ? 2 : 1));
    const range = document.createRange(); range.setStart(node, at); range.setEnd(node, Math.max(at, end));
    const box = [...range.getClientRects()].find(r => r.height > 0);
    return box || null;
}
// Document y of the top of the rendered line holding the source character, or null.
function glyphTop(block, offset) {
    const a = alignment(block); if (!a.total) return null;
    let low = 0, high = a.total - 1, index = 0;
    while (low <= high) { const middle = (low + high) >> 1; if (a.offsets[middle] <= offset) { index = middle; low = middle + 1; } else high = middle - 1; }
    const {node, start} = nodeAt(a, index), box = charBox(node, index - start);
    return box ? scrollY + box.top : null;
}
// Without text (images, math, diagrams) a line keeps its share of the block's height.
function lineTop(line) {
    const b = blockFor(line); if (!b) return null;
    if (line < b.line || line >= b.end) return interpolate('line', line, 'y');
    const box = b.e.getBoundingClientRect();
    return scrollY + box.top + box.height * Math.max(0, Math.min(.999, (line - b.line) / (b.end - b.line)));
}
function anchorY(line, column) {
    const b = blockFor(line);
    if (b && Number.isFinite(column) && line >= b.line && line < b.end) {
        const y = glyphTop(b, sourceOffset(line, column)); if (y !== null) return y;
    }
    return lineTop(line);
}
// The DOM caret (text node and offset) at a viewport point.
function caretAt(x, y) {
    const position = document.caretPositionFromPoint?.(x, y);
    if (position) return {node: position.offsetNode, at: position.offset};
    const range = document.caretRangeFromPoint?.(x, y);
    return {node: range?.startContainer || null, at: range?.startOffset || 0};
}
// The source character at the preview's reference height and the top of its rendered line.
function previewAnchor(height = innerHeight * .5) {
    const fallback = () => ({line: interpolate('y', scrollY + height, 'line'), column: null, offset: height});
    const content = root.getBoundingClientRect(), x = content.left + (parseFloat(getComputedStyle(root).paddingLeft) || 0) + 2;
    const {node, at} = caretAt(x, height);
    if (!node || node.nodeType !== Node.TEXT_NODE || !root.contains(node)) return fallback();
    const element = node.parentElement.closest('[data-line]'), block = element && blockOf.get(element);
    if (!block) return fallback();
    const a = alignment(block), entry = a.nodes.find(n => n.node === node);
    if (!entry || !a.total) return fallback();
    const index = Math.min(a.total - 1, entry.start + Math.min(at, node.data.length - 1)), box = charBox(node, index - entry.start);
    if (!box || box.bottom < 0 || box.top > innerHeight) return fallback();
    return {...sourceLineColumn(a.offsets[index]), offset: box.top};
}
// The source position under a viewport point: the character where it resolves, otherwise the
// block's first line, otherwise the interpolated line. Line 0 means no rendered content.
function sourceAt(x, y) {
    if (!blocks.length) return {line: 0, column: null};
    const {node, at} = caretAt(x, y);
    const element = (node?.nodeType === Node.TEXT_NODE ? node.parentElement : document.elementFromPoint(x, y))?.closest?.('[data-line]');
    const block = element && root.contains(element) ? blockOf.get(element) : null;
    if (!block) return {line: Math.max(1, interpolate('y', scrollY + y, 'line')), column: null};
    const a = alignment(block), entry = node?.nodeType === Node.TEXT_NODE ? a.nodes.find(n => n.node === node) : null;
    if (!entry || !a.total) return {line: block.line, column: null};
    return sourceLineColumn(a.offsets[Math.min(a.total - 1, entry.start + Math.max(0, Math.min(at, node.data.length - 1)))]);
}

// ---- Applying and reporting positions -------------------------------------------------------
// request: {line, column, offset, top, end, caret, serial} in the displayed revision. offset is the
// CSS px height from the viewport top where the anchor character's line goes.
function applyScroll(request) {
    if (!Number.isFinite(request.line)) return;
    suppression = Date.now() + 300;
    const max = Math.max(0, document.documentElement.scrollHeight - innerHeight);
    let wanted = 0;
    if (request.end) wanted = max;
    else if (!request.top) {
        const y = anchorY(request.line, request.column);
        wanted = (y ?? 0) - (Number.isFinite(request.offset) ? request.offset : innerHeight * (request.ratio ?? .2));
    }
    const target = Math.max(0, Math.min(max, wanted));
    if (Math.abs(scrollY - target) >= .5) scrollTo(0, target);
    appliedAnchor = {line: request.line, column: request.column ?? null, offset: request.offset ?? null, caret: !!request.caret,
        clamped: Math.abs(wanted - target) > 2};
    if (Number.isFinite(request.serial)) appliedSerial = Math.max(appliedSerial, request.serial);
    pinned = request;
}
// Requests arriving faster than frames are merged: only the newest is applied, once per frame.
let pendingScroll = null, applyQueued = false;
function queueScroll(request) {
    pendingScroll = request; if (applyQueued) return; applyQueued = true;
    const run = () => {
        if (!applyQueued) return; applyQueued = false;
        const next = pendingScroll; pendingScroll = null;
        if (next && displayedRevision === revision) { applyScroll(next); emitPosition(); }
    };
    // Chromium can suspend animation callbacks in an occluded native preview.
    requestAnimationFrame(run); setTimeout(run, 50);
}
// Late images, fonts and width changes move the layout under an applied anchor.
let relayoutQueued = false;
function layoutChanged() {
    relayout();
    if (relayoutQueued) return; relayoutQueued = true;
    const run = () => {
        if (!relayoutQueued) return; relayoutQueued = false;
        if (pinned && displayedRevision === revision && Date.now() >= intent.until) applyScroll({...pinned, serial: undefined});
        emitPosition();
    };
    requestAnimationFrame(run); setTimeout(run, 50);
}
function currentHeading() {
    // Headings are in document order, so their tops rise monotonically.
    let low = 0, high = headings.length - 1, found = -1;
    while (low <= high) { const middle = (low + high) >> 1; if (headings[middle].getBoundingClientRect().top < 100) { found = middle; low = middle + 1; } else high = middle - 1; }
    return headings[Math.max(0, found)] || null;
}
function emitPosition() {
    if (!root.children.length || displayedRevision !== revision) return;
    const h = currentHeading(), marked = blockFor(currentPosition), box = marked?.e.getBoundingClientRect();
    const anchor = previewAnchor(), max = document.documentElement.scrollHeight - innerHeight, cause = scrollCause();
    let applied = null;
    if (appliedAnchor) {
        const y = anchorY(appliedAnchor.line, appliedAnchor.column), top = y === null ? null : y - scrollY;
        applied = {...appliedAnchor, actualOffset: top, visible: top !== null && top >= 0 && top <= innerHeight};
    }
    report({type: 'scroll', offset: scrollY, viewportHeight: innerHeight, line: anchor.line, column: anchor.column, anchorOffset: anchor.offset,
        ratio: anchor.offset / Math.max(1, innerHeight), user: cause !== '', intent: cause,
        top: scrollY <= 1, bottom: scrollY >= max - 1, appliedAnchor: applied, appliedSerial,
        position: {line: currentPosition, visible: !!box && box.bottom > 0 && box.top < innerHeight},
        anchorBuilds: counters.anchorBuilds, anchorMs: counters.anchorMs, alignments: counters.alignments,
        progress: max <= 0 ? 100 : Math.round(scrollY / max * 100), heading: h?.textContent || '', headingLine: Number(h?.dataset.line || 0)});
}
// Measurements for tests only; nothing here runs unless the host asks.
function diagnostics(request) {
    const result = {type: 'diagnostics', id: request.id ?? 0, viewportHeight: innerHeight, documentHeight: document.documentElement.scrollHeight,
        viewportWidth: document.documentElement.clientWidth, scrollY, tokens: {}, images: {}};
    if (request.textRows) {
        let rows = 0;
        for (const block of root.querySelectorAll('p,pre code')) {
            if (!block.textContent.trim()) continue;
            const range = document.createRange(); range.selectNodeContents(block);
            rows = Math.max(rows, new Set([...range.getClientRects()].filter(r => r.height > 0).map(r => Math.round(r.top))).size);
        }
        result.textRows = rows;
    }
    const wanted = new Set(request.tokens || []);
    if (wanted.size) for (const node of textNodes(root)) for (const token of wanted) {
        const at = node.data.indexOf(token); if (at < 0) continue;
        const range = document.createRange(); range.setStart(node, at); range.setEnd(node, at + token.length);
        const box = [...range.getClientRects()].find(r => r.height > 0); if (!box) continue;
        result.tokens[token] = {top: box.top, bottom: box.bottom, height: box.height, visible: box.bottom > 0 && box.top < innerHeight};
        wanted.delete(token);
    }
    for (const name of request.images || []) {
        const image = [...root.querySelectorAll('img')].find(i => (i.getAttribute('src') || '').endsWith(name)); if (!image) continue;
        const box = image.getBoundingClientRect(); result.images[name] = {top: box.top, bottom: box.bottom, visible: box.bottom > 0 && box.top < innerHeight};
    }
    report(result);
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
    const md=parser(),tokens=md.parse(request.text,{}),outline=[],slugs=new Map();
    for(let i=0;i<tokens.length;i++){const t=tokens[i];if(t.map){t.attrSet('data-line',String(t.map[0]+1));t.attrSet('data-end',String(t.map[1]+1))}if(t.type==='heading_open'){const title=(tokens[i+1].children||[]).filter(x=>['text','code_inline'].includes(x.type)).map(x=>x.content).join('');const slug=title.trim().toLowerCase().replace(/[^\p{L}\p{N}_ -]/gu,'').replace(/\s+/g,'-')||'heading';const count=slugs.get(slug)||0;slugs.set(slug,count+1);const id=slug+(count?'-'+count:'');t.attrSet('id',id);outline.push({text:title,level:Number(t.tag[1]),line:t.map[0]+1,id});}}
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
        const previous=scrollY<=1||displayedRevision<0?{line:1,top:true}:previewAnchor();
        suppression=Date.now()+300;
        root.replaceChildren(...children);fonts(options.previewFonts);
        setSource(request.text);displayedRevision=revision;collectBlocks();applyScroll(sourceAnchor||previous);
        const details=[...root.querySelectorAll('.problem')].map(e=>({kind:e.classList.contains('mermaid')?'Mermaid':'KaTeX',line:Number(e.closest('[data-line]')?.dataset.line||1),message:e.title||e.textContent}));
        report({type:'rendered',headings:outline,problems:details.length,details});emitPosition();
    } catch(error) {if(generation===serial)throw error;} finally {next.remove();}
}
function scrollToLine(line,ratio=0.2,end=false){applyScroll({line,ratio,end});}
window.hcMarkdown={render,scrollToLine,diagnostics:request=>{diagnostics(request);}};
let scheduled=false;addEventListener('scroll',()=>{if(scheduled)return;scheduled=true;requestAnimationFrame(()=>{scheduled=false;emitPosition()})});
addEventListener('resize',layoutChanged);
document.fonts?.addEventListener?.('loadingdone',layoutChanged);
root.addEventListener('load',layoutChanged,true);
root.addEventListener('click',e=>{
    const link=e.target.closest('a');
    if(link){e.preventDefault();const href=link.getAttribute('href');if(href?.startsWith('#'))document.getElementById(decodeURIComponent(href.slice(1)))?.scrollIntoView();else report({type:'link',url:link.href});return;}
    const block=e.target.closest('[data-line]');if(!block)return;
    // The clicked character, where it can be resolved, keeps its height in the editor.
    const hit=previewAnchor(e.clientY),exact=hit.column!==null&&Math.abs(hit.offset-e.clientY)<64;
    report({type:'position',line:exact?hit.line:Number(block.dataset.line),column:exact?hit.column:null,offset:exact?hit.offset:e.clientY});
});
// The host draws a themed menu instead of Chromium's. x and y are CSS px in the viewport; line and
// column are in the displayed revision. Without the bridge (still loading) Chromium's own request
// reaches the host's fallback, so the event is left alone.
addEventListener('contextmenu',e=>{
    if(!bridge)return;
    e.preventDefault();
    const link=e.target.closest?.('a[href]'),href=link?.getAttribute('href')||'',hit=sourceAt(e.clientX,e.clientY);
    let fragment='';if(href.startsWith('#'))try{fragment=decodeURIComponent(href.slice(1));}catch{fragment=href.slice(1);}
    // The host only needs to know a selection exists; a whole-document selection must not fill the pipe.
    report({type:'context',x:e.clientX,y:e.clientY,selection:String(getSelection()||'').slice(0,1024),link:link?link.href:'',
        fragment,line:hit.line,column:hit.column});
});
if(window.qt)new QWebChannel(qt.webChannelTransport,channel=>{
    bridge=channel.objects.markdown;
    bridge.request.connect(s=>{
        const r=JSON.parse(s);
        if(r.type==='render')render(r).catch(e=>report({type:'error',message:String(e)}));
        else if(r.type==='diagnostics')diagnostics(r);
        else if(r.revision!==revision)return;
        else if(r.type==='scroll'){
            sourceAnchor=r;if(displayedRevision===revision)queueScroll(r);
        } else if(r.type==='position'){
            currentPosition=r.line;
            if(displayedRevision!==revision)return;
            const b=blockFor(r.line);
            if(b){root.querySelectorAll('.current').forEach(x=>x.classList.remove('current'));b.e.classList.add('current');if(r.reveal!==false)scrollToLine(r.line);}
            emitPosition();
        } else if(r.type==='anchor'){
            // An in-document link opened from the host's context menu, as a click would.
            if(displayedRevision===revision)document.getElementById(r.id)?.scrollIntoView();
        }
    });
    report({type:'ready'});
});
