// Display-only CSSOM overrides. Never rewrite package resources or engine root layout.
const states = new WeakMap();
const textSelector = "p, li, blockquote, td, dd";

export function applyReadingOverrides(doc, profile, foreground) {
    let state = states.get(doc);
    if (!state) { state = new Map(); states.set(doc, state); }
    // Restore exact publisher attributes before deriving the next display profile.
    for (const [node, original] of state) {
        if (original === null) node.removeAttribute("style");
        else node.setAttribute("style", original);
    }
    // Keep exact publisher attributes for the lifetime of this read-only document.
    if (!profile.override) return;
    const nodes = [...doc.body.querySelectorAll("*")].filter(node =>
        node.namespaceURI === "http://www.w3.org/1999/xhtml"
        && !node.closest("rt, rp, svg, math, img, video, audio, script, style")
        && (node.matches(textSelector) || node.closest(textSelector)));
    if (nodes.length > 32768) throw Error("Reading override element budget exceeded");
    for (const node of nodes) {
        if(!state.has(node)) state.set(node, node.getAttribute("style"));
        const declaration = doc.createElementNS("http://www.w3.org/1999/xhtml", "span").style;
        declaration.cssText = state.get(node) || "";
        const values = { "font-family": JSON.stringify(profile.font), "font-size": `${profile.size}px`,
            "font-weight": String(profile.weight), "line-height": String(profile.line),
            "letter-spacing": `${profile.letter}em`, "word-spacing": `${profile.word}em`,
            "text-align": profile.align, color: foreground };
        if (node.matches("p")) Object.assign(values, {
            "margin-block-start": `${profile.before}em`, "margin-block-end": `${profile.after}em`,
            "text-indent": `${profile.indent}em` });
        for (const [name, value] of Object.entries(values)) {
            // Reinsert logical margins after physical/shorthand declarations in author order.
            declaration.removeProperty(name);
            declaration.setProperty(name, value, "important");
        }
        node.setAttribute("style", declaration.cssText);
    }
}
