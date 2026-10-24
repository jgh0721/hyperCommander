import { parse, walk, generate, lexer, ident } from "css-tree";
const allowed = new Set(`color background-color font-family font-size font-weight font-style font-stretch font-variant font-variant-caps font-variant-ligatures font-feature-settings font-kerning line-height letter-spacing word-spacing text-align text-indent text-decoration text-decoration-line text-decoration-color text-decoration-style text-transform text-wrap white-space word-break overflow-wrap hyphens vertical-align margin margin-top margin-right margin-bottom margin-left margin-block margin-block-start margin-block-end margin-inline margin-inline-start margin-inline-end padding padding-top padding-right padding-bottom padding-left padding-block padding-inline border border-width border-style border-color border-radius border-top border-bottom border-left border-right max-width min-width width height max-height display list-style-type list-style-position`.split(" "));
const functions = new Set(["calc", "min", "max", "clamp", "rgb", "rgba", "hsl", "hsla", "var", "color-mix"]);
const dynamic = new Set(["has", "hover", "active", "focus", "focus-visible", "focus-within", "visited", "target"]);
const decode = value => ident.decode(value).toLowerCase();
export class CssError extends Error {
    constructor(message, node) { super(message); this.line = node?.loc?.start?.line || node?.line || 1; this.column = node?.loc?.start?.column || node?.column || 1; }
}
const compare = (a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2];
function specificity(node) {
    if (!node) return [0, 0, 0];
    if (node.type === "IdSelector") return [1, 0, 0];
    if (node.type === "ClassSelector" || node.type === "AttributeSelector") return [0, 1, 0];
    if (node.type === "TypeSelector") return [0, 0, node.name === "*" ? 0 : 1];
    const children = node.children?.toArray?.() || [];
    if (node.type === "PseudoClassSelector") {
        const name = decode(node.name);
        if (name === "where") return [0, 0, 0];
        if (name === "is" || name === "not") return children.map(specificity).sort(compare).at(-1) || [0, 0, 0];
        return [0, 1, 0];
    }
    if (node.type === "SelectorList") return children.map(specificity).sort(compare).at(-1) || [0, 0, 0];
    return children.map(specificity).reduce((a, b) => a.map((v, i) => v + b[i]), [0, 0, 0]);
}
export function compileCss(source = "") {
    if (typeof source !== "string" || source.length > 16384 || new TextEncoder().encode(source).length > 32768) throw new CssError("CSS size limit exceeded");
    let ast;
    try { ast = parse(source, { positions: true, onParseError(error) { throw error; } }); }
    catch (error) { throw new CssError(error.message, error); }
    let count = 0;
    walk(ast, node => {
        if (++count > 8192) throw new CssError("CSS complexity limit exceeded", node);
        if (node.type === "Raw" || node.type === "Url") throw new CssError("External resources or unsupported syntax", node);
        if (node.type === "PseudoElementSelector" || (node.type === "PseudoClassSelector" && dynamic.has(decode(node.name)))) throw new CssError("Unsupported dynamic selector", node);
        if (node.type === "Function" && !functions.has(decode(node.name))) throw new CssError("Unsupported CSS function", node);
    });
    const rules = [];
    function collect(nodes, conditions = []) {
        for (const node of nodes) {
            if (node.type === "Atrule") {
                const name = decode(node.name);
                if (!node.block || !["media", "supports"].includes(name) || conditions.length >= 4) throw new CssError("Unsupported @" + name, node);
                collect(node.block.children, [...conditions, { type: name, query: generate(node.prelude) }]); continue;
            }
            if (node.type !== "Rule" || node.prelude.type !== "SelectorList") throw new CssError("Expected a selector rule", node);
            const declarations = [];
            for (const declaration of node.block.children) {
                if (declaration.type !== "Declaration") throw new CssError("Expected a declaration", declaration);
                const property = decode(declaration.property);
                if (!allowed.has(property)) throw new CssError("Unsupported property: " + property, declaration);
                let variable = false;
                walk(declaration.value, item => { if (item.type === "Function" && decode(item.name) === "var") variable = true; });
                if (!variable && lexer.matchProperty(property, declaration.value).error) throw new CssError("Invalid value for " + property, declaration);
                declarations.push({ property, value: generate(declaration.value), important: !!declaration.important });
            }
            for (const selector of node.prelude.children) {
                if (rules.length >= 256) throw new CssError("Too many CSS selectors", node);
                rules.push({ selector: generate(selector), specificity: specificity(selector), declarations, conditions, order: rules.length });
            }
        }
    }
    collect(ast.children);
    return rules;
}
export function applyCustomCss(doc, rules, remember, force = true) {
    const selected = new Map(); let matched = 0;
    const textNodes = [...doc.body.querySelectorAll("*")].filter(node => node.namespaceURI === "http://www.w3.org/1999/xhtml"
        && !node.closest("rt,rp,svg,math,img,video,audio,script,style") && (node.matches("p,li,blockquote,td,dd") || node.closest("p,li,blockquote,td,dd")));
    const inherited = name => name.startsWith("font-") || ["color","line-height","letter-spacing","word-spacing","text-align","text-indent","white-space","word-break","overflow-wrap","hyphens"].includes(name);
    for (const rule of rules) {
        if (!rule.conditions.every(c => c.type === "media" ? doc.defaultView.matchMedia(c.query).matches : doc.defaultView.CSS.supports(c.query))) continue;
        for (const node of doc.querySelectorAll(rule.selector)) {
            if (node.namespaceURI !== "http://www.w3.org/1999/xhtml" || node.closest("script,style,svg,math,iframe,object,embed,link,meta")) continue;
            for (const declaration of rule.declarations) {
                const targets = (node === doc.body || node === doc.documentElement) && inherited(declaration.property) ? [node,...textNodes] : [node];
                for (const target of targets) {
                    if (++matched > 65536) return "CSS match budget exceeded";
                    if (!selected.has(target)) selected.set(target,new Map());
                    const values=selected.get(target);
                    const next={...declaration,important:force || declaration.important,specificity:rule.specificity,order:rule.order};
                    const current=values.get(next.property);
                    if(!current || Number(next.important)>Number(current.important)
                        || (next.important===current.important && (compare(next.specificity,current.specificity)>0
                            || (compare(next.specificity,current.specificity)===0 && next.order>=current.order)))) values.set(next.property,next);
                }
            }
        }
    }
    for(const [node,values] of selected) for(const [property,value] of values) {
        if(!value.important && node.style.getPropertyPriority(property)==="important")continue;
        remember(node,property);node.style.removeProperty(property);node.style.setProperty(property,value.value,value.important?"important":"");
    }
    return "";
}
