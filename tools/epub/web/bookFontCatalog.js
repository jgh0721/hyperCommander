import { resolveFontSources } from "./fontSources.js";

const encodePath = path => path.split("/").map(encodeURIComponent).join("/");
const fontTypes = new Set(["font/ttf", "font/otf", "font/woff", "font/woff2"]);
const installed = new WeakMap();

export async function buildBookFontCatalog(publication, root) {
    const allowed = new Set(publication.resources.filter(r => fontTypes.has(r.mediaType))
        .map(r => new URL(encodePath(r.path), root).href));
    // Without manifest-backed font URLs, scanning chapters cannot add a font.
    if (!allowed.size) return { records: [], families: [], warnings: [] };
    const records = [], warnings = [];
    const warn = reason => { if (!warnings.includes(reason)) warnings.push(reason); };
    let bytes = 0, count = 0, rules = 0;
    function* declarations(list, conditions = []) {
        if (conditions.length > 16) { warn("condition-depth-budget"); return; }
        for (const rule of list) {
            if (++rules > 8192 || records.length >= 128) { warn("rule-budget"); return; }
            if (rule.type === CSSRule.MEDIA_RULE || rule.type === CSSRule.SUPPORTS_RULE) {
                yield* declarations(rule.cssRules, [...conditions, {
                    type: rule.type === CSSRule.MEDIA_RULE ? "media" : "supports", text: rule.conditionText }]);
            } else if (rule.type === CSSRule.FONT_FACE_RULE) yield { rule, conditions };
        }
    }
    for (const resource of publication.resources) {
        if (!["text/css", "application/xhtml+xml"].includes(resource.mediaType)) continue;
        if (++count > 512 || (bytes += resource.bytes) > 4 * 1024 * 1024) { warn("scan-budget"); break; }
        let text;
        try { const response = await fetch(new URL(encodePath(resource.path), root), { signal: AbortSignal.timeout(5000) });
            if (!response.ok) throw Error("Resource unavailable");
            const data = await response.arrayBuffer();
            if (data.byteLength !== resource.bytes) throw Error("Resource length mismatch");
            text = new TextDecoder("utf-8", { fatal: true }).decode(data); }
        catch { warn("unsupported-encoding"); continue; }
        let sources = [text];
        if (resource.mediaType === "application/xhtml+xml") {
            const doc = new DOMParser().parseFromString(text, "application/xhtml+xml");
            sources = [...doc.querySelectorAll("style")].map(node => node.textContent);
        }
        for (const source of sources) {
            const sheet = new CSSStyleSheet();
            sheet.replaceSync(source); // Detached sheet: never loads imports or applies publisher CSS.
            for (const { rule, conditions } of declarations(sheet.cssRules)) {
                const style = rule.style;
                const rawFamily = style.getPropertyValue("font-family").trim();
                let family = rawFamily;
                if (/^["']/.test(family) && family.at(-1) === family[0]) family = family.slice(1, -1);
                if (!family || family.length > 128 || /[\\\x00-\x1f\x7f]/.test(family)) { warn("unsupported-family"); continue; }
                const resolvedSource = resolveFontSources(style.getPropertyValue("src").trim(),
                    new URL(encodePath(resource.path), root), allowed, warn);
                if (!resolvedSource) continue;
                const descriptors = {};
                for (const [css, js] of [["font-weight", "weight"], ["font-style", "style"], ["font-stretch", "stretch"],
                    ["unicode-range", "unicodeRange"], ["font-display", "display"]]) {
                    const value = style.getPropertyValue(css); if (value) descriptors[js] = value;
                }
                const record = { family, cssFamily: rawFamily, source: resolvedSource, descriptors, conditions };
                if (!records.some(r => JSON.stringify(r) === JSON.stringify(record))) records.push(record);
            }
        }
        if (rules > 8192 || records.length >= 128) break;
        if (count % 16 === 0) await new Promise(resolve => setTimeout(resolve, 0));
    }
    return { records, families: [...new Set(records.map(r => r.family))].sort(), warnings };
}

function active(record, view) {
    return record.conditions.every(condition => condition.type === "media"
        ? view.matchMedia(condition.text).matches : view.CSS.supports(condition.text));
}

export function activeBookFonts(catalog, view) {
    return [...new Set(catalog.records.filter(record => active(record, view)).map(record => record.family))].sort();
}

export function applyBookFont(doc, catalog, profile) {
    let state = installed.get(doc);
    if (!state) {
        state = { key: "", faces: [], profile, queries: [] };
        installed.set(doc, state);
        const media = new Set(catalog.records.flatMap(record => record.conditions)
            .filter(condition => condition.type === "media").map(condition => condition.text));
        for (const text of media) {
            const query = doc.defaultView.matchMedia(text);
            query.addEventListener("change", () => applyBookFont(doc, catalog, state.profile));
            state.queries.push(query);
        }
    }
    state.profile = profile;
    const family = profile.override ? profile.font : "";
    const selected = catalog.records.filter(record => record.family === family && active(record, doc.defaultView));
    const key = JSON.stringify([family, selected]);
    if (state.key === key) return;
    for (const face of state.faces) doc.fonts.delete(face);
    state.faces = []; state.key = key;
    for (const record of selected) {
        const face = new doc.defaultView.FontFace(record.cssFamily, record.source, record.descriptors);
        doc.fonts.add(face); state.faces.push(face);
    }
}
