"""Derive an unvisited-chapter font catalog fixture from the original CC0 typography fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
from make_corpus import archive


def generate(source, output, conditional=False):
    output.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(source) as book:
        entries = {name: book.read(name) for name in book.namelist()}
    css = entries['EPUB/Styles/book.css'].decode('utf-8')
    start = css.index('@font-face')
    font_rule = css[start:]
    entries['EPUB/Styles/book.css'] = css[:start].encode('utf-8')
    first = next(name for name in entries if name.startswith('EPUB/Text/') and name != 'EPUB/Text/second.xhtml')
    text = entries[first].decode('utf-8').replace('</head>', '<style>'+font_rule+'</style></head>')
    entries[first] = text.replace('<body ', '<body data-hc-catalog="true" ', 1).encode('utf-8')
    second = entries['EPUB/Text/second.xhtml'].decode('utf-8').replace('</head>', '<style>'+font_rule.replace('HCFixtureFont', 'HCInlineFont')+'</style></head>')
    entries['EPUB/Text/second.xhtml'] = second.encode('utf-8')
    entries['EPUB/Styles/catalog.css'] = (font_rule.replace('HCFixtureFont', 'HCCatalogFont')
        + '@font-face{font-family:HCDeniedFont;src:url("https://example.invalid/probe.ttf")}').encode('utf-8')
    entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'</manifest>',
        b'<item id="catalog" href="Styles/catalog.css" media-type="text/css"/></manifest>')
    if conditional:
        text = entries[first].decode('utf-8').replace('data-hc-catalog="true"', 'data-hc-catalog="true" data-hc-conditional="true"')
        entries[first] = text.encode('utf-8')
        entries['EPUB/Fonts/bad.woff2'] = b'Invalid synthetic font for fallback validation'
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'</manifest>',
            b'<item id="bad-font" href="Fonts/bad.woff2" media-type="font/woff2"/></manifest>')
        fallback = font_rule.replace('HCFixtureFont', 'HCCatalogFont').replace(
            'src:url("../Fonts/probe.ttf") format("truetype")',
            'src:local("Installed, Font"),url("https://example.invalid/font.ttf"),url("../Fonts/bad.woff2") format("woff2"),url("../Fonts/probe.ttf") format("truetype")')
        catalog = '@media screen{@supports (display:block){'+fallback+'}}'
        catalog += '@media print{'+font_rule.replace('HCFixtureFont', 'HCPrintOnly')+'}'
        catalog += '@supports (hc-unsupported:value){'+font_rule.replace('HCFixtureFont', 'HCSupportsDisabled')+'}'
        catalog += '@media (min-width:600px){'+font_rule.replace('HCFixtureFont', 'HCMediaResize')+'}'
        catalog += '@media screen{@supports (display:grid){'+font_rule.replace('HCFixtureFont', 'HCNestedFont')+'}}'
        entries['EPUB/Styles/catalog.css'] = catalog.encode('utf-8')
    data = archive(entries)
    (output/'book-font.epub').write_bytes(data)
    manifest = dict(schemaVersion=1, license='CC0-1.0', sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                    cases=[dict(file='book-font.epub', sha256=hashlib.sha256(data).hexdigest())])
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8', newline='\r\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument("--conditional", action="store_true")
    args = parser.parse_args()
    generate(args.source, args.output, args.conditional)
