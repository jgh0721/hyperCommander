// Scroll synchronisation between the EPUB source editor and its preview.
//
// Editing previews receive chapters whose elements carry data-hc-l (start source line) and, for
// elements spanning lines, data-hc-e (end line); the render process adds them while serving the
// chapter. The line shown at a height ratio of the editor is kept at the same ratio here, as in
// MultiRoot-reST's mrr_preview.js: measured element positions form a table and lines between two
// measured points are interpolated, because source lines and rendered pixels are not proportional.
const FEEDBACK_GUARD_MS = 250;     // our own scroll after a host request is not reported back
const USER_INTENT_TAIL_MS = 400;   // only a scroll this soon after input counts as the user's
const REPORT_DELAY_MS = 60;
const ANCHOR_RATIO = 0.5;
const INPUT_EVENTS = ["wheel", "keydown", "mousedown", "pointerdown", "pointermove", "touchstart", "touchmove"];

// documents(): chapter documents in view. container(): the scrolling element. chapter(): spine index.
// layout(): {paginated, fixed}. Positions are in the container's scroll coordinates.
export function createPreviewScroll({ documents, container, chapter, layout }) {
    let table = null, measured = "", measureMs = 0, suppressUntil = 0, userIntentUntil = 0, timer = 0, serial = 0, attached = null;
    let report = { serial: 0, chapter: -1, line: 0, ratio: ANCHOR_RATIO, user: false, top: true, bottom: false, applied: 0, appliedLine: 0 };
    const now = () => performance.now();
    const invalidate = () => { table = null; };
    const markIntent = () => { userIntentUntil = now() + USER_INTENT_TAIL_MS; };
    // A page keeps scrolling by itself while images, fonts and layout settle. Those scrolls must not
    // move the editor, and only the window that receives input can tell them apart from the user's.
    const listenForInput = target => INPUT_EVENTS.forEach(type => target.addEventListener(type, event => {
        if (type === "pointermove" && !event.buttons) return;
        markIntent();
    }, { passive: true, capture: true }));
    if (typeof window !== "undefined") { listenForInput(window); window.addEventListener("resize", invalidate); }

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

    function scrolled() {
        if (now() < suppressUntil) return;
        clearTimeout(timer);
        timer = setTimeout(() => {
            const host = container();
            if (!host || now() < suppressUntil) return;
            const max = Math.max(0, host.scrollHeight - host.clientHeight), line = centerLine(host);
            report = { ...report, serial: ++serial, chapter: chapter(), line: line ?? 0, ratio: ANCHOR_RATIO, user: now() < userIntentUntil,
                top: host.scrollTop <= 1, bottom: host.scrollTop >= max - 1 };
        }, REPORT_DELAY_MS);
    }
    function attach() {
        const host = container();
        if (!host || host === attached) return;
        attached = host; host.addEventListener("scroll", scrolled, { passive: true });
    }
    return {
        hook(doc) {
            listenForInput(doc);
            doc.addEventListener("load", invalidate, true);
            doc.fonts?.addEventListener?.("loadingdone", invalidate);
            invalidate(); attach();
        },
        // request: {chapter, line, ratio, end, serial} in the source lines of this preview.
        apply(request) {
            const host = container();
            if (!request || !Number.isInteger(request.serial)) return;
            attach();
            if (!host || request.chapter !== chapter()) { report = { ...report, applied: request.serial, error: "chapter" }; return; }
            const max = Math.max(0, host.scrollHeight - host.clientHeight);
            let target;
            if (request.line <= 1) target = 0;
            else if (request.end) target = max;
            else {
                const y = yForLine(request.line);
                if (y === null) { report = { ...report, applied: request.serial, error: "unsupported" }; return; }
                target = Math.max(0, Math.min(max, y - host.clientHeight * request.ratio));
            }
            suppressUntil = now() + FEEDBACK_GUARD_MS;
            if (Math.abs(host.scrollTop - target) >= 1) host.scrollTo({ top: target, behavior: "instant" });
            const { error, ...rest } = report;
            report = { ...rest, applied: request.serial, appliedLine: centerLine(host) ?? 0 };
        },
        markIntent, invalidate,
        state() {
            attach();
            const points = anchorTable();
            return { ...report, supported: points.length > 0, anchors: points.length, measureMs: Math.round(measureMs * 10) / 10 };
        },
        test: { anchorTable, yForLine, lineForY, invalidate }
    };
}
