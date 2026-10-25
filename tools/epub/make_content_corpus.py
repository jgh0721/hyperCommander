from pathlib import Path
import zipfile,io,json,hashlib,struct,argparse
from make_corpus import publication
parser=argparse.ArgumentParser(description='Generate owned EPUB fixed/mixed/RTL/vertical/Ruby and legacy-ZIP regression fixtures')
parser.add_argument('output',type=Path);out=parser.parse_args().output;out.mkdir(parents=True,exist_ok=True)
baseline=publication()
records=[]
for kind in ('fixed','mixed','rtl','vertical','ruby','fixed-missing-viewport','conflicting-layout','legacy-deflate-flags','legacy-container'):
    entries=baseline.copy();opf=entries['EPUB/content.opf'].decode()
    if kind in ('fixed','mixed','fixed-missing-viewport'):
        opf=opf.replace('</metadata>','<meta property="rendition:layout">pre-paginated</meta></metadata>')
    if kind=='mixed':opf=opf.replace('<itemref idref="c2"/>','<itemref idref="c2" properties="rendition:layout-reflowable"/>')
    if kind=='conflicting-layout':opf=opf.replace('<itemref idref="c1"/>','<itemref idref="c1" properties="rendition:layout-reflowable rendition:layout-pre-paginated"/>')
    if kind in ('rtl','vertical'):opf=opf.replace('<spine>','<spine page-progression-direction="rtl">')
    entries['EPUB/content.opf']=opf.encode()
    for name in tuple(entries):
        if name.startswith('EPUB/Text/') and name.endswith('.xhtml'):
            text=entries[name].decode().replace('<p>','<p id="hc-reading-profile">',1)
            if kind in ('fixed','mixed'):
                text=text.replace('</head>','<meta name="viewport" content="width=1200,height=900"/></head>')
                if kind=='fixed' or 'second' not in name:
                    text=text.replace('</head>','<style>html,body{margin:0;width:1200px;height:900px}h1{position:absolute;left:80px;top:80px}p#hc-reading-profile{position:absolute;left:80px;top:240px;width:920px}body p:not(#hc-reading-profile){display:none}body div{position:absolute;left:80px;top:420px}img{max-width:320px}</style></head>')
            if kind=='rtl':text=text.replace('xml:lang="ko"','xml:lang="ar" dir="rtl"').replace('나무 그늘에서','مرحبا بالعالم القراءة من اليمين إلى اليسار ')
            if kind in ('vertical','ruby'):
                text=text.replace('</head>','<style>ruby{ruby-position:over}rt{font-size:50%}</style></head>').replace('<p id="hc-reading-profile">','<p id="hc-reading-profile"><ruby>日本語<rt>にほんご</rt></ruby> ')
            if kind=='vertical':text=text.replace('</head>','<style>html,body{writing-mode:vertical-rl}body{line-height:1.8}img{max-height:300px}</style></head>')
            entries[name]=text.encode()
    if kind=='legacy-container':
        mime=entries.pop('mimetype');entries['mimetype']=mime
    stream=io.BytesIO()
    with zipfile.ZipFile(stream,'w') as z:
        for name,data in entries.items():z.writestr(name,data,compress_type=zipfile.ZIP_STORED if name=='mimetype' else zipfile.ZIP_DEFLATED)
    data=bytearray(stream.getvalue())
    if kind in ('legacy-deflate-flags','legacy-container'):
        end=data.rindex(b'PK\x05\x06');at=struct.unpack_from('<I',data,end+16)[0]
        while data[at:at+4]==b'PK\x01\x02':
            if struct.unpack_from('<H',data,at+10)[0]==8 or kind=='legacy-container':
                flags=struct.unpack_from('<H',data,at+8)[0]|0x16;offset=struct.unpack_from('<I',data,at+42)[0]
                struct.pack_into('<H',data,at+8,flags);struct.pack_into('<H',data,offset+6,flags)
            n,e,c=struct.unpack_from('<HHH',data,at+28);at+=46+n+e+c
    path=out/(kind+'.epub');path.write_bytes(data);records.append(dict(kind=kind,path=str(path.resolve()),sha256=hashlib.sha256(data).hexdigest()))
(out/'fixtures.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
print('Prepared',len(records),'owned EPUB content fixtures')
