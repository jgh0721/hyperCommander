import assert from "node:assert/strict";
import { resolveFontSources } from "./fontSources.js";
const base = "epub://app/book/EPUB/Styles/book.css";
const first = "epub://app/book/EPUB/Fonts/first.ttf", second = "epub://app/book/EPUB/Fonts/a,b.woff2";
const allowed = new Set([first, second]);
const resolve = text => resolveFontSources(text, base, allowed);
assert.equal(resolve('local("Installed, Font"), url("../Fonts/first.ttf") format("truetype"), url("../Fonts/a,b.woff2")'),
    `url("${first}") format("truetype"), url("${second}")`);
assert.equal(resolve('url("https://example.invalid/font.ttf"), url("../Fonts/first.ttf")'), `url("${first}")`);
assert.equal(resolve('url("../Fonts/missing.ttf"), url("../Fonts/first.ttf")'), `url("${first}")`);
assert.equal(resolve('local("Installed")'), null);
assert.equal(resolve('url("file:///C:/font.ttf"), url("data:font/ttf;base64,AA==")'), null);
assert.equal(resolve('url("../Fonts/first.ttf?x=1"), url("../Fonts/first.ttf#fragment")'), null);
assert.equal(resolve('url("../Fonts/first.ttf"), url("../Fonts/first.ttf")'), `url("${first}")`);
assert.equal(resolve('url("../Fonts/first.ttf") tech(variations), url("../Fonts/a,b.woff2")'), `url("${second}")`);
for (const text of ['url("broken)', ')url("../Fonts/first.ttf")', 'url("../Fonts/first.ttf"', 'url("x\\']) assert.equal(resolve(text), null);
assert.equal(resolve('x'.repeat(16385)), null);
assert.equal(resolve(Array(33).fill('url("../Fonts/first.ttf")').join(',')), null);
console.log("Font source fallback, URL confinement, quoting and budgets passed");
