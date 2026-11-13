// What moved a preview page: the user, the editor or the page itself.
//
// Shared by the three previews that keep a source editor in step: Markdown (tools/markdown/web/renderer.js,
// bundled), reStructuredText (tools/rst/web/mrr_preview.js, injected before it into Sphinx pages) and EPUB
// (previewScroll.js, bundled). It lives here because the EPUB runtime accepts bundle sources only under
// tools/epub/web (qualify_runtime.py). No import or export: the reST preview runs it as a classic script.
// Changing it changes the EPUB runtime (re-pin) and the Markdown bundle (node build.mjs in tools/markdown).
//
// Measured with real input in the VM (2026-11-14) on all three previews, viewport and element scrollbars alike:
//  - pressing the scrollbar, on the thumb or the track, reaches the page as pointer input;
//  - moving a dragged thumb does not: the page scrolls with no pointer or mouse event.
// A slow thumb drag therefore outlives the input that started it. So a scroll is the user's when input came
// just before it, or when nothing else explains it: no request of the editor (suppress) and no layout change
// (late images and fonts, reflow, a resized window; layoutChanged). That covers a thumb drag, autoscroll and
// find in page. A page scrolls by itself only right after its layout changed.
//
// Classify only for a scroll event. A report sent for another reason (a caret move, a finished render)
// describes a page that did not move, and reading it as the user's moved the editor (Markdown 04d22951).
(function (global) {
    "use strict";
    // Only a scroll this soon after input carries the input's kind. Wheel and key repeat keep sending
    // events while they scroll; this covers the gap between the last event and the scroll it causes.
    var INPUT_TAIL_MS = 400;
    // A scroll this soon after a layout change is the page settling: scroll anchoring, a clamped offset.
    var LAYOUT_SETTLE_MS = 250;
    var INPUT_EVENTS = { wheel: "wheel", keydown: "key", mousedown: "pointer", pointerdown: "pointer",
                         pointermove: "pointer", touchstart: "touch", touchmove: "touch" };

    // onInput(kind): the user's input arrived (the preview stops re-applying the editor's last placement).
    function create(options) {
        var onInput = options && options.onInput;
        var clock = typeof performance !== "undefined" && performance.now ? function () { return performance.now(); } : Date.now;
        var inputUntil = 0, inputKind = "", quietUntil = 0, layoutAt = clock(), lastHeight = 0, announced = false;
        function markInput(kind) {
            inputUntil = clock() + INPUT_TAIL_MS;
            inputKind = kind || "pointer";
            if (onInput) onInput(inputKind);
        }
        function layoutChanged() { layoutAt = clock(); announced = true; }
        return {
            // Input is captured before the page's own handlers can swallow it. A pointer passing by is not
            // input; one moving with a button held is (a drag inside the page).
            listen: function (target) {
                Object.keys(INPUT_EVENTS).forEach(function (type) {
                    target.addEventListener(type, function (event) {
                        if (type === "pointermove" && !event.buttons) return;
                        markInput(INPUT_EVENTS[type]);
                    }, { passive: true, capture: true });
                });
            },
            markInput: markInput,
            // The page moves for the editor (or for the preview's own command) during the next ms.
            suppress: function (ms) { quietUntil = Math.max(quietUntil, clock() + ms); },
            suppressed: function () { return clock() < quietUntil; },
            layoutChanged: layoutChanged,
            // Call for every scroll event with the scroller's scrollHeight. A height that differs from the
            // last scroll's while no layout change was announced since is one nothing announced (a theme that
            // pins html to the window keeps ResizeObserver quiet). An announced change explains the new
            // height, however long ago: the next scroll is not the page settling again. Returns whether an
            // unannounced change was found.
            scrolled: function (height) {
                var unannounced = lastHeight > 0 && height !== lastHeight && !announced;
                lastHeight = height;
                announced = false;
                if (unannounced) layoutAt = clock();
                return unannounced;
            },
            inputActive: function () { return clock() < inputUntil; },
            // "" for the editor's request or the page settling; otherwise the user's: the input kind, or
            // "scrollbar" when no input explains the scroll.
            cause: function () {
                var at = clock();
                if (at < quietUntil) return "";
                if (at < inputUntil) return inputKind;
                if (at - layoutAt <= LAYOUT_SETTLE_MS) return "";
                return "scrollbar";
            }
        };
    }

    global.hcScrollIntent = { create: create, INPUT_TAIL_MS: INPUT_TAIL_MS, LAYOUT_SETTLE_MS: LAYOUT_SETTLE_MS };
}(typeof globalThis !== "undefined" ? globalThis : this));
