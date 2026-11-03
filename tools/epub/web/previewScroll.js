// Scroll synchronisation between the EPUB source editor and its preview.
//
// Editing previews receive chapters whose elements carry data-hc-l (start source line) and, for
// elements spanning lines, data-hc-e (end line); the render process adds them while serving the
// chapter. A request names the editor's anchor character by its source-map position (element
// path, text node, offset) and the screen height of its row; the character's rendered line goes
// to that height. Without a character the line shown at a height ratio of the editor is kept at
// the same ratio here, as in MultiRoot-reST's mrr_preview.js: measured element positions form a
// table and lines between two measured points are interpolated.
import { sourcePositionForNode } from "./sourcePositions.js";

const FEEDBACK_GUARD_MS = 250;     // our own scroll after a host request is not reported back
const USER_INTENT_TAIL_MS = 400;   // only a scroll this soon after input counts as the user's
const LAYOUT_SETTLE_MS = 250;      // a scroll this soon after a layout change is the page settling
const REPORT_DELAY_MS = 60;
const ANCHOR_RATIO = 0.5;
const INPUT_EVENTS = {wheel: "wheel", keydown: "key", mousedown: "pointer", pointerdown: "pointer", pointermove: "pointer", touchstart: "touch", touchmove: "touch"};

// documents(): chapter documents in view. container(): the scrolling element. chapter(): spine index.
// layout(): {paginated, fixed}. Positions are in the container's scroll coordinates.
export function createPreviewScroll({ documents, container, chapter, layout }) {
    let table = null, measured = "", measureMs = 0, suppressUntil = 0, userIntentUntil = 0, intentKind = "", layoutAt = 0;
    let timer = 0, serial = 0, attached = null, pinned = null, pending = null, queued = false;
    let report = { serial: 0, chapter: -1, line: 0, ratio: ANCHOR_RATIO, user: false, top: true, bottom: false, applied: 0, appliedLine: 0 };
    const now = () => performance.now();
    const invalidate = () => { table = null; };
    const markIntent = (kind = "pointer") => { userIntentUntil = now() + USER_INTENT_TAIL_MS; intentKind = kind; pinned = null; };
    // The reader's own moves for the host (a revealed source position, a restored location, another
    // chapter) are not the user's scroll, and they replace the placement the editor asked for.
    const quiet = () => { suppressUntil = Math.max(suppressUntil, now() + FEEDBACK_GUARD_MS); pinned = null; };
    // A page keeps scrolling by itself while images, fonts and layout settle. Those scrolls must not
    // move the editor, and only the window that receives input can tell them apart from the user's.
    const listenForInput = target => Object.keys(INPUT_EVENTS).forEach(type => target.addEventListener(type, event => {
        if (type === "pointermove" && !event.buttons) return;
        markIntent(INPUT_EVENTS[type]);
    }, { passive: true, capture: true }));
    // Late images, fonts and width changes move the layout under an applied anchor: apply it again,
    // unless the user has placed the preview since.
    const relayout = () => {
        invalidate(); layoutAt = now();
        if (pinned && now() >= userIntentUntil) queue({ ...pinned, reapply: true });
    };
    if (typeof window !== "undefined") { listenForInput(window); window.addEventListener("resize", relayout); }

    function supported() {
        const { paginated, fixed } = layout();
        const doc = documents()[0];
        if (paginated || fixed || !doc) return false;
        return !String(doc.defaultView.getComputedStyle(doc.documentElement).writingMode || "").startsWith("vertical");
    }
    function anchorTable() {
        const host = container();
        // Late images, fonts and reflows change the chapter's height or width; either makes the table stale.
        // No ResizeObserver: its loop notice is a page error, and the reader treats page errors as fatal.
        const size = host ? `${host.scrollHeight}x${host.clientWidth}` : "";
        if (table && size === measured) return table;
        const started = now(), points = new Map();measured = size;
        if (host && supported()) {
            const offset = host.scrollTop - host.getBoundingClientRect().top;
            for (const doc of documents()) {
                const frame = doc.defaultView.frameElement, frameTop = frame ? frame.getBoundingClientRect().top : 0;
                for (const element of doc.body.querySelectorAll("[data-hc-l]")) {
                    const line = Number(element.getAttribute("data-hc-l"));
                    const rect = element.getBoundingClientRect();
                    if (!(line > 0) || rect.height <= 0) continue;
                    const point = { line, end: Math.max(line, Number(element.getAttribute("data-hc-e")) || line), y: rect.top + frameTop + offset, height: rect.height };
                    // Several elements can start on one line; the smallest locates it best.
                    const existing = points.get(line);
                    if (!existing || point.height < existing.height) points.set(line, point);
                }
            }
        }
        // Interpolation needs y and line to rise together. Table cells of one row share a y; keeping
        // the first spreads the row's lines over the gap to the next point instead of jumping.
        const sorted = [...points.values()].sort((a, b) => a.y - b.y || a.line - b.line), result = [];
        for (const point of sorted) {
            const last = result[result.length - 1];
            if (!last || (point.line > last.line && point.y > last.y)) result.push(point);
        }
        // The end of the last element bounds interpolation at the end of the chapter.
        if (result.length) { const tail = result[result.length - 1]; result.push({ line: Math.max(tail.line + 1, tail.end + 1), end: tail.end + 1, y: tail.y + tail.height, height: 0 }); }
        measureMs = now() - started; table = result; return table;
    }
    // Both directions use the same table, so a line maps to a y and back to the same line.
    function interpolate(key, value, other) {
        const points = anchorTable();
        if (!points.length) return null;
        if (value <= points[0][key]) return points[0][other];
        let low = 0, high = points.length - 1;
        if (value >= points[high][key]) return points[high][other];
        while (high - low > 1) { const middle = (low + high) >> 1; if (points[middle][key] <= value) low = middle; else high = middle; }
        const a = points[low], b = points[high];
        return a[other] + (b[other] - a[other]) * (value - a[key]) / Math.max(1e-6, b[key] - a[key]);
    }
    const yForLine = line => interpolate("line", line, "y");
    const lineForY = y => interpolate("y", y, "line");
    const centerLine = host => lineForY(host.scrollTop + host.clientHeight * ANCHOR_RATIO);

    // The rendered box of the character a source-map position names, in main viewport coordinates.
    // Collapsed white space has no box; the next character on the line stands for it.
    function anchorBox(anchor) {
        const doc = documents()[0];
        if (!doc || !anchor || !Array.isArray(anchor.path) || anchor.path.length > 128) return null;
        let element = doc.documentElement;
        for (const index of anchor.path) { element = Number.isInteger(index) ? element?.children?.[index] : null; if (!element) return null; }
        if (element !== doc.body && !doc.body.contains(element)) return null;
        const frame = doc.defaultView.frameElement, frameTop = frame ? frame.getBoundingClientRect().top : 0;
        const node = Number.isInteger(anchor.child) && anchor.child >= 0 ? element.childNodes[anchor.child] : null;
        if (node && (node.nodeType === 3 || node.nodeType === 4)) {
            const text = node.textContent, range = doc.createRange();
            for (let at = Math.min(Math.max(0, anchor.offset | 0), Math.max(0, text.length - 1)), steps = 0; at < text.length && steps < 64; ++at, ++steps) {
                const code = text.charCodeAt(at), end = Math.min(text.length, at + (code >= 0xd800 && code < 0xdc00 ? 2 : 1));
                range.setStart(node, at); range.setEnd(node, end);
                const box = [...range.getClientRects()].find(r => r.height > 0 && r.width > 0);
                if (box) return { top: box.top + frameTop, bottom: box.bottom + frameTop };
            }
        }
        const box = element.getBoundingClientRect();
        return box.height > 0 ? { top: box.top + frameTop, bottom: box.bottom + frameTop } : null;
    }
    // The character at the preview's reference height: its source-map position and the
    // main-viewport top of its rendered line.
    function referencePoint(host) {
        const hostBox = host.getBoundingClientRect(), y = hostBox.top + host.clientHeight * ANCHOR_RATIO, x = hostBox.left + host.clientWidth * .5;
        for (const doc of documents()) {
            const frame = doc.defaultView.frameElement, box = frame ? frame.getBoundingClientRect() : { left: 0, top: 0, bottom: Infinity };
            if (y < box.top || y > box.bottom) continue;
            let node = null, offset = 0;
            const position = doc.caretPositionFromPoint?.(x - box.left, y - box.top);
            if (position) { node = position.offsetNode; offset = position.offset; }
            else { const range = doc.caretRangeFromPoint?.(x - box.left, y - box.top); node = range?.startContainer; offset = range?.startOffset || 0; }
            if (!node || (node.nodeType !== 3 && node.nodeType !== 4) || !doc.body.contains(node)) return null;
            const anchor = sourcePositionForNode(doc, node, Math.min(offset, Math.max(0, node.textContent.length - 1)));
            const glyph = anchor && anchorBox(anchor);
            return anchor && glyph ? { anchor, viewOffset: glyph.top } : null;
        }
        return null;
    }
    // What moved the page: "" for this script or settling layout, otherwise the user's input.
    // Dragging or pressing the scrollbar delivers no page events; a scroll that no request,
    // input or recent layout change explains is one of those.
    function scrollCause() {
        const at = now();
        if (at < suppressUntil) return "";
        if (at < userIntentUntil) return intentKind;
        if (at - layoutAt <= LAYOUT_SETTLE_MS) return "";
        pinned = null;
        return "scrollbar";
    }
    function scrolled() {
        if (now() < suppressUntil) return;
        clearTimeout(timer);
        timer = setTimeout(() => {
            const host = container();
            if (!host || now() < suppressUntil) return;
            const max = Math.max(0, host.scrollHeight - host.clientHeight), line = centerLine(host), cause = scrollCause();
            report = { ...report, serial: ++serial, chapter: chapter(), line: line ?? 0, ratio: ANCHOR_RATIO, user: cause !== "", intent: cause,
                top: host.scrollTop <= 1, bottom: host.scrollTop >= max - 1, reference: referencePoint(host) };
        }, REPORT_DELAY_MS);
    }
    function attach() {
        const host = container();
        if (!host || host === attached) return;
        attached = host; host.addEventListener("scroll", scrolled, { passive: true });
    }
    function applyNow(request) {
        const host = container();
        if (!host || request.chapter !== chapter()) { if (!request.reapply) report = { ...report, applied: request.serial, error: "chapter" }; return; }
        const max = Math.max(0, host.scrollHeight - host.clientHeight);
        let target;
        if (request.line <= 1 && !request.anchor) target = 0;
        else if (request.end) target = max;
        else {
            const glyph = request.anchor && Number.isFinite(request.viewOffset) ? anchorBox(request.anchor) : null;
            if (glyph) target = host.scrollTop + glyph.top - request.viewOffset;
            else {
                const y = yForLine(request.line);
                if (y === null) { if (!request.reapply) report = { ...report, applied: request.serial, error: "unsupported" }; return; }
                // viewOffset is measured from the top of the preview window; the scrolling host may start lower.
                target = y - (Number.isFinite(request.viewOffset) ? request.viewOffset - host.getBoundingClientRect().top : host.clientHeight * request.ratio);
            }
            target = Math.max(0, Math.min(max, target));
        }
        suppressUntil = now() + FEEDBACK_GUARD_MS;
        if (Math.abs(host.scrollTop - target) >= 1) host.scrollTo({ top: target, behavior: "instant" });
        pinned = request;
        if (request.reapply) return;
        const { error, ...rest } = report, glyph = request.anchor ? anchorBox(request.anchor) : null;
        // Where the anchor character's line now sits, for the host to verify.
        report = { ...rest, applied: request.serial, appliedLine: centerLine(host) ?? 0, appliedOffset: glyph ? glyph.top : null, clamped: target <= 0 || target >= max };
    }
    // Requests arriving faster than frames are merged: only the newest is applied, once per frame.
    function queue(request) {
        pending = request; if (queued) return; queued = true;
        const run = () => {
            if (!queued) return; queued = false;
            const next = pending; pending = null;
            if (next) { attach(); applyNow(next); }
        };
        // Chromium can suspend animation callbacks in an occluded preview.
        if (typeof requestAnimationFrame === "function") requestAnimationFrame(run);
        setTimeout(run, 50);
    }
    return {
        hook(doc) {
            listenForInput(doc);
            doc.addEventListener("load", relayout, true);
            doc.fonts?.addEventListener?.("loadingdone", relayout);
            // A newly shown document is a layout change: the page settles under it.
            invalidate(); layoutAt = now(); attach();
        },
        // request: {chapter, line, ratio, end, serial, anchor?, viewOffset?} in the source lines of this preview.
        apply(request) {
            if (!request || !Number.isInteger(request.serial)) return;
            queue(request);
        },
        markIntent, quiet, invalidate,
        state() {
            attach();
            const points = anchorTable();
            return { ...report, supported: points.length > 0, anchors: points.length, measureMs: Math.round(measureMs * 10) / 10 };
        },
        test: { anchorTable, yForLine, lineForY, invalidate, anchorBox, relayout }
    };
}
