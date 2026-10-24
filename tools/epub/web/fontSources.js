// Parse the browser's serialized src descriptor, not arbitrary stylesheets.
// Preserve safe fallback order; never resolve local() or unregistered URLs.
export function resolveFontSources(text, base, allowed, warn = () => {}) {
    if (text.length > 16384) { warn("source-budget"); return null; }
    const parts = [];
    let start = 0, depth = 0, quote = "", escaped = false;
    for (let i = 0; i < text.length; ++i) {
        const c = text[i];
        if (escaped) { escaped = false; continue; }
        if (c === "\\") { escaped = true; continue; }
        if (quote) { if (c === quote) quote = ""; continue; }
        if (c === '"' || c === "'") quote = c;
        else if (c === "(") ++depth;
        else if (c === ")") { if (--depth < 0) return null; }
        else if (c === "," && depth === 0) { parts.push(text.slice(start, i).trim()); start = i + 1; }
    }
    if (quote || depth || escaped) { warn("unsupported-source"); return null; }
    parts.push(text.slice(start).trim());
    if (parts.length > 32) { warn("source-budget"); return null; }
    const result = [];
    for (const part of parts) {
        if (/^local\(/i.test(part)) { warn("local-source-skipped"); continue; }
        const match = /^url\("([^"\\]*)"\)(\s+format\("[^"\\]*"\))?$/.exec(part);
        if (!match) { warn("unsupported-source"); continue; }
        let url;
        try { url = new URL(match[1], base).href; } catch { warn("invalid-source"); continue; }
        if (!allowed.has(url)) { warn("source-outside-font-manifest"); continue; }
        const value = `url(${JSON.stringify(url)})${match[2] || ""}`;
        if (!result.includes(value)) result.push(value);
    }
    return result.length ? result.join(", ") : null;
}
