import EpubIframeView from "epubjs/src/managers/views/iframe.js";
import EpubRendition from "epubjs/src/rendition.js";
import { EVENTS } from "epubjs/src/utils/constants.js";
import EpubQueue from "epubjs/src/utils/queue.js";
import { createReaderTools } from "./readerTools.js";
import ePub from "epubjs";
import { compileCss } from "./customCss.js";
import { buildBookFontCatalog, applyBookFont, activeBookFonts, releaseBookFonts } from "./bookFontCatalog.js";
import { applyReadingOverrides, releaseReadingOverrides } from "./readingOverrides.js";
import { EpubNavigator, EpubPreferences, Injector } from "@readium/navigator";
import { Manifest, Publication, HttpFetcher, Locator } from "@readium/shared";
import { typographyProbe } from "./typographyProbe.js";
import EpubCFI from "epubjs/src/epubcfi.js";
import { pageKeyAction } from "./readerKeys.js";
import { ReaderViewManager } from "./readerViewManager.js";

// EPUB.js 0.3.93 advances its work queue through animation frames. A hidden
// native tab can suspend those frames before an iframe exists. Yield through
// timers while keeping every queued operation serialized; painting stays on RAF.
const runEpubQueue=EpubQueue.prototype.run;
EpubQueue.prototype.run=function(){this.tick=callback=>setTimeout(callback,0);return runEpubQueue.call(this);};
// Finish location mapping in the engine queue. Its default unawaited RAF can
// publish a new page with the previous CFI, especially in a background tab.
EpubRendition.prototype.reportLocation=function() {
    return this.q.enqueue(async()=>{
        const raw=await this.manager.currentLocation();if(!raw)return;
        const located=this.located(raw);if(!located?.start || !located?.end)return;
        this.location=located;
        this.emit(EVENTS.RENDITION.LOCATION_CHANGED,{index:located.start.index,href:located.start.href,start:located.start.cfi,end:located.end.cfi,percentage:located.start.percentage});
        this.emit(EVENTS.RENDITION.RELOCATED,located);
    });
};

// Percentage-height covers depend on the iframe viewport. EPUB.js measures
// them while the iframe is still zero pixels tall in scrolled-doc mode, then
// feeds that zero back into the iframe. Keep a viewport-sized minimum on the
// engine-owned frame; leave publisher XHTML and its inline styles intact.
class ReaderIframeView extends EpubIframeView {
    destroy() {
        const frame=this.iframe,doc=this.document;
        if(doc){releaseReadingOverrides(doc);releaseBookFonts(doc);profileStyles.delete(doc);appliedProfiles.delete(doc);cssErrors.delete(doc);inputDocuments.delete(doc);}
        super.destroy();
        // EPUB.js leaves its document and resolved render promise attached to
        // retired views. Navigate detached frames away from image-heavy pages
        // so their resources are released even before Chromium's next GC pass.
        this.document=undefined;this.window=undefined;this.sectionRender=undefined;
        if(doc){doc.fonts?.clear();doc.body?.replaceChildren();doc.head?.replaceChildren();}
        if(frame){frame.onload=null;frame.remove();frame.removeAttribute("srcdoc");frame.src="about:blank";}
    }
    setLayout(layout) {
        const key=JSON.stringify([layout.props,this.settings.axis,layout.settings.direction]);
        if(this.contents && this.appliedLayout===key){this.layout=layout;return;}
        super.setLayout(layout);
        if(this.contents)this.appliedLayout=key;
    }
    expand(force) {
        if (this.iframe && this.settings.axis === "vertical" && this.layout?.name === "reflowable")
            this.iframe.style.minHeight = `${Math.max(1, this.layout.height || pane.clientHeight)}px`;
        return super.expand(force);
    }
}

const encodePath = path => path.split("/").map(encodeURIComponent).join("/");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const root = "epub://app/book/";
const pane = document.getElementById("reader");
const viewport = document.getElementById("viewport");
let publication, engine, book, rendition, navigator;
let selectedToc=-1;
let fontCatalog;
const tools = createReaderTools({publication:()=>publication,documents,snapshot:currentSnapshot,restore:restoreLocator,book:()=>book,apply:applyDocument,navigate,remember});
const history=[];
function remember() {const locator=getLocator();if(locator && history.at(-1)!==locator){history.push(locator);if(history.length>64)history.shift();}}
let profile = { theme: "light", font: "Malgun Gothic", size: 19, weight: 400,
    letter: 0, word: 0, line: 1.8, before: 0, after: 0.8, indent: 0,
    align: "left", margin: 28, width: 760, override: true, background: "theme", backgroundColor: "", textColor: "",
    backgroundAsset: "", cssEnabled: false, cssForce: true, customCss: "",layout:"scroll",columns:0,backgroundFit:"cover",backgroundOpacity:1 };
const cssErrors = new WeakMap();
const profileStyles = new WeakMap();
const appliedProfiles = new WeakMap();
let readerBindings = { "epub.next": "PgDown", "epub.previous": "PgUp" };
let pendingCommands = 0, pendingPageMoves = 0;
let activeCommand = "", reflowStage = "";
const inputDocuments=new WeakSet();let keyboardSerial=0;
let customRules = [], backgroundImage = { asset: "", data: "" };
let sequence = Promise.resolve();
let generation = 0;
let renditionSize = null;
let resizeQueued = false;
let startupTimings = {};
// EPUB.js clears in-flight iframe views on automatic window resize. Fixed pixel
// dimensions disable that path; all reflow is serialized with display/navigation.
async function resizeRendition() {
    if (!rendition || engine !== "epubjs") return;
    const width = pane.clientWidth, height = pane.clientHeight;
    if (!width || !height || (renditionSize?.width === width && renditionSize?.height === height)) return;
    const locator = getLocator();
    renditionSize = { width, height };
    await rendition.q.enqueue(() => {});
    rendition.resize(width, height, locator || undefined);
    // resize already enqueues its display through onResized. Wait for that
    // display before another operation can clear its still-loading iframe.
    await rendition.q.enqueue(() => {});
}
async function settleFrame() {
    if(!rendition)return;
    await rendition.q.enqueue(() => {});
    await waitFor(() => documents().length && documents().every(doc => [...doc.images].every(img => img.complete && img.naturalWidth > 0)));
    await Promise.all(documents().map(doc => doc.fonts.ready));
    await new Promise(resolve => {
        const timer=setTimeout(resolve,80);
        requestAnimationFrame(()=>requestAnimationFrame(()=>{clearTimeout(timer);resolve();}));
    });
}
new ResizeObserver(() => {
    if (!rendition || engine !== "epubjs" || resizeQueued) return;
    const mine = generation;
    resizeQueued = true;
    sequence = sequence.then(async () => {
        const manager=rendition?.manager;manager?.beginTransition?.();
        try {
            if (mine === generation)do {await resizeRendition();await settleFrame();}
            while(mine===generation && (renditionSize?.width!==pane.clientWidth || renditionSize?.height!==pane.clientHeight));
        }
        finally {manager?.endTransition?.();resizeQueued = false;}
    }).catch(fail);
}).observe(pane);
// P0 adapter for pinned Navigator 2.13.2. The public injector lacks a core-script nonce option.
// Admit no publisher scripts; mark only scripts produced by the trusted injector, never blanket blob: scripts.
const injectForDocument = Injector.prototype.injectForDocument;
Injector.prototype.injectForDocument = async function (doc, link) {
    if (doc.querySelector("script")) throw Error("Publisher script denied before Readium injection");
    await injectForDocument.call(this, doc, link);
    for (const script of doc.querySelectorAll("script")) {
        if (!script.src.startsWith("blob:epub://app/") || script.textContent.trim()) throw Error("Unexpected injected script");
        script.setAttribute("nonce", publication.scriptNonce);
    }
};
window.hcEvidence = { status: "loading", phase: "P0-B", checks: {} };
const colors = { system: ["#ffffff", "#303840"], light: ["#fffdf7", "#303840"], dark: ["#242831", "#e9e6dd"],
    sepia: ["#eee1c5", "#453e32"], green: ["#e4eddf", "#293d2e"] };

function documents() {
    return [...pane.querySelectorAll("iframe")].filter(frame =>
        !frame.closest("[data-hc-retired]") && getComputedStyle(frame).visibility !== "hidden" && getComputedStyle(frame).display !== "none"
    ).map(frame => frame.contentDocument).filter(doc => doc?.body);
}

function applyDocument(doc) {
    tools.hook(doc);
    if(!inputDocuments.has(doc)) {
        inputDocuments.add(doc);
        doc.addEventListener("keydown",event=>{
            if(!tools.state().popup) {
                const pageAction=pageKeyAction(event,readerBindings);
                if(pageAction){event.preventDefault();if(!event.repeat || !pendingPageMoves)window.hcCommand("read",{action:pageAction,serial:--keyboardSerial});return;}
            }
            if(event.defaultPrevented || event.isComposing || event.ctrlKey || event.altKey || event.metaKey || tools.state().popup
                || event.target.closest?.("input,textarea,select,[contenteditable=true]"))return;
            let action;
            if(event.key===" " && !event.target.closest?.("a,button,img"))action=event.shiftKey?"previous":"next";
            if(profile.layout==="paginated" && !event.shiftKey && ["ArrowLeft","ArrowRight"].includes(event.key))
                action=(event.key==="ArrowRight")!==(readingLocation().readingProgression==="rtl" || book?.packaging?.metadata.direction==="rtl")?"next":"previous";
            if(action){event.preventDefault();window.hcCommand("read",{action,serial:--keyboardSerial});}
        });
    }
    if (fontCatalog) applyBookFont(doc, fontCatalog, profile);
    const [themeBackground, themeForeground] = colors[profile.theme];
    const profileKey = JSON.stringify([profile, themeBackground, themeForeground]);
    if (appliedProfiles.get(doc) === profileKey) return;
    let style = profileStyles.get(doc);
    if (!style) { style = doc.createElement("style"); doc.head.append(style); profileStyles.set(doc,style); }
    const background = profile.backgroundColor || themeBackground, foreground = profile.textColor || themeForeground;
    cssErrors.set(doc, applyReadingOverrides(doc, profile, foreground, background, customRules));
    style.textContent = profile.override ? `
      html, body { background-color:${profile.background === "theme" ? background : "transparent"}!important; color:${foreground}!important; }
      body, body p, body li, body blockquote, body td, body dd {
        font-family:${CSS.escape(profile.font)}!important; font-size:${profile.size}px!important;
        font-weight:${profile.weight}!important; line-height:${profile.line}!important;
        letter-spacing:${profile.letter}em!important; word-spacing:${profile.word}em!important;
        text-align:${profile.align}!important; color:${foreground}!important;
      }
      body p { margin-block-start:${profile.before}em!important; margin-block-end:${profile.after}em!important;
        text-indent:${profile.indent}em!important; }
      body img, body svg { max-width:100%!important; height:auto; }
    ` : "";
    style.textContent+="a:focus-visible, img:focus-visible { outline:2px solid currentColor!important; outline-offset:3px; }";
    appliedProfiles.set(doc,profileKey);
}

async function applyProfile(next = {}) {
    reflowStage="profile";
    const locator=getLocator(), oldLayout=profile.layout,oldColumns=profile.columns;
    const candidate = { ...profile, ...next };
    if (!colors[candidate.theme] || typeof candidate.font !== "string" || !candidate.font || candidate.font.length > 128
        || !["left", "center", "right", "justify"].includes(candidate.align)) throw Error("Invalid profile enum");
    const limits = { size: [12, 40], weight: [400, 700], letter: [-0.05, 0.2], word: [0, 0.8],
        line: [1, 3], before: [0, 3], after: [0, 3], indent: [0, 3], margin: [8, 80], width: [400, 1200] };
    for (const [key, [min, max]] of Object.entries(limits))
        if (!Number.isFinite(candidate[key]) || candidate[key] < min || candidate[key] > max) throw Error(`Invalid ${key}`);
    if (!["theme", "color", "paper", "grid", "image"].includes(candidate.background)
        || ![candidate.backgroundColor, candidate.textColor].every(v => typeof v === "string" && (!v || /^#[0-9a-f]{6}$/i.test(v)))) throw Error("Invalid background profile");
    customRules = candidate.cssEnabled ? compileCss(candidate.customCss) : [];
    if(!["scroll","paginated"].includes(candidate.layout) || ![0,1,2].includes(candidate.columns))throw Error("Invalid reading layout");
    if(!["cover","contain","tile"].includes(candidate.backgroundFit) || !Number.isFinite(candidate.backgroundOpacity) || candidate.backgroundOpacity<0 || candidate.backgroundOpacity>1)throw Error("Invalid background placement");
    profile = candidate;
    const [themeBackground, themeForeground] = colors[profile.theme];
    const background = profile.backgroundColor || themeBackground, foreground = profile.textColor || themeForeground;
    document.body.style.backgroundColor = background;
    document.body.style.color = foreground;document.body.style.fontFamily=profile.font;document.body.style.fontSize=`${profile.size}px`;
    document.body.style.backgroundImage = profile.background === "paper" ? "radial-gradient(rgba(127,97,48,.10) .6px,transparent .7px)"
        : profile.background === "grid" ? "linear-gradient(rgba(127,127,127,.10) 1px,transparent 1px),linear-gradient(90deg,rgba(127,127,127,.10) 1px,transparent 1px)"
        : profile.background === "image" && backgroundImage.asset === profile.backgroundAsset && backgroundImage.data ? `url("${backgroundImage.data}")` : "none";
    if(profile.background==="image" && backgroundImage.data) {
        const rgb=background.slice(1).match(/../g).map(v=>parseInt(v,16)).join(",");
        const fade=`rgba(${rgb},${1-profile.backgroundOpacity})`;
        document.body.style.backgroundImage=`linear-gradient(${fade},${fade}),url("${backgroundImage.data}")`;
    }
    document.body.style.backgroundRepeat=profile.backgroundFit==="tile" || ["paper","grid"].includes(profile.background)?"repeat":"no-repeat";
    document.body.style.backgroundSize = profile.background === "paper" ? "5px 5px" : profile.background === "grid" ? "24px 24px" : profile.backgroundFit==="tile"?"auto":profile.backgroundFit;
    document.body.style.backgroundPosition = "center";
    document.body.style.backgroundAttachment = "fixed";
    viewport.style.width = `min(calc(100% - ${profile.margin * 2}px), ${profile.width*(profile.layout==="paginated" && profile.columns!==1?2:1)}px)`;
    if (navigator) await navigator.submitPreferences(new EpubPreferences({
        scroll: profile.layout==="scroll", columnCount: profile.columns || "auto", backgroundColor: profile.override ? background : null,
        textColor: profile.override ? foreground : null,
        fontFamily: profile.override ? profile.font : null,
        // Readium scales the root with fontSize. Keep it neutral: the common adapter owns pixel size,
        // otherwise text and images are magnified again after our explicit font-size has been applied.
        fontSize: profile.override ? 1 : null,
        lineHeight: profile.override ? profile.line : null,
        // Separate paragraph-before/after and letter/word spacing are the common adapter's CSS contract.
    }));
    documents().forEach(applyDocument);
    if(rendition) {
        reflowStage="before-layout";
        await rendition.q.enqueue(() => {});
        reflowStage="flow";
        if(oldLayout!==profile.layout){rendition.flow(profile.layout==="paginated"?"paginated":"scrolled-doc");await rendition.q.enqueue(() => {});}
        if(oldLayout!==profile.layout || oldColumns!==profile.columns)rendition.spread(profile.columns===1 || profile.layout==="scroll"?"none":"auto",profile.columns===2?600:900);
        reflowStage="resize";await resizeRendition();
        reflowStage="restore";if(locator)await rendition.display(locator);
    }
    if (!rendition && !navigator){reflowStage="";return;}
    await sleep(150);
    documents().forEach(applyDocument);
    reflowStage="";
}

async function waitFor(predicate, timeout = 12000) {
    const end = performance.now() + timeout;
    while (performance.now() < end) { if (predicate()) return; await sleep(80); }
    throw Error("Renderer readiness timeout");
}

function bookFonts() {
    return fontCatalog ? activeBookFonts(fontCatalog, documents()[0]?.defaultView || window) : [];
}
window.hcBookFonts = bookFonts;

function currentSnapshot() {
    const docs = documents();
    return { ...tools.state(), ...readingLocation(), recovery: publication?.recovery || {}, engine, profile, cssError: docs.map(doc => cssErrors.get(doc)).find(Boolean) || "", backgroundKind: profile.background, backgroundImageApplied: document.body.style.backgroundImage !== "none", canvasColor: getComputedStyle(document.body).backgroundColor, bookFonts: bookFonts(), fontCatalogWarnings: fontCatalog?.warnings || [], languages: navigator?.publication.metadata.languages || [book?.packaging?.metadata.language].filter(Boolean),
        readingProgression: navigator?.readingProgression || book?.packaging?.metadata.direction || "default",
        text: docs.map(doc => doc.body.textContent).join(" ").trim().slice(0, 2000),
        paragraphs: docs.flatMap(doc => [...doc.querySelectorAll("p")].slice(0, 3).map(p => {
            const s = doc.defaultView.getComputedStyle(p);
            return { font: s.fontFamily, size: s.fontSize, weight: s.fontWeight, line: s.lineHeight,
                letter: s.letterSpacing, word: s.wordSpacing, before: s.marginBlockStart, after: s.marginBlockEnd,
                indent: s.textIndent, align: s.textAlign, color: s.color };
        })), images: docs.flatMap(doc => [...doc.images].map(img => ({ src: img.getAttribute("src"),
            complete: img.complete, naturalWidth: img.naturalWidth, width: img.getBoundingClientRect().width,
            alt: img.getAttribute("alt"), filter: doc.defaultView.getComputedStyle(img).filter }))),
        viewportWidth: viewport.clientWidth, paneWidth: pane.clientWidth, paneHeight: pane.clientHeight,
        frameBounds: [...pane.querySelectorAll("iframe")].filter(f => !f.closest("[data-hc-retired]") && getComputedStyle(f).visibility !== "hidden")
            .map(f => ({ width: f.clientWidth, height: f.clientHeight })),
        rootZoom: docs.map(doc => doc.defaultView.getComputedStyle(doc.documentElement).zoom),
        readerHorizontalScrollbar: !!rendition?.manager?.container && getComputedStyle(rendition.manager.container).overflowX !== "hidden" && rendition.manager.container.scrollWidth > rendition.manager.container.clientWidth+1,
        columnCount:rendition?._layout?.divisor || 1,
        frameId:docs[0]?.defaultView.frameElement?.id || "",
        readerBusy:pendingCommands>0 || resizeQueued, readerFocused:docs.some(doc=>doc.hasFocus()), locator: getLocator(), frameCount: docs.length };
}

function getLocator() {
    if (rendition) return rendition.currentLocation()?.start?.cfi || null;
    return navigator?.currentLocator?.serialize() || null;
}

function readingLocation() {
    const location=rendition?.currentLocation(), index=location?.start?.index ?? 0;
    const path=publication?.spine[index]?.path;
    let tocIndex=-1;
    const doc=documents()[0], section=book?.spine.get(index), compare=new EpubCFI();
    let selectedAnchor=null;
    for(const [i,item] of (publication?.navigation || []).entries()) {
        if(item.path!==path)continue;
        if(!item.fragment) {if(tocIndex<0)tocIndex=i;continue;}
        const target=doc?.getElementById(item.fragment);
        if(target && location?.start?.cfi && section) {
            try {const anchor=section.cfiFromElement(target);if(i===selectedToc)selectedAnchor=anchor;if(compare.compare(anchor,location.start.cfi)<=0)tocIndex=i;} catch {}
        }
    }
    // A chapter's final heading cannot always reach the viewport's top edge.
    // Retain an explicit TOC target while that heading is visible, then resume
    // location-based selection when the reader moves away.
    if(selectedAnchor && location?.end?.cfi && compare.compare(selectedAnchor,location.start.cfi)>=0 && compare.compare(selectedAnchor,location.end.cfi)<=0)tocIndex=selectedToc;
    else if(tocIndex!==selectedToc)selectedToc=-1;
    const page=location?.start?.displayed?.page || 1, pages=location?.start?.displayed?.total || 1;
    const count=publication?.spine.length || 1;
    return {chapterIndex:index,tocIndex,chapterTitle:publication?.navigation[tocIndex]?.label || path || "",
        chapterCount:count,chapterPage:page,chapterPages:pages,progress:location?.atEnd?1:Math.min(1,(index+(page-1)/pages)/count),
        atStart:!!location?.atStart,atEnd:!!location?.atEnd,canBack:history.length>0};
}

async function navigate(action,value) {
    if(action==="back") {const locator=history.pop();if(locator)await restoreLocator(locator);window.hcFocusReading?.();return;}
    if(action==="next" || action==="previous") {
        if(rendition)await rendition[action==="next"?"next":"prev"]();
        else await new Promise(resolve=>navigator[action==="next"?"goForward":"goBackward"](false,resolve));
        documents().forEach(applyDocument);window.hcFocusReading?.();return;
    }
    if(action==="next-chapter" || action==="previous-chapter") {
        const index=readingLocation().chapterIndex+(action==="next-chapter"?1:-1);
        if(index>=0 && index<publication.spine.length){remember();await goChapter(index);window.hcFocusReading?.();}return;
    }
    if(action!=="toc")throw Error("Unsupported reading navigation");
    const item=publication.navigation[value];if(!Number.isInteger(value) || !item)throw Error("Invalid TOC item");
    const index=publication.spine.findIndex(section=>section.path===item.path);
    if(index<0)throw Error("TOC target is outside reading order");
    remember();
    if(rendition)await rendition.display(book.spine.get(index).href+(item.fragment?"#"+item.fragment:""));
    else await goChapter(index);
    selectedToc=value;
    documents().forEach(applyDocument);
    window.hcFocusReading?.();
}

async function restoreLocator(locator) {
    if (rendition) await rendition.display(locator);
    else await new Promise((resolve, reject) => navigator.go(Locator.deserialize(locator), false,
        ok => ok ? resolve() : reject(Error("Locator restore failed"))));
    await sleep(350);
}

async function goChapter(index) {
    const item = publication.spine[index];
    if (!item) throw Error("Chapter index outside spine");
    if (rendition) await rendition.display(index);
    else await new Promise((resolve, reject) => navigator.goLink(
        navigator.publication.readingOrder.items[index], false, ok => ok ? resolve() : reject(Error("Navigation failed"))));
    await sleep(160);
    documents().forEach(applyDocument);
    await waitFor(() => documents().length && documents().every(doc => [...doc.images].every(img => img.complete && img.naturalWidth > 0)));
}

async function start(name) {
    const startedAt = performance.now();
    startupTimings = {};
    let stageAt = startedAt;
    const measured = name => { const now = performance.now(); startupTimings[name] = Math.round(now - stageAt); stageAt = now; };
    const mine = ++generation;
    selectedToc=-1;
    window.hcEvidence = { status: "loading", phase: "P0-B", engine: name, checks: {} };
    if (navigator) { await navigator.destroy(); navigator = null; }
    if (book) { book.destroy(); book = rendition = null; }
    pane.replaceChildren();
    renditionSize = null;
    engine = name;
    window.hcEvidence.stage = "publication";
    publication ||= await fetch("epub://app/app/publication.json").then(r => r.json());
    measured("publicationMs");
    window.hcEvidence.stage = "font-catalog";
    fontCatalog ||= await buildBookFontCatalog(publication, root);
    measured("fontCatalogMs");
    if (name === "epubjs") {
        book = ePub(root + encodePath(publication.rootfile), { openAs: "opf" });
        book.on("openFailed", error => { if (mine === generation) fail(error); });
        window.hcEvidence.stage = "book-ready";
        await book.ready;
        measured("bookReadyMs");
        renditionSize = { width: Math.max(1, pane.clientWidth), height: Math.max(1, pane.clientHeight) };
        rendition = book.renderTo(pane, { ...renditionSize, resizeOnOrientationChange: false, flow: profile.layout==="paginated"?"paginated":"scrolled-doc",
            manager: ReaderViewManager,
            view: ReaderIframeView, spread: profile.layout==="scroll" || profile.columns===1?"none":"auto", minSpreadWidth:profile.columns===2?600:900,
            allowScriptedContent: false, allowPopups: false });
        rendition.on("displayerror", error => { if (mine === generation) fail(error); });
        rendition.hooks.content.register(content => applyDocument(content.document));
        window.hcEvidence.stage = "first-display";
        await rendition.display();
        measured("firstDisplayMs");
    } else if (name === "readium") {
        const readingOrder = publication.spine.map(item => ({ href: encodePath(item.path), type: "application/xhtml+xml" }));
        const manifest = Manifest.deserialize({ "@context": "https://readium.org/webpub-manifest/context.jsonld",
            metadata: { title: publication.title, language: publication.languages || [],
                readingProgression: ["ltr", "rtl"].includes(publication.readingProgression) ? publication.readingProgression : undefined,
                conformsTo: ["https://readium.org/webpub-manifest/profiles/epub"],
                presentation: { layout: "reflowable" } },
            links: [{ href: root + "manifest.json", rel: "self", type: "application/webpub+json" }], readingOrder,
            resources: publication.resources.map(item => ({ href: encodePath(item.path), type: item.mediaType })),
            toc: publication.navigation.filter(item=>item.path).map(item => ({ href: encodePath(item.path)+(item.fragment?"#"+encodeURIComponent(item.fragment):""), title: item.label })) });
        if (!manifest) throw Error("Readium manifest rejected");
        const pub = new Publication({ manifest, fetcher: new HttpFetcher(undefined, root) });
        const positions = readingOrder.map((item, i) => Locator.deserialize({ ...item,
            locations: { position: i + 1, progression: 0, totalProgression: i / readingOrder.length } }));
        const noop = () => {};
        navigator = new EpubNavigator(pane, pub, { frameLoaded: wnd => applyDocument(wnd.document),
            positionChanged: noop, timelineItemChanged: noop, tap: () => false, click: () => false,
            zoom: noop, miscPointer: noop, scroll: noop, customEvent: noop, handleLocator: () => false,
            textSelected: noop, contentProtection: noop, contextMenu: noop, peripheral: noop }, positions, positions[0],
            { preferences: { scroll: true, columnCount: 1 }, defaults: {}, upgradeInsecureRequests: "never" });
        await navigator.load();
    } else throw Error("Unknown engine");
    window.hcEvidence.stage = "first-chapter";
    if (rendition) {
        // The host submits the profile before start and the content hook applies
        // it before display. Keep that first frame instead of displaying it twice
        // again merely to apply the same profile and navigate back to chapter 0.
        await resizeRendition();
        documents().forEach(applyDocument);
        await Promise.all(documents().map(doc => doc.fonts.ready));
        await waitFor(() => documents().length && documents().every(doc => [...doc.images].every(img => img.complete && img.naturalWidth > 0)));
    } else {
        await applyProfile();
        await goChapter(0);
    }
    await settleFrame();
    measured("firstChapterMs");
    startupTimings.totalMs = Math.round(performance.now() - startedAt);
    if (mine === generation) window.hcEvidence = { status: "ready", phase: "P0-B", ...currentSnapshot() };
}

async function selfTest() {
    const fixture = documents()[0]?.body.dataset.hcProbe;
    if (fixture) {
        window.hcEvidence = await typographyProbe({ documents, snapshot: currentSnapshot, apply: applyProfile,
            profile: () => profile, viewport: () => pane.getBoundingClientRect(), chapter: goChapter,
            locator: getLocator, restore: restoreLocator, publication, engine }, fixture);
        return;
    }
    const initialProfile = { ...profile };
    const initial = currentSnapshot();
    const checks = { noReaderHorizontalScrollbar: !initial.readerHorizontalScrollbar, firstText: initial.text.length > 10,
        imagesDecoded: initial.images.length > 0 && initial.images.every(img => img.complete && img.naturalWidth > 0),
        viewportFilled: initial.frameBounds.length > 0 && initial.frameBounds.every(f => f.width >= initial.paneWidth * 0.9 && f.height >= initial.paneHeight * 0.9),
        readingWidthApplied: initial.viewportWidth <= profile.width + 1,
        noDoubleFontScaling: initial.rootZoom.every(zoom => Math.abs(parseFloat(zoom || "1") - 1) < 0.01) };
    const initialChapter=readingLocation().chapterIndex;
    await applyProfile({layout:"paginated",columns:2});
    checks.paginatedLayout=rendition ? rendition.manager.isPaginated && readingLocation().chapterIndex===initialChapter : true;
    await applyProfile({layout:"scroll",columns:0});
    checks.scrollLayout=rendition ? !rendition.manager.isPaginated && readingLocation().chapterIndex===initialChapter : true;
    await applyProfile({ theme: "dark", size: 24, line: 2.1, before: 0.4, after: 1.3,
        letter: 0.08, word: 0.25, indent: 1.5, align: "justify", font: "Batang", weight: 700 });
    const typography = currentSnapshot();
    const p = typography.paragraphs[0];
    const near = (value, expected) => Math.abs(parseFloat(value) - expected) < 0.15;
    checks.typography = !!p && near(p.size, 24) && near(p.line, 50.4) && near(p.before, 9.6)
        && near(p.after, 31.2) && near(p.letter, 1.92) && near(p.word, 6) && near(p.indent, 36)
        && p.align === "justify" && p.font.includes("Batang") && p.weight === "700";
    checks.imageColorsPreserved = typography.images.every(img => img.filter === "none");
    await applyProfile({ line: 2.4 });
    const lineOnly = currentSnapshot().paragraphs[0];
    checks.lineSpacingIndependent = near(lineOnly.line, 57.6) && near(lineOnly.before, 9.6) && near(lineOnly.after, 31.2);
    await applyProfile({ before: 0.7, after: 1.8 });
    const paragraphOnly = currentSnapshot().paragraphs[0];
    checks.paragraphSpacingIndependent = near(paragraphOnly.line, 57.6) && near(paragraphOnly.before, 16.8) && near(paragraphOnly.after, 43.2);
    await applyProfile({ override: false });
    const publisher = currentSnapshot();
    checks.publisherReset = publisher.paragraphs[0]?.color === "rgb(48, 56, 64)"
        && !near(publisher.paragraphs[0]?.letter || "0", 1.92);
    await applyProfile({ ...initialProfile, override: true });
    if (publication.spine.length > 1) {
        await goChapter(1);
        const second = currentSnapshot();
        checks.chapterChanged = second.text.length > 0 && second.text !== initial.text;
        const locator = getLocator();
        await goChapter(0);
        await restoreLocator(locator);
        checks.locatorRestored = currentSnapshot().text === second.text;
    }
    const doc = documents()[0];
    await navigate("toc",0);
    checks.tocNavigation=readingLocation().chapterIndex===publication.spine.findIndex(item=>item.path===publication.navigation[0].path);
    const beforeJump=readingLocation().chapterIndex;
    if(beforeJump+1<publication.spine.length) {
        await navigate("next-chapter");checks.nextChapter=readingLocation().chapterIndex===beforeJump+1;
        await navigate("back");checks.backNavigation=readingLocation().chapterIndex===beforeJump;
    }
    const script = doc.createElement("script"); script.textContent = "window.hcUntrustedExecuted=1";
    doc.body.append(script);
    const button = doc.createElement("button"); button.setAttribute("onclick", "window.hcUntrustedExecuted=2");
    doc.body.append(button); button.click();
    await sleep(100);
    checks.bookScriptBlocked = !doc.defaultView.hcUntrustedExecuted;
    const blobScript = doc.createElement("script");
    const blobUrl = URL.createObjectURL(new Blob(["window.hcUntrustedExecuted=3"], { type: "text/javascript" }));
    blobScript.src = blobUrl; doc.body.append(blobScript);
    await sleep(100);
    checks.untrustedBlobScriptBlocked = !doc.defaultView.hcUntrustedExecuted;
    blobScript.remove(); URL.revokeObjectURL(blobUrl);
    script.remove(); button.remove();
    checks.remoteBlocked = !(await fetch("https://example.invalid/hc-epub-probe").then(() => true, () => false));
    checks.fileBlocked = !(await fetch("file:///C:/hc-epub-denied.txt").then(() => true, () => false));
    await goChapter(0);
    await applyProfile(initialProfile);
    window.hcEvidence = { status: Object.values(checks).every(Boolean) ? "passed" : "failed", phase: "P0-B",
        engine, checks, initial, typography, publisher, final: currentSnapshot(), sourceSha256: publication.sourceSha256,
        locatorScope: "same-session chapter restore; durable cross-edit locator unverified" };
}

function fail(error) { window.hcEvidence = { status: "error", phase: "P0-B", stage: window.hcEvidence?.stage, engine, message: String(error?.stack || error) }; }
window.hcValidateCss = source => {
    try { const rules = compileCss(source); return { ready: true, ok: true, rules: rules.length }; }
    catch (error) { return { ready: true, ok: false, error: error.message, line: error.line || 1, column: error.column || 1 }; }
};
window.hcCommand = (command, value) => {
    const pageMove=command==="read" && ["next","previous"].includes(value?.action);
    if(pageMove && pendingPageMoves>=2)return;
    if(pageMove)++pendingPageMoves;
    ++pendingCommands;
    sequence = sequence.then(async () => {
        activeCommand=command;
        const manager=rendition?.manager;manager?.beginTransition?.();
        try {
        if (command === "start") await start(value);
        else if (command === "profile") await applyProfile(value);
        else if (command === "palette") {
            if (!/^#[0-9a-f]{6}$/i.test(value?.background) || !/^#[0-9a-f]{6}$/i.test(value?.foreground)) throw Error("Invalid host palette");
            colors.system = [value.background, value.foreground];
            if (profile.theme === "system") await applyProfile(profile);
        }
        else if (command === "background") {
            if (!value.asset && !value.data) backgroundImage = { asset: "", data: "" };
            else {
                if (!/^[0-9a-f]{64}$/.test(value.asset) || typeof value.data !== "string" || value.data.length > 12*1024*1024
                    || !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(value.data)) throw Error("Invalid managed background");
                const image = new Image(); image.src = value.data; await image.decode(); backgroundImage = value;
            }
        }
        else if (command === "bindings") readerBindings=value;
        else if (command === "read") await tools.action(value);
        else if (command === "chapter") await goChapter(value);
        else if (command === "test") await selfTest();
        else throw Error("Unknown command");
        if(command!=="bindings")await settleFrame();
        if (window.hcEvidence.status === "ready") window.hcEvidence = { ...window.hcEvidence, ...currentSnapshot() };
        } finally {manager?.endTransition?.();}
    }).catch(fail).finally(()=>{activeCommand="";--pendingCommands;if(pageMove)--pendingPageMoves;});
};
window.addEventListener("error", event => fail(event.error || event.message));
window.addEventListener("unhandledrejection", event => fail(event.reason));

// Trusted-shell diagnostics only: no book script or mutable document authority.
window.hcStartupDiagnostics = () => ({
    activeCommand, reflowStage, pendingCommands, pendingPageMoves,
    jsHeapBytes:performance.memory?.usedJSHeapSize || 0,
    managedViews:rendition?.manager?.views?.length || 0,
    retiredViews:rendition?.manager?.retired?.length || 0,
    timings: startupTimings,
    visibility: document.visibilityState,
    paneWidth: pane.clientWidth, paneHeight: pane.clientHeight,
    renditionQueue: rendition ? { running: !!rendition.q?.running, pending: rendition.q?._q?.length } : null,
    managerQueue: rendition?.manager ? { running: !!rendition.manager.q?.running, pending: rendition.manager.q?._q?.length } : null,
    frames: [...pane.querySelectorAll("iframe")].map(frame => ({
        readyState: frame.contentDocument?.readyState, width: frame.clientWidth, height: frame.clientHeight
    }))
});

window.hcSnapshot = () => window.hcEvidence.status === "ready" ? {...window.hcEvidence,...currentSnapshot()} : window.hcEvidence;
window.hcFocusReading=()=>{
    if(tools.state().popup)return false;
    const doc=documents()[0];if(!doc)return false;
    const frame=doc.defaultView.frameElement;frame.tabIndex=0;frame.focus({preventScroll:true});
    doc.body.tabIndex=-1;doc.body.focus({preventScroll:true});return doc.hasFocus();
};
