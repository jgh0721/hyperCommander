"""Reader-control fixtures built from the original CC0 package corpus."""
import argparse
from pathlib import Path
from make_corpus import publication, archive, webp
p=argparse.ArgumentParser();p.add_argument("output",type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
for version,name in [("3.0","epub3-images.epub"),("2.0","epub2-ncx.epub")]:
    entries=publication(version)
    for path,data in list(entries.items()):
        if path.endswith(".xhtml") and "/Text/" in path:
            text=data.decode().replace('<html xmlns=', '<html xmlns:epub="http://www.idpf.org/2007/ops" xmlns=')
            text=text.replace("</body>",'<p>reader-<em>control</em> needle <a epub:type="noteref" href="#hc-note">각주</a></p><aside id="hc-note" epub:type="footnote"><p>각주 팝업 검증 내용</p></aside></body>')
            text=text.replace("<p>",'<p id="hc-reading-profile">',1)
            entries[path]=text.encode()
    (a.output/name).write_bytes(archive(entries))

# Exercise compressed local records and ZIP metadata preservation as well.
import io,zipfile
import urllib.parse
source=a.output/"epub3-images.epub"
entries=publication()
first=next(path for path in entries if path.startswith("EPUB/Text/") and not path.endswith("second.xhtml"))
href=urllib.parse.quote(first.removeprefix("EPUB/"))
entries[first]=('<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Navigation</title></head><body><h1 id="start">Start</h1>'+('<p>Reading navigation paragraph with enough text for several screens.</p>'*180)+'<h2 id="세부절">Unicode section</h2><p>End of section.</p></body></html>').encode()
entries["EPUB/nav.xhtml"]=(f'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>TOC</title></head><body><nav epub:type="toc"><ol><li><span>Part</span><ol><li><a href="{href}#start">Start</a></li><li><a href="{href}#%EC%84%B8%EB%B6%80%EC%A0%88">Section</a></li></ol></li></ol></nav></body></html>').encode()
(a.output/"epub3-toc-fragments.epub").write_bytes(archive(entries))
with zipfile.ZipFile(source) as before,zipfile.ZipFile(a.output/"epub3-compressed.epub","w") as after:
    after.comment=b"Original CC0 ZIP metadata preservation fixture"
    for old in before.infolist():
        info=zipfile.ZipInfo(old.filename,(2026,10,23,12,34,56));info.create_system=3;info.external_attr=0o100644<<16;info.comment=b"Retain entry comment"
        if old.filename!="mimetype":
            info.compress_type=zipfile.ZIP_DEFLATED;info.extra=bytes.fromhex("555405000100000000")
        after.writestr(info,before.read(old))

# A legacy cover with percentage heights collapses in a zero-height iframe.
entries=publication()
cover="EPUB/Text/첫 장.xhtml"
entries[cover]=b'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Percentage cover</title><style>html,body{height:100%;margin:0;padding:0}.cover{height:100%;display:-webkit-box;-webkit-box-align:center;-webkit-box-pack:center}img{width:100%;height:auto}</style></head><body><div class="cover"><img src="../Images/alpha.png" alt="cover"/></div></body></html>'
(a.output/"epub3-percentage-cover.epub").write_bytes(archive(entries))

# Safe legacy declarations, WebP images and explicit read-only spine recovery.
entries=publication("2.0")
entries["EPUB/content.opf"]=entries["EPUB/content.opf"].replace(b'application/x-dtbncx+xml',b'application/xhtml+xml').replace(b'media-type="text/css"',b'media-type="application/xhtml+xml"')
for name in list(entries):
    if name.endswith('.xhtml'):entries[name]=entries[name].replace(b'<head>',b'<head><meta http-equiv="Content-Type" content="text/html;charset=utf-8"/>')
(a.output/"epub2-legacy-mime.epub").write_bytes(archive(entries)+b'\x00')
entries=publication()
entries["EPUB/content.opf"]=entries["EPUB/content.opf"].replace(b'</manifest>',b'<item id="font" href="Fonts/test.ttf" media-type="application/x-font-ttf"/></manifest>')
entries["EPUB/Fonts/test.ttf"]=b'Unreferenced font declaration fixture'
(a.output/"epub3-legacy-font.epub").write_bytes(archive(entries))
entries=publication()
entries["EPUB/Images/alpha.webp"]=webp();del entries["EPUB/Images/alpha.png"]
for name in list(entries):
    if name.endswith(('.opf','.xhtml')):entries[name]=entries[name].replace(b'alpha.png',b'alpha.webp').replace(b'image/png',b'image/webp')
(a.output/"epub3-webp.epub").write_bytes(archive(entries))
entries["EPUB/content.opf"]=entries["EPUB/content.opf"].replace(b'<itemref idref="c2"/>',b'<itemref idref="absent"/><itemref idref="c2" linear="no"/>')
(a.output/"epub3-recovered-spine.epub").write_bytes(archive(entries))
