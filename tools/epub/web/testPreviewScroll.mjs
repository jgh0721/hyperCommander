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
// Requests are applied on the next frame.
const apply=async request=>{scroll.apply(request);await sleep(70);};
await apply({chapter:3,line:7,ratio:0.5,end:false,serial:1});
assert.equal(container.scrollTop,100);let state=scroll.state();assert.equal(state.applied,1);assert.equal(state.appliedLine,7);assert.ok(state.supported);assert.equal(state.anchors,5);
await apply({chapter:3,line:1,ratio:0.5,end:false,serial:2});assert.equal(container.scrollTop,0);
await apply({chapter:3,line:9,ratio:0.5,end:true,serial:3});assert.equal(container.scrollTop,600);
await apply({chapter:4,line:9,ratio:0.5,end:false,serial:4});assert.equal(container.scrollTop,600);assert.equal(scroll.state().error,"chapter");
await apply({chapter:3,line:9,ratio:0.25,end:false,serial:5});assert.equal(container.scrollTop,250);assert.equal(scroll.state().error,undefined);
// A screen offset from the window top replaces the ratio; an anchor that does not resolve keeps the line.
await apply({chapter:3,line:9,ratio:0.25,viewOffset:20,end:false,serial:6});assert.equal(container.scrollTop,280);
await apply({chapter:3,line:9,ratio:0.25,viewOffset:20,anchor:{path:[7,9],child:0,offset:0},end:false,serial:7});assert.equal(container.scrollTop,280);assert.equal(scroll.state().applied,7);
// Requests faster than frames are merged: only the newest is applied.
scroll.apply({chapter:3,line:7,ratio:0.5,end:false,serial:8});scroll.apply({chapter:3,line:9,ratio:0.25,end:false,serial:9});await sleep(70);
assert.equal(container.scrollTop,250);assert.equal(scroll.state().applied,9);
// A layout change re-applies the last request without reporting a new one.
elements[4].getBoundingClientRect=()=>({top:340,height:40});scroll.test.invalidate();
scroll.test.relayout();await sleep(70);assert.equal(container.scrollTop,290);assert.equal(scroll.state().applied,9);
elements[4].getBoundingClientRect=()=>({top:300,height:40});scroll.test.invalidate();await apply({chapter:3,line:9,ratio:0.25,end:false,serial:10});

// Our own scroll is not reported. A later scroll with no input is a scrollbar drag or press and
// counts as the user's; it also releases the editor's anchor. A scroll right after a layout change is
// the page settling; input names its kind.
onScroll();await sleep(80);assert.equal(scroll.state().serial,0);
await sleep(300);container.scrollTop=300;onScroll();await sleep(80);state=scroll.state();
assert.equal(state.serial,1);assert.equal(state.chapter,3);assert.equal(state.line,lineOf(400));assert.equal(state.user,true);assert.equal(state.intent,'scrollbar');assert.equal(state.top,false);assert.equal(state.bottom,false);
scroll.test.relayout();container.scrollTop=320;onScroll();await sleep(80);state=scroll.state();assert.equal(state.serial,2);assert.equal(state.user,false);assert.equal(container.scrollTop,320);
scroll.markIntent('wheel');container.scrollTop=600;onScroll();await sleep(80);state=scroll.state();assert.equal(state.serial,3);assert.equal(state.user,true);assert.equal(state.intent,'wheel');assert.equal(state.bottom,true);
// The reader's own moves for the host (a revealed source position) are not reported, and a later
// layout change does not undo them by re-applying the editor's placement.
await apply({chapter:3,line:9,ratio:0.25,end:false,serial:11});await sleep(300);assert.equal(container.scrollTop,250);
scroll.quiet();container.scrollTop=420;onScroll();await sleep(80);assert.equal(scroll.state().serial,3);
scroll.test.relayout();await sleep(70);assert.equal(container.scrollTop,420);
await sleep(300);container.scrollTop=430;onScroll();await sleep(80);state=scroll.state();assert.equal(state.serial,4);assert.equal(state.intent,'scrollbar');
function lineOf(y){return scroll.test.lineForY(y);}

// Paginated, fixed-layout and vertical chapters have no table, so the host keeps position sync only.
layout.paginated=true;scroll.invalidate();assert.equal(scroll.state().supported,false);assert.equal(scroll.state().anchors,0);
layout.paginated=false;writingMode="vertical-rl";scroll.invalidate();assert.equal(scroll.state().supported,false);
writingMode="horizontal-tb";scroll.invalidate();assert.ok(scroll.state().supported);
console.log("Preview scroll table, interpolation, apply, report and support checks passed");
