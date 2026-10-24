"""Reader-control fixtures built from the original CC0 package corpus."""
import argparse
from pathlib import Path
from make_corpus import publication, archive
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
source=a.output/"epub3-images.epub"
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
