// Display-only CSSOM overrides. Package bytes are never rewritten.
import { applyCustomCss } from "./customCss.js";
const states = new WeakMap();
const textSelector = "p, li, blockquote, td, dd";
export function releaseReadingOverrides(doc) {
    states.get(doc)?.clear();states.delete(doc);
}
export function applyReadingOverrides(doc, profile, foreground, background, custom = []) {
    let state = states.get(doc);
    if (!state) { state = new Map(); states.set(doc, state); }
    for (const [node, record] of state) {
        if (node.getAttribute("style") === record.applied) {
            if (record.original === null) node.removeAttribute("style"); else node.setAttribute("style", record.original);
        } else {
            // Preserve layout changes made by the engine between profiles.
            for (const property of record.owned) {
                const value = record.base.getPropertyValue(property);
                if (value) node.style.setProperty(property, value, record.base.getPropertyPriority(property)); else node.style.removeProperty(property);
            }
            if (!node.style.cssText && record.original === null) node.removeAttribute("style");
            record.original = node.getAttribute("style");
        }
        record.owned.clear(); record.base.cssText = record.original || "";
    }
    if (!profile.override) return "";
    const remember = (node, property) => {
        if (!state.has(node)) {
            const original = node.getAttribute("style"); const base = doc.createElementNS("http://www.w3.org/1999/xhtml", "span").style;
            base.cssText = original || ""; state.set(node, { original, base, owned: new Set(), applied: original });
        }
        state.get(node).owned.add(property);
    };
    const set = (node, name, value) => { remember(node, name); node.style.removeProperty(name); node.style.setProperty(name, value, "important"); };
    const nodes = [...doc.body.querySelectorAll("*")].filter(node => node.namespaceURI === "http://www.w3.org/1999/xhtml"
        && !node.closest("rt, rp, svg, math, img, video, audio, script, style") && (node.matches(textSelector) || node.closest(textSelector)));
    if (nodes.length > 32768) throw Error("Reading override element budget exceeded");
    for (const node of [doc.documentElement, doc.body]) set(node, "background-color", profile.background && profile.background !== "theme" ? "transparent" : background);
    const values = { "font-family": JSON.stringify(profile.font), "font-size": `${profile.size}px`, "font-weight": String(profile.weight),
            "line-height": String(profile.line), "letter-spacing": `${profile.letter}em`, "word-spacing": `${profile.word}em`, "text-align": profile.align, color: foreground };
    const paragraphValues = { ...values, "margin-block-start": `${profile.before}em`, "margin-block-end": `${profile.after}em`, "text-indent": `${profile.indent}em` };
    // Parse the declarations once, then change each live element once. Retain
    // the same inline !important precedence and exact publisher-style restore.
    const declaration = entries => Object.entries(entries).map(([name, value]) => `${name}:${value} !important;`).join("");
    const textStyle = declaration(values), paragraphStyle = declaration(paragraphValues);
    for (const node of nodes) {
        const paragraph = node.matches("p");
        for (const name of Object.keys(paragraph ? paragraphValues : values)) remember(node, name);
        node.style.cssText += ";" + (paragraph ? paragraphStyle : textStyle);
    }
    const error = applyCustomCss(doc, custom, remember, profile.cssForce !== false);
    for (const [node, record] of state) record.applied = node.getAttribute("style");
    return error || "";
}
