// EPUB.js 0.3.93 format() ignores the return values of these setters, but
// each setter reads computed style immediately after writing it. Restrict
// batching to that synchronous call; ordinary measurement APIs stay intact.
const adapted = new WeakSet();
export function batchLayout(layout) {
    if (!layout || adapted.has(layout)) return;
    adapted.add(layout);
    const format = layout.format;
    layout.format = function(contents, ...args) {
        const saved = new Map(["width", "height", "css"].map(name => [name, Object.getOwnPropertyDescriptor(contents, name)]));
        const width = contents.width, height = contents.height;
        for (const [name, original] of [["width", width], ["height", height]]) {
            contents[name] = function(value) {
                if (!arguments.length) return original.call(this);
                if (value) this.content.style[name] = typeof value === "number" ? `${value}px` : value;
                return value;
            };
        }
        contents.css = function(property, value, priority) {
            const style = (this.content || this.document.body).style;
            if (value) style.setProperty(property, value, priority ? "important" : "");
            else style.removeProperty(property);
            return value;
        };
        try { return format.call(this, contents, ...args); }
        finally {
            for (const [name, descriptor] of saved) {
                if (descriptor) Object.defineProperty(contents, name, descriptor);
                else delete contents[name];
            }
        }
    };
}
