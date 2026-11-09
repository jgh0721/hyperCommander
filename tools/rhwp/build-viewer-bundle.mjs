// Build a separate engine-only read-only candidate. Frozen H0 bundles are never modified.
import fs from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
const Root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const [EngineRoot,SourceRoot,OutputRoot]=process.argv.slice(2).map(Value=>path.resolve(Value));
if(!EngineRoot||!SourceRoot||!OutputRoot)throw Error('Usage: build-viewer-bundle.mjs <engine> <source> <new-output>');
const Hash=Bytes=>createHash('sha256').update(Bytes).digest('hex');
const Lock=JSON.parse(await fs.readFile(path.join(Root,'tools/rhwp/engine-lock.json'),'utf8'));
const Assets=new Map();
for(const [Name,Pin] of Object.entries(Lock.files)){
 const Bytes=await fs.readFile(path.join(EngineRoot,Name));if(Hash(Bytes)!==Pin)throw Error('Engine pin mismatch: '+Name);
 Assets.set(Name,Bytes);
}
for(const Name of ['index.html','viewer.js','viewer.css','readOnlyAdapter.js','startupTrace.js','hostPrint.js','printSnapshot.js'])
 Assets.set(Name,await fs.readFile(path.join(Root,'UniqueLibs/document/rhwp',Name)));
for(const [Input,Name] of [['LICENSE','notices/RHWP-LICENSE.txt'],['THIRD_PARTY_LICENSES.md','notices/UPSTREAM-THIRD-PARTY.txt']])
 Assets.set(Name,execFileSync('git',['-C',SourceRoot,'show',Lock.sourceCommit+':'+Input],{windowsHide:true}));
Assets.set('notices/engine-lock.json',await fs.readFile(path.join(Root,'tools/rhwp/engine-lock.json')));
const Manifest={schemaVersion:1,sourceCommit:Lock.sourceCommit,releaseEligible:false,profile:'native-read-only',
 capabilities:{view:true,edit:false,save:false,print:true,recovery:false},
 files:[...Assets].sort(([A],[B])=>A<B?-1:A>B?1:0).map(([Name,Bytes])=>({path:Name,bytes:Bytes.length,sha256:Hash(Bytes)}))};
await fs.mkdir(OutputRoot);
for(const [Name,Bytes] of Assets){const Target=path.join(OutputRoot,Name);await fs.mkdir(path.dirname(Target),{recursive:true});await fs.writeFile(Target,Bytes);}
await fs.writeFile(path.join(OutputRoot,'asset-manifest.json'),(JSON.stringify(Manifest,null,2)+'\n').replace(/\r?\n/g,'\r\n'));
console.log(JSON.stringify({output:OutputRoot,sha256:Hash(await fs.readFile(path.join(OutputRoot,'asset-manifest.json'))),files:Assets.size,releaseEligible:false}));
