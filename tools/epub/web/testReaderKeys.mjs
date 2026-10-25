import assert from "node:assert/strict";
import { pageKeyAction } from "./readerKeys.js";
const bindings = { "epub.next": "PgDown", "epub.previous": "PgUp" };
for (const [key, code, expected] of [["PageDown", "PageDown", "next"], ["PageUp", "PageUp", "previous"], ["PageDown", "Numpad3", "next"], ["PageUp", "Numpad9", "previous"], ["3", "Numpad3", "next"], ["9", "Numpad9", "previous"]]) {
    assert.equal(pageKeyAction({ key, code }, bindings), expected);
    assert.equal(pageKeyAction({ key, code, ctrlKey: true }, bindings), null);
    assert.equal(pageKeyAction({ key, code, isComposing: true }, bindings), null);
    assert.equal(pageKeyAction({ key, code, target: { closest: () => ({}) } }, bindings), null);
}
assert.equal(pageKeyAction({ key: "PageDown" }, { ...bindings, "epub.next": "" }), null);
assert.equal(pageKeyAction({ key: "j", ctrlKey: true }, { ...bindings, "epub.next": "Ctrl+J" }), "next");
assert.equal(pageKeyAction({ key: "PageDown" }, { ...bindings, "epub.next": "Ctrl+J" }), null);
console.log("Page keys, Num Lock variants, input exclusions and custom bindings passed");
