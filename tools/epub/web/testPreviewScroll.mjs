import assert from "node:assert/strict";
import {build} from "esbuild";
import {fileURLToPath} from "node:url";
const bundled=await build({stdin:{contents:'export {createPreviewScroll} from "./previewScroll.js";',resolveDir:fileURLToPath(new URL(".",import.meta.url))},bundle:true,platform:"node",format:"esm",write:false});
const {createPreviewScroll}=await import("data:text/javascript;base64,"+Buffer.from(bundled.outputFiles[0].contents).toString("base64"));
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));

// A chapter frame inside a scrolling container: element rects are frame-relative, the frame moves with scrollTop.
let onScroll=null;
const container={scrollTop:0,clientHeight:200,scrollHeight:800,getBoundingClientRect:()=>({top:0}),addEventListener:(type,handler)=>{if(type==="scroll")onScroll=handler;},scrollTo({top}){this.scrollTop=top;}};
const element=(line,y,height,end)=>({getAttribute:name=>name==="data-hc-l"?String(line):name==="data-hc-e"&&end?String(end):null,getBoundingClientRect:()=>({top:y,height})});
const elements=[element(2,0,50),element(5,100,80),element(5,100,20),element(6,100,20),element(9,300,40,10),element(12,500,50),element(14,0,0)];
let writingMode="horizontal-tb";
const doc={defaultView:{frameElement:{getBoundingClientRect:()=>({top:-container.scrollTop})},getComputedStyle:()=>({writingMode})},documentElement:{},body:{querySelectorAll:()=>elements},addEventListener(){},fonts:null};
const layout={paginated:false,fixed:false};
const scroll=createPreviewScroll({documents:()=>[doc],container:()=>container,chapter:()=>3,layout:()=>layout});
scroll.hook(doc);

// Smallest element per start line, y and line rising together, a tail at the end of the last element.
const table=scroll.test.anchorTable();
assert.deepEqual(table.map(point=>point.line),[2,5,9,12,13]);
assert.deepEqual(table.map(point=>point.y),[0,100,300,500,550]);
assert.equal(scroll.test.yForLine(7),200);assert.equal(scroll.test.lineForY(200),7);
for(const line of [2.5,6.25,10.5,12.9])assert.ok(Math.abs(scroll.test.lineForY(scroll.test.yForLine(line))-line)<1e-9);

// The requested line lands at the requested height ratio; line 1 and the end are exact.
scroll.apply({chapter:3,line:7,ratio:0.5,end:false,serial:1});
assert.equal(container.scrollTop,100);let state=scroll.state();assert.equal(state.applied,1);assert.equal(state.appliedLine,7);assert.ok(state.supported);assert.equal(state.anchors,5);
scroll.apply({chapter:3,line:1,ratio:0.5,end:false,serial:2});assert.equal(container.scrollTop,0);
scroll.apply({chapter:3,line:9,ratio:0.5,end:true,serial:3});assert.equal(container.scrollTop,600);
scroll.apply({chapter:4,line:9,ratio:0.5,end:false,serial:4});assert.equal(container.scrollTop,600);assert.equal(scroll.state().error,"chapter");
scroll.apply({chapter:3,line:9,ratio:0.25,end:false,serial:5});assert.equal(container.scrollTop,250);assert.equal(scroll.state().error,undefined);

// Our own scroll is not reported; a later scroll is, and only input makes it the user's.
onScroll();await sleep(80);assert.equal(scroll.state().serial,0);
await sleep(200);container.scrollTop=300;onScroll();await sleep(80);state=scroll.state();
assert.equal(state.serial,1);assert.equal(state.chapter,3);assert.equal(state.line,lineOf(400));assert.equal(state.user,false);assert.equal(state.top,false);assert.equal(state.bottom,false);
scroll.markIntent();container.scrollTop=600;onScroll();await sleep(80);state=scroll.state();assert.equal(state.serial,2);assert.equal(state.user,true);assert.equal(state.bottom,true);
function lineOf(y){return scroll.test.lineForY(y);}

// Paginated, fixed-layout and vertical chapters have no table, so the host keeps position sync only.
layout.paginated=true;scroll.invalidate();assert.equal(scroll.state().supported,false);assert.equal(scroll.state().anchors,0);
layout.paginated=false;writingMode="vertical-rl";scroll.invalidate();assert.equal(scroll.state().supported,false);
writingMode="horizontal-tb";scroll.invalidate();assert.ok(scroll.state().supported);
console.log("Preview scroll table, interpolation, apply, report and support checks passed");
