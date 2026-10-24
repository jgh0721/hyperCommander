"""Verify untouched EPUB ZIP local records and central metadata after native Save As."""
import io,json,struct,sys,zipfile
from pathlib import Path

def records(data):
    end=data.rfind(b"PK\x05\x06");offset=struct.unpack_from("<I",data,end+16)[0];result={}
    while data[offset:offset+4]==b"PK\x01\x02":
        name,extra,comment=struct.unpack_from("<HHH",data,offset+28);size=46+name+extra+comment;record=data[offset:offset+size];path=record[46:46+name].decode("utf-8");result[path]=record[:42]+record[46:];offset+=size
    return result

def verify(original,saved):
    old_records,new_records=records(original),records(saved)
    with zipfile.ZipFile(io.BytesIO(original)) as before,zipfile.ZipFile(io.BytesIO(saved)) as after:
        assert before.namelist()==after.namelist(),"entry order or names changed"
        assert before.comment==after.comment,"archive comment changed"
        assert after.infolist()[0].filename=="mimetype" and after.infolist()[0].compress_type==zipfile.ZIP_STORED and not after.infolist()[0].extra
        untouched=0;changed=[]
        for item in before.infolist():
            target=after.getinfo(item.filename);old=before.read(item);new=after.read(target)
            if old!=new:changed.append(item.filename);continue
            untouched+=1
            def local(data,info):
                n,e=struct.unpack_from("<HH",data,info.header_offset+26);return data[info.header_offset:info.header_offset+30+n+e+info.compress_size]
            assert local(original,item)==local(saved,target),(item.filename,"compressed local record changed")
            assert old_records[item.filename]==new_records[item.filename],(item.filename,"central metadata changed")
        assert len(changed)==4,("expected XHTML/CSS/OPF/PNG edits",changed)
        assert untouched>=3
        return {"untouchedRecords":untouched,"changedResources":changed,"archiveCommentPreserved":True}

def main():
    root=Path(sys.argv[1]);results=[]
    for source in sorted(root.glob("*.original")):
        output=root/source.name.removesuffix(".original");result=verify(source.read_bytes(),output.read_bytes());result["file"]=output.name;results.append(result)
    assert len(results)>=3,"native EPUB2/3 and compressed Save As evidence required"
    print(json.dumps(results,ensure_ascii=False,indent=2));return 0
if __name__=="__main__":raise SystemExit(main())
