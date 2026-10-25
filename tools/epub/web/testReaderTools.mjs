import assert from "node:assert/strict";
import {findText,validCfi} from "./readerTools.js";
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
