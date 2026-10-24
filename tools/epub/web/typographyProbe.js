// Diagnostic observations are deliberately separate from compatibility acceptance.
// Only explicit self-test runs use the synthetic fixture marker.
export async function typographyProbe(api, fixture) {
    if (!["embedded-font", "css-priority", "ruby", "rtl", "vertical"].includes(fixture)) throw Error("Unknown typography fixture");
    const initialProfile = { ...api.profile() };
    const conditionalFixture = api.documents()[0]?.body.dataset.hcConditional === "true";
    const catalogFixture = api.documents()[0]?.body.dataset.hcCatalog === "true";
    const near = (value, expected) => Math.abs(parseFloat(value) - expected) < 0.2;
    const inspect = () => {
        const doc = api.documents()[0];
        const styles = selector => {
            const node = doc.querySelector(selector);
            if (!node) return null;
            const s = doc.defaultView.getComputedStyle(node), r = node.getBoundingClientRect();
            return { font: s.fontFamily, size: s.fontSize, line: s.lineHeight, direction: s.direction,
                writingMode: s.writingMode, display: s.display, x: r.x, y: r.y, width: r.width, height: r.height };
        };
        const axis = doc.getElementById("hc-axis");
        const visible = selector => {
            const node = doc.querySelector(selector), frame = doc.defaultView.frameElement;
            if (!node || !frame) return false;
            const r = node.getBoundingClientRect(), f = frame.getBoundingClientRect(), v = api.viewport();
            const left = f.left + r.left, top = f.top + r.top;
            return Math.min(left + r.width, v.right) > Math.max(left, v.left)
                && Math.min(top + r.height, v.bottom) > Math.max(top, v.top);
        };
        const glyphs = axis ? [0, 1].map(offset => {
            const range = doc.createRange(); range.setStart(axis.firstChild, offset); range.setEnd(axis.firstChild, offset + 1);
            const r = range.getBoundingClientRect(); return { x: r.x, y: r.y, width: r.width, height: r.height };
        }) : [];
        return { ...api.snapshot(), inlineStyles: [...doc.querySelectorAll("p, p *")].map(node => node.getAttribute("style")), body: styles("body"), ruby: styles("#hc-ruby"), rt: styles("#hc-ruby rt"),
            nested: styles("#hc-nested"), fontProbe: styles("#hc-font-probe"), glyphs,
            headingVisible: visible("h1"), firstParagraphVisible: visible("p"),
            scroll: { width: doc.scrollingElement?.scrollWidth, height: doc.scrollingElement?.scrollHeight,
                clientWidth: doc.documentElement.clientWidth, clientHeight: doc.documentElement.clientHeight } };
    };
    await api.apply({ override: false });
    let fontFaces = [];
    if (fixture === "embedded-font") {
        fontFaces = await Promise.race([api.documents()[0].fonts.load('20px "HCFixtureFont"', "\ue000"),
            new Promise((_, reject) => setTimeout(() => reject(Error("Embedded font timeout")), 5000))]);
    }
    const publisher = inspect();
    const previousProfile = JSON.stringify(api.profile());
    let invalidRejected = false;
    try { await api.apply({ size: 1000 }); } catch { invalidRejected = true; }
    const invalidProfilePreserved = invalidRejected && JSON.stringify(api.profile()) === previousProfile;
    await api.apply({ override: true, size: 24, line: 2.1, before: 0.4, after: 1.3,
        letter: 0.08, word: 0.25, indent: 1.5, align: "justify", font: "Batang", weight: 700 });
    const adjusted = inspect(), p = adjusted.paragraphs[0];
    const capabilities = { invalidProfilePreserved, typographyOverride: !!p && near(p.size, 24) && near(p.line, 50.4)
        && near(p.before, 9.6) && near(p.after, 31.2) && near(p.letter, 1.92) && near(p.word, 6)
        && near(p.indent, 36) && p.align === "justify" && p.font.includes("Batang") && p.weight === "700" };
    await api.apply({ line: 2.4 });
    const lineOnly = inspect().paragraphs[0];
    await api.apply({ before: 0.7, after: 1.8 });
    const paragraphOnly = inspect().paragraphs[0];
    capabilities.independentSpacing = near(lineOnly.line, 57.6) && near(lineOnly.before, 9.6) && near(lineOnly.after, 31.2)
        && near(paragraphOnly.line, 57.6) && near(paragraphOnly.before, 16.8) && near(paragraphOnly.after, 43.2);
    if (fixture === "embedded-font") {
        capabilities.embeddedFont = fontFaces.length > 0 && fontFaces.every(face => face.status === "loaded")
            && near(publisher.fontProbe.width, 36);
        await api.apply({ font: "HCFixtureFont", weight: 400 });
        capabilities.embeddedFontSelection = inspect().paragraphs[0].font.includes("HCFixtureFont");
    }
    if (fixture === "css-priority") capabilities.nestedInlineOverride = near(adjusted.nested?.size, 24);
    if (fixture === "ruby" || fixture === "vertical") {
        capabilities.rubyAnnotation = adjusted.rt?.display === "ruby-text" && parseFloat(adjusted.rt.size) < parseFloat(adjusted.ruby.size)
            && adjusted.rt.width > 0 && adjusted.rt.height > 0;
    }
    if (fixture === "rtl") capabilities.rtlDirection = adjusted.body.direction === "rtl" && adjusted.readingProgression === "rtl";
    if (fixture === "vertical") capabilities.verticalGlyphFlow = adjusted.body.writingMode === "vertical-rl"
        && adjusted.glyphs.length === 2 && Math.abs(adjusted.glyphs[0].x - adjusted.glyphs[1].x) < 1
        && adjusted.glyphs[1].y > adjusted.glyphs[0].y;
    await api.apply({ override: false });
    const restored = inspect();
    capabilities.publisherRestored = JSON.stringify(restored.paragraphs) === JSON.stringify(publisher.paragraphs);
    capabilities.publisherInlineRestored = JSON.stringify(restored.inlineStyles) === JSON.stringify(publisher.inlineStyles);
    const locator = api.locator();
    await api.chapter(1);
    const second = api.snapshot();
    if (catalogFixture) {
        capabilities.unvisitedFontDiscovered = publisher.bookFonts.includes("HCCatalogFont")
            && publisher.bookFonts.includes("HCInlineFont") && !publisher.bookFonts.includes("HCDeniedFont");
        await api.apply({ override: true, font: "HCCatalogFont", size: 20, letter: 0, word: 0 });
        const doc = api.documents()[0];
        const faces = await Promise.race([doc.fonts.load('20px "HCCatalogFont"', "\ue000"),
            new Promise((_, reject) => setTimeout(() => reject(Error("Catalog font timeout")), 5000))]);
        capabilities.crossChapterFontLoaded = faces.length > 0 && faces.every(face => face.status === "loaded")
            && near(doc.querySelector("#hc-font-probe").getBoundingClientRect().width, 36);
        if (conditionalFixture) {
            capabilities.conditionalFonts = publisher.bookFonts.includes("HCNestedFont")
                && !publisher.bookFonts.includes("HCPrintOnly") && !publisher.bookFonts.includes("HCSupportsDisabled");
            await api.apply({ font: "HCMediaResize", width: 1000 });
            const wide = api.snapshot().bookFonts.includes("HCMediaResize");
            await api.apply({ width: 400 });
            const narrowDoc = api.documents()[0];
            const narrow = !api.snapshot().bookFonts.includes("HCMediaResize")
                && ![...narrowDoc.fonts].some(face => face.family.includes("HCMediaResize"));
            await api.apply({ width: 1000 });
            const wideDoc = api.documents()[0];
            const restoredFaces = await Promise.race([wideDoc.fonts.load('20px "HCMediaResize"', "\ue000"),
                new Promise((_, reject) => setTimeout(() => reject(Error("Responsive font timeout")), 5000))]);
            capabilities.mediaFontTransition = wide && narrow && restoredFaces.length > 0
                && near(wideDoc.querySelector("#hc-font-probe").getBoundingClientRect().width, 36);
        }
        await api.apply({ override: false });
        capabilities.catalogPublisherReset = ![...api.documents()[0].fonts].some(face => face.family.includes("HCCatalogFont"));

    }
    await api.restore(locator);
    const returned = api.snapshot();
    const checks = { textRendered: publisher.text.length > 10,
        imagesDecoded: adjusted.images.length > 0 && adjusted.images.every(img => img.complete && img.naturalWidth > 0),
        chapterChanged: second.text !== publisher.text, locatorRestored: returned.text === publisher.text,
        metadataTransferred: adjusted.languages.includes(api.publication.languages[0])
            && adjusted.readingProgression === api.publication.readingProgression };
    if (catalogFixture) Object.assign(checks, {
        unvisitedFontDiscovered: capabilities.unvisitedFontDiscovered,
        crossChapterFontLoaded: capabilities.crossChapterFontLoaded,
        catalogPublisherReset: capabilities.catalogPublisherReset });
    if (conditionalFixture) Object.assign(checks, {
        conditionalFonts: capabilities.conditionalFonts, mediaFontTransition: capabilities.mediaFontTransition });
    await api.apply(initialProfile);
    const final = inspect();
    capabilities.firstContentAfterProfileRestore = final.headingVisible && final.firstParagraphVisible;
    return { status: Object.values(checks).every(Boolean) ? "passed" : "failed", phase: "P0-B-typography",
        scope: "Probe execution only; false capabilities remain unsupported and block acceptance", fixture,
        engine: api.engine, checks, capabilities, publisher, adjusted, restored, final,
        sourceSha256: api.publication.sourceSha256 };
}
