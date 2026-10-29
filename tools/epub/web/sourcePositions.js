// Coordinates refer to the unchanged publisher DOM in the current preview revision.
export function sourcePositionForNode(doc, node, offset = 0) {
    const element = node?.nodeType === 1 ? node : node?.parentElement;
    if (!element || !doc.documentElement.contains(element)) return null;
    const path = []; let current = element;
    while (current !== doc.documentElement) {
        const parent = current.parentElement;
        if (!parent || path.length >= 128) return null;
        path.unshift([...parent.children].indexOf(current)); current = parent;
    }
    return {path, child: node === element ? -1 : [...element.childNodes].indexOf(node), offset};
}
export function placeSourcePosition(doc, anchor) {
    if (!Array.isArray(anchor?.path) || anchor.path.length > 128 || !anchor.path.every(i => Number.isInteger(i) && i >= 0 && i < 32768)
        || !Number.isInteger(anchor.child) || anchor.child < -1 || anchor.child > 65535
        || !Number.isInteger(anchor.offset) || anchor.offset < 0 || anchor.offset > 2*1024*1024) throw Error("소스 위치가 올바르지 않습니다.");
    let element = doc.documentElement;
    for (const index of anchor.path) element = element?.children[index];
    if (!element || !doc.body.contains(element) && element !== doc.body) throw Error("본문에 대응하는 소스 위치가 없습니다.");
    const range = doc.createRange();
    if (anchor.child >= 0) {
        const node = element.childNodes[anchor.child];
        if (!node || ![3,4].includes(node.nodeType) || anchor.offset > node.textContent.length) throw Error("본문의 소스 위치가 변경되었습니다.");
        range.setStart(node, anchor.offset); range.collapse(true);
    } else { range.selectNodeContents(element); range.collapse(true); }
    const selection = doc.defaultView.getSelection(); selection.removeAllRanges(); selection.addRange(range);
    // With scroll synchronisation the preview is already aligned; only a target out of view moves it.
    element.scrollIntoView({block:anchor.reveal === "nearest" ? "nearest" : "center", inline:"nearest"});
}
// container(): the scrolling element; a click's height ratio in it lets the editor put the line at the same height.
export function createSourcePositions(snapshot, container = () => null) {
    let selection = {}, serial = 0;
    const capture = doc => {
        const selected = doc.defaultView.getSelection();
        const point = sourcePositionForNode(doc, selected?.anchorNode, selected?.anchorOffset || 0);
        if (!point) return;
        let ratio;
        const host = container(), node = selected.anchorNode, owner = node?.nodeType === 1 ? node : node?.parentElement;
        const box = (selected.rangeCount ? selected.getRangeAt(0).getClientRects()[0] : null) || owner?.getBoundingClientRect();
        if (host && box && host.clientHeight > 0) {
            const frame = doc.defaultView.frameElement, top = box.top + (frame ? frame.getBoundingClientRect().top : 0) - host.getBoundingClientRect().top;
            ratio = Math.min(0.9, Math.max(0, top / host.clientHeight));
        }
        selection = {...point, chapter:snapshot().chapterIndex, serial:++serial, ...(ratio === undefined ? {} : {ratio})};
    };
    return {
        capture,
        hook(doc) {
            doc.addEventListener("pointerup", () => capture(doc));
            doc.addEventListener("keyup", event => {if (!event.isComposing) capture(doc);});
        },
        state: () => selection
    };
}
