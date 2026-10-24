"""Generate a deterministic CC0 reflowable EPUB for bounded large-book validation."""
import argparse,hashlib,json
from pathlib import Path
from make_corpus import publication,archive
p=argparse.ArgumentParser();p.add_argument("output",type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
entries=publication();opf=entries["EPUB/content.opf"].decode();manifest=[];spine=[]
for chapter in range(160):
    name=f"Text/large-{chapter:03}.xhtml";manifest.append(f'<item id="large{chapter}" href="{name}" media-type="application/xhtml+xml"/>');spine.append(f'<itemref idref="large{chapter}"/>')
    paragraphs=[]
    for para in range(240):
        text=" ".join(hashlib.sha256(f"{chapter}/{para}/{part}".encode()).hexdigest() for part in range(4))
        paragraphs.append(f'<p>Chapter {chapter} paragraph {para} {text}</p>')
    entries["EPUB/"+name]=('<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Large chapter '+str(chapter)+'</title></head><body>'+''.join(paragraphs)+'<img src="../Images/alpha.png" alt="Transparent image"/></body></html>').encode()
opf=opf.replace("</manifest>",''.join(manifest)+"</manifest>");begin=opf.index("<spine");opf=opf[:begin]+"<spine>"+''.join(spine)+"</spine></package>";entries["EPUB/content.opf"]=opf.encode()
book=archive(entries);path=a.output/"large.epub";path.write_bytes(book)
record={"chapters":160,"paragraphs":38400,"inputBytes":len(book),"uncompressedBytes":sum(map(len,entries.values())),"sha256":hashlib.sha256(book).hexdigest()};(a.output/"manifest.json").write_text(json.dumps(record,indent=2));print(record)
