"""Build an owned, hashed EPUB2/3 Save As corpus and verify it with EPUBCheck and two reader engines."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET
from make_corpus import publication, archive

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\r\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for option in ("native-probe","renderer","bundle","kit","java","epubcheck-jar","output"):
        parser.add_argument("--"+option, type=Path, required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    corpus=args.output/"corpus";corpus.mkdir()
    env=os.environ.copy();env["PATH"]=str(args.kit/"bin")+os.pathsep+env["PATH"]
    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    def run(command, timeout=120):
        result=subprocess.run([str(part) for part in command],env=env,capture_output=True,timeout=timeout,startupinfo=startup)
        if result.returncode:raise RuntimeError(f"Command failed ({result.returncode}): {command[0]}\n{result.stdout.decode('utf-8','replace')[-3000:]}\n{result.stderr.decode('utf-8','replace')[-3000:]}")
        return result
    def probe(path,*options):return json.loads(run([args.native_probe,path,*options]).stdout.decode("utf-8"))
    cases=[];preservation=[]
    for version in ("2.0","3.0"):
        prefix="epub"+version[0];source=corpus/(prefix+"-original.epub")
        source.write_bytes(archive(publication(version)));original_hash=sha(source)
        metadata=probe(source,"--metadata");before=probe(source)
        for kind in ("original","metadata","structure"):
            path=source if kind=="original" else corpus/(prefix+"-"+kind+".epub")
            changed=set();renamed={}
            if kind!="original":
                operations=[dict(operation="metadata",expectedSha256=metadata["sha256"],values=dict(title="골목과 산책 — 사본 & 검증",creator="HyperCommander 검증",language="ko"))]
                changed.add(metadata["resource"])
                if kind=="structure":
                    occurrence=next(row for row in probe(source,"--inspect-image","EPUB/Images/street.svg")["occurrences"] if row["editable"] and row["captionEditable"])
                    operations.append(dict(operation="image-attributes",resource="EPUB/Images/street.svg",document=occurrence["document"],elementPath=occurrence["elementPath"],expectedSha256=occurrence["sha256"],alt="사본의 선택 위치",width="320",height="180",caption="사본의 선택 위치 캡션"))
                    operations.extend([dict(operation="chapter-add",resource="EPUB/Text/추가 장.xhtml",title="추가 장",after=1),
                                       dict(operation="rename",resource="EPUB/Images/street.svg",destination="EPUB/Images/이동한 그림.svg")])
                    changed.update(row["path"] for row in before["resources"] if row["mediaType"] in ("application/xhtml+xml","application/x-dtbncx+xml","text/css"))
                    renamed["EPUB/Images/street.svg"]="EPUB/Images/이동한 그림.svg"
                requests=args.output/(prefix+"-"+kind+"-requests.json");write_json(requests,operations)
                probe(source,"--edit-requests",requests,"--output",path)
            actual=probe(path);after_metadata=probe(path,"--metadata")
            assert after_metadata["values"]["identifier"]==metadata["values"]["identifier"]
            assert kind=="original" or after_metadata["values"]["title"]=="골목과 산책 — 사본 & 검증"
            if kind=="structure":
                inspected=probe(path,"--inspect-image",renamed["EPUB/Images/street.svg"])["occurrences"]
                assert any(row.get("alt")=="사본의 선택 위치" and row.get("width")=="320" and row.get("height")=="180" for row in inspected)
            untouched=0
            with zipfile.ZipFile(source) as original,zipfile.ZipFile(path) as saved:
                for member in saved.namelist():
                    if member.endswith((".xhtml",".opf",".ncx",".xml",".svg")):ET.fromstring(saved.read(member))
                assert saved.namelist()[0]=="mimetype" and saved.getinfo("mimetype").compress_type==zipfile.ZIP_STORED
                for name in original.namelist():
                    if name in changed:continue
                    target=renamed.get(name,name);assert original.read(name)==saved.read(target),(path.name,name);untouched+=1
            assert sha(source)==original_hash
            report=args.output/(path.stem+"-epubcheck.json")
            checked=subprocess.run([str(args.java),"-jar",str(args.epubcheck_jar),str(path),"--json",str(report)],env=env,capture_output=True,timeout=120,startupinfo=startup)
            (args.output/(path.stem+"-epubcheck.log")).write_bytes(checked.stdout+checked.stderr)
            result=json.loads(report.read_text(encoding="utf-8")) if report.exists() else {}
            errors=[message for message in result.get("messages",[]) if message.get("severity") in ("ERROR","FATAL")]
            cases.append(dict(id=path.stem,file=path.name,sha256=sha(path),license="CC0-1.0",provenance="Owned make_corpus.py publication and native EditModel Save As",packageVersion=version,
                              features=["xhtml","ncx" if version=="2.0" else "nav","shared-svg","transparent-png",kind]+([] if kind=="original" else ["title","creator","language"])+(["image-alt","image-caption","image-width-height","chapter-add","rename"] if kind=="structure" else []),expectedStatus="readable",epubCheckExit=checked.returncode,epubCheckErrors=len(errors)))
            preservation.append(dict(case=path.stem,sourcePreserved=True,untouchedEntries=untouched,spineEntries=len(actual["spine"])))
    write_json(corpus/"manifest.json",dict(schemaVersion=1,cases=cases))
    write_json(args.output/"preservation.json",preservation)
    version=run([args.java,"-jar",args.epubcheck_jar,"--version"]).stdout.decode("utf-8","replace").strip()
    write_json(args.output/"toolchain.json",dict(epubCheckVersion=version,epubCheckJarSha256=sha(args.epubcheck_jar),nativeProbeSha256=sha(args.native_probe),readerDependencies=json.loads((Path(__file__).parent/"web/package.json").read_text(encoding="utf-8"))["dependencies"]))
    if any(case["epubCheckExit"]!=0 or case["epubCheckErrors"] for case in cases):
        raise RuntimeError("EPUBCheck did not accept every owned original and saved copy; see retained reports")
    run([sys.executable,Path(__file__).with_name("run_render_validation.py"),"--executable",args.renderer,"--bundle",args.bundle,"--kit",args.kit,"--corpus",corpus,
         "--compatibility-manifest",corpus/"manifest.json","--native","--output",args.output/"readers"],timeout=2400)
    reader_results=json.loads((args.output/"readers/validation.json").read_text(encoding="utf-8"))["cases"]
    assert all(record["sourcePositionChecks"] and all(record["sourcePositionChecks"].values()) for record in reader_results), "Source-position reader acceptance missing or failed"
    write_json(args.output/"acceptance.json",dict(status="passed",cases=len(cases),readerEngines=["epubjs","readium"],themes=["light","dark"],scope="Owned EPUB2/3 reflowable originals, metadata and structural Save As; broader compatibility is not implied"))
    print("Accepted",len(cases),"owned EPUB originals/copies with EPUBCheck and two reader engines")
    return 0

if __name__=="__main__":raise SystemExit(main())
