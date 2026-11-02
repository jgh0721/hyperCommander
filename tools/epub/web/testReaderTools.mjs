import assert from "node:assert/strict";
import {build} from "esbuild";
import {fileURLToPath} from "node:url";
// Match the product bundler: EPUB.js upstream imports omit .js extensions.
const bundled=await build({stdin:{contents:'export {findText,validCfi} from "./readerTools.js"; export {placeSourcePosition} from "./sourcePositions.js";',resolveDir:fileURLToPath(new URL(".",import.meta.url))},bundle:true,platform:"node",format:"esm",write:false});
const {findText,validCfi,placeSourcePosition}=await import("data:text/javascript;base64,"+Buffer.from(bundled.outputFiles[0].contents).toString("base64"));
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
