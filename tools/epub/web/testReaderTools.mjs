import assert from "node:assert/strict";
import {build} from "esbuild";
import {fileURLToPath} from "node:url";
// Match the product bundler: EPUB.js upstream imports omit .js extensions.
const bundled=await build({stdin:{contents:'export {findText,validCfi,createReaderTools} from "./readerTools.js"; export {placeSourcePosition} from "./sourcePositions.js";',resolveDir:fileURLToPath(new URL(".",import.meta.url))},bundle:true,platform:"node",format:"esm",write:false});
const {findText,validCfi,createReaderTools,placeSourcePosition}=await import("data:text/javascript;base64,"+Buffer.from(bundled.outputFiles[0].contents).toString("base64"));
const section = chunks => {
    const nodes=chunks.map(textContent=>({textContent,parentElement:{closest:()=>null}}));let index=0;
    return {document:{documentElement:{},createTreeWalker:()=>{index=0;return {nextNode:()=>nodes[index++]};},createRange:()=>({setStart(node,offset){this.start=[nodes.indexOf(node),offset];},setEnd(node,offset){this.end=[nodes.indexOf(node),offset];}})},cfiFromRange:r=>JSON.stringify([r.start,r.end])};
};
assert.equal(findText(section(["reader-","control needle"]),"reader-control").hits.length,1);
assert.equal(findText(section(["a.* [x] "]),".*").hits[0].cfi,'[[0,1],[0,3]]');
assert.equal(findText(section(["İstanbul foo FOO foo"]),"foo",2).hits.length,2);
assert.equal(findText(section(["나무 골목"]),"골목").hits[0].cfi,'[[0,3],[0,5]]');
assert.throws(()=>findText(section(["text"]),""));
assert.ok(validCfi("epubcfi(/6/2!/4/2:0)"));assert.ok(!validCfi("https://example.invalid"));
console.log("Cross-node, literal, Unicode, hit-limit and locator checks passed");
assert.ok(validCfi("epubcfi(/6/2!/4/2[세부절]/1:0)"));
assert.ok(!validCfi("epubcfi(/6/2!\n/4/2:0)"));

// Automatic source selection cannot pull a scroll-aligned preview back to an
// offscreen caret. Explicit locate and legacy position-only sync still reveal it.
let selected=0,revealed=[];
const target={scrollIntoView:options=>revealed.push(options)};
const doc={documentElement:{children:[target]},body:{contains:node=>node===target},
    createRange:()=>({selectNodeContents:node=>assert.equal(node,target),collapse:()=>{}}),
    defaultView:{getSelection:()=>({removeAllRanges(){},addRange(){++selected;}})}};
const anchor={path:[0],child:-1,offset:0};
placeSourcePosition(doc,{...anchor,reveal:"none"});assert.equal(selected,1);assert.equal(revealed.length,0);
placeSourcePosition(doc,{...anchor,reveal:"nearest"});assert.equal(revealed.at(-1).block,"nearest");
placeSourcePosition(doc,anchor);assert.equal(revealed.at(-1).block,"center");assert.equal(selected,3);
console.log("Source selection preserves scroll-sync viewport and explicit locate reveals the target");

// Reading mode: once the host enables its menu, a right click reports the selection, link, reading
// position and frame-relative point instead of opening the browser menu. Disabled, it reports nothing.
globalThis.window??={addEventListener(){}};
globalThis.document??={getElementById:()=>({getBoundingClientRect:()=>({left:0,top:0,right:0,bottom:0})})};
let ranges=[],chosen="";
const selection={toString:()=>chosen,get rangeCount(){return ranges.length;},getRangeAt:i=>ranges[i],containsNode:()=>chosen!=="",
    removeAllRanges(){ranges=[];},addRange(range){ranges.push(range);},anchorNode:null,anchorOffset:0};
const handlers={},frame={getBoundingClientRect:()=>({left:300,top:20})};
const page={images:[],baseURI:"epub://app/book/OEBPS/chapter1.xhtml",documentElement:{},body:{},fonts:null,
    addEventListener:(type,handler)=>(handlers[type]??=[]).push(handler),getSelection:()=>selection,
    defaultView:{frameElement:frame,getSelection:()=>selection},caretRangeFromPoint:(x,y)=>({caret:[x,y]})};
const chapter={index:2,cfiFromRange:range=>range.caret?`epubcfi(/6/6!/4/2/1:${range.caret[0]})`:"epubcfi(/6/6!/4/2,/1:0,/1:5)"};
const tools=createReaderTools({publication:()=>({resources:[]}),documents:()=>[page],snapshot:()=>({chapterIndex:2}),restore:async()=>{},
    book:()=>({rendition:{manager:{views:{all:()=>[{document:page,section:chapter}]}}}}),apply:()=>{},navigate:async()=>{},
    scroll:{container:()=>null,chapter:()=>2,layout:()=>({paginated:true,fixed:false})}});
tools.hook(page);
const rightClick=(target,clientX,clientY)=>{let prevented=false;
    for(const handler of handlers.contextmenu)handler({type:"contextmenu",target,clientX,clientY,preventDefault(){prevented=true;},stopImmediatePropagation(){}});return prevented;};
const link={getAttribute:()=>"chapter2.xhtml#note"},text={closest:query=>query==="a[href]"?link:null},plain={closest:()=>null};
assert.ok(!rightClick(plain,10,10));assert.equal(tools.state().contextRequest.serial,0);
tools.setContextMenus(true);
chosen="바람의 도시";ranges=[{selection:true}];
assert.ok(rightClick(text,40,50));
let request=tools.state().contextRequest;
assert.deepEqual({serial:request.serial,x:request.x,y:request.y,selection:request.selection,link:request.link,locator:request.locator,chapter:request.chapter,kind:request.target.kind},
    {serial:1,x:340,y:70,selection:"바람의 도시",link:"epub://app/book/OEBPS/chapter2.xhtml#note",locator:"epubcfi(/6/6!/4/2,/1:0,/1:5)",chapter:2,kind:"text"});
// Without a selection the caret moves to the click, so the request names that point.
chosen="";ranges=[];
assert.ok(rightClick(plain,12,8));request=tools.state().contextRequest;
assert.equal(request.serial,2);assert.equal(request.selection,"");assert.equal(request.link,"");assert.equal(request.locator,"epubcfi(/6/6!/4/2/1:12)");assert.equal(request.chapter,2);
await tools.action({action:"context-menu",value:false,serial:9});assert.ok(!rightClick(plain,1,1));assert.equal(tools.state().contextRequest.serial,2);
console.log("Read-mode context requests carry selection, link, locator, chapter and frame offset");
