// Match the reader's effective Qt portable bindings inside Chromium's iframe.
// Numpad navigation works with either Num Lock state, outside editable fields.
export function pageKeyAction(event, bindings) {
    if (event.isComposing || event.defaultPrevented || event.target?.closest?.("input,textarea,select,[contenteditable]")) return null;
    const aliases = { PgUp: "PageUp", PgDown: "PageDown", Space: " ", Left: "ArrowLeft", Right: "ArrowRight" };
    const key = event.code === "Numpad9" ? "PageUp" : event.code === "Numpad3" ? "PageDown" : event.key;
    for (const [id, action] of [["epub.next", "next"], ["epub.previous", "previous"]]) {
        const binding = bindings[id];
        if (typeof binding !== "string" || !binding || binding.includes(",")) continue;
        const parts = binding.split("+"), expected = parts.pop();
        if (["Ctrl", "Alt", "Shift", "Meta"].some((modifier, i) => parts.includes(modifier) !== !![event.ctrlKey, event.altKey, event.shiftKey, event.metaKey][i])) continue;
        if ((aliases[expected] || expected).toUpperCase() === key?.toUpperCase()) return action;
    }
    return null;
}
