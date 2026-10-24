import assert from "node:assert/strict";
import { compileCss } from "./customCss.js";
const rules = compileCss("p, #chapter .note { line-height: 2; margin-block-start: .8em !important; color: #345; }");
assert.equal(rules.length, 2); assert.deepEqual(rules[1].specificity, [1,1,0]);
assert.equal(rules[0].declarations[1].important, true);
assert.equal(compileCss("@media (min-width: 600px) { p { text-align:justify } }")[0].conditions[0].type, "media");
assert.deepEqual(compileCss(":where(.a) p {color:red}")[0].specificity,[0,0,1]);
assert.deepEqual(compileCss(":is(#a,.b) p {color:red}")[0].specificity,[1,0,1]);
for (const source of ["@import 'https://x';", "@font-face {font-family:x;src:url(file:///x)}", "p{background-color:url(https://x)}",
    "p{color:expression(alert(1))}", "p{position:fixed}", "p{--asset:url(x)}", "p{color:garbage}", "p{color:red;invalid}",
    "body:has(p){color:red}", "a:hover{color:red}", "p::before{color:red}", "x".repeat(16385)]) assert.throws(()=>compileCss(source), undefined, source);
try { compileCss("p {\n bad-property: 2;\n}"); assert.fail(); } catch(error) { assert.equal(error.line,2); assert.ok(error.column>0); }
console.log("Custom CSS AST, specificity, locations and blocked-resource cases passed");
