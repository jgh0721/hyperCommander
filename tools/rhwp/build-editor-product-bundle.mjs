import fs from 'node:fs/promises';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
const [Engine,Source,Output]=process.argv.slice(2);
if(!Engine || !Source || !Output) throw Error('engine source new-output required');
const Run=(...Args)=>execFileSync(process.execPath,Args,{stdio:'inherit',windowsHide:true});
const Studio=Output+'-studio';
Run('tools/rhwp/build-viewer-bundle.mjs',Engine,Source,Output);
Run('tools/rhwp/build-bundle.mjs','build-editor',Source,Studio,Engine);
const StudioRoot=path.join(Studio,'site');
const Inputs=JSON.parse(await fs.readFile(path.join(StudioRoot,'asset-manifest.json'),'utf8'));
const Hash=Bytes=>createHash('sha256').update(Bytes).digest('hex');
const Manifest=JSON.parse(await fs.readFile(path.join(Output,'asset-manifest.json'),'utf8'));
for(const Entry of Inputs.files){
 const Bytes=await fs.readFile(path.join(StudioRoot,Entry.path));
 if(Hash(Bytes)!==Entry.sha256 || Bytes.length!==Entry.bytes) throw Error('Studio changed: '+Entry.path);
 const Name='editor/'+Entry.path,Target=path.join(Output,Name);
 await fs.mkdir(path.dirname(Target),{recursive:true}); await fs.writeFile(Target,Bytes,{flag:'wx'});
 Manifest.files.push({...Entry,path:Name});
}
const Policy=Buffer.from('{"schemaVersion":1,"edit":true}\r\n');
await fs.writeFile(path.join(Output,'editor/policy.json'),Policy,{flag:'wx'});
Manifest.files.push({path:'editor/policy.json',bytes:Policy.length,sha256:Hash(Policy)});
Manifest.profile='native-editor'; Manifest.capabilities.edit=true; Manifest.capabilities.save=true;
Manifest.files.sort((A,B)=>A.path<B.path?-1:A.path>B.path?1:0);
await fs.writeFile(path.join(Output,'asset-manifest.json'),(JSON.stringify(Manifest,null,2)+'\n').replace(/\r?\n/g,'\r\n'));
