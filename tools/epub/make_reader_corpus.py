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
            entries[path]=text.encode()
    (a.output/name).write_bytes(archive(entries))
