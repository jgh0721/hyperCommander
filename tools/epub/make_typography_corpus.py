"""Original typography fixtures and a synthetic CC0 test font (not a reading font)."""
from __future__ import annotations
import argparse
import hashlib
import io
import json
from pathlib import Path
import sys
from make_corpus import archive, publication


def synthetic_font():
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    font = FontBuilder(1000, isTTF=True)
    names = ['.notdef', 'space', 'probe']
    font.setupGlyphOrder(names)
    font.setupCharacterMap({32: 'space', 0xE000: 'probe'})
    glyphs = {}
    for name in names:
        pen = TTGlyphPen(None)
        if name != 'space':
            pen.moveTo((100, 0)); pen.lineTo((100, 700)); pen.lineTo((1600, 700)); pen.lineTo((1600, 0)); pen.closePath()
        glyphs[name] = pen.glyph()
    font.setupGlyf(glyphs)
    font.setupHorizontalMetrics({name: (1800 if name != 'space' else 500, 0) for name in names})
    font.setupHorizontalHeader(ascent=800, descent=-200)
    font.setupNameTable({'familyName': 'HCFixtureFont', 'styleName': 'Regular', 'uniqueFontIdentifier': 'HCFixtureFont-1',
        'fullName': 'HCFixtureFont Regular', 'psName': 'HCFixtureFont-Regular', 'version': 'Version 1.000',
        'copyright': 'Original synthetic test glyphs. CC0-1.0.'})
    font.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    font.setupPost(); font.setupMaxp()
    font.font['head'].created = font.font['head'].modified = 3875558400
    font.font.recalcTimestamp = False
    stream = io.BytesIO(); font.save(stream)
    return stream.getvalue()


def generate(output):
    output.mkdir(parents=True, exist_ok=False)
    cases = []
    for ident, language, direction in [('embedded-font', 'en', 'ltr'), ('css-priority', 'ko', 'ltr'),
                                      ('ruby', 'ja', 'ltr'), ('rtl', 'ar', 'rtl'), ('vertical', 'ja', 'rtl')]:
        entries = publication()
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<dc:language>ko</dc:language>', f'<dc:language>{language}</dc:language>'.encode())
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<spine>', f'<spine page-progression-direction="{direction}">'.encode())
        for path in ['EPUB/Text/첫 장.xhtml', 'EPUB/Text/second.xhtml']:
            raw = entries[path].decode()
            raw = raw.replace('xml:lang="ko"', f'xml:lang="{language}"' + (' dir="rtl"' if ident == 'rtl' else ''))
            raw = raw.replace('<body>', f'<body data-hc-probe="{ident}">')
            if ident == 'embedded-font':
                raw = raw.replace('<p>', '<p><span id="hc-font-probe" style="font-family:HCFixtureFont;font-size:20px;letter-spacing:0;word-spacing:0">\ue000</span> Font loading probe. ', 1)
            elif ident == 'css-priority':
                raw = raw.replace('<p>', '<p id="hc-priority" style="font:italic 700 11px/1.1 serif!important;letter-spacing:.3em!important;margin:9px 17px!important;text-align:right!important;color:#803030!important"><span id="hc-nested" style="font-size:14px!important;line-height:1!important">Nested text</span> ', 1)
            elif ident in ('ruby', 'vertical'):
                raw = raw.replace('<p>', '<p><ruby id="hc-ruby">日本語<rt>にほんご</rt></ruby><span id="hc-axis">日本語</span> ', 1)
            elif ident == 'rtl':
                raw = raw.replace('<p>', '<p><span id="hc-arabic">مرحبا بالعالم</span> <bdi dir="ltr">123 ABC</bdi> ', 1)
            entries[path] = raw.encode()
        if ident == 'embedded-font':
            font = synthetic_font()
            entries['EPUB/Fonts/probe.ttf'] = font
            entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'</manifest>', b'<item id="probe-font" href="Fonts/probe.ttf" media-type="font/ttf"/></manifest>')
            entries['EPUB/Styles/book.css'] += b'@font-face{font-family:HCFixtureFont;src:url("../Fonts/probe.ttf") format("truetype");font-weight:400;font-style:normal}'
        elif ident == 'css-priority':
            entries['EPUB/Styles/book.css'] += b':root{--publisher-size:13px}@media screen{body #hc-priority{font-size:var(--publisher-size)!important;margin-block-end:41px!important}}'
        elif ident == 'vertical':
            entries['EPUB/Styles/book.css'] += b'html,body{writing-mode:vertical-rl!important}rt{font-size:50%}img{max-height:360px}'
        elif ident == 'ruby':
            entries['EPUB/Styles/book.css'] += b'rt{font-size:50%}'
        data = archive(entries)
        (output / (ident + '.epub')).write_bytes(data)
        cases.append(dict(id=ident, file=ident+'.epub', language=language, readingProgression=direction,
                          sha256=hashlib.sha256(data).hexdigest(), bytes=len(data)))
    manifest = dict(schemaVersion=1, license='CC0-1.0', origin='Original text additions and synthetic rectangle glyph; base from make_corpus.py',
                    fontGenerator='fontTools 4.60.1 (test-only dependency)', epubCheckExecuted=False, cases=cases)
    (output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8', newline='\r\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fonttools', type=Path, help='Optional isolated fontTools package directory')
    args = parser.parse_args()
    if args.fonttools:
        sys.path.insert(0, str(args.fonttools.resolve()))
    print(json.dumps(generate(args.output), ensure_ascii=True))
