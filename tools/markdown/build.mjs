import {build} from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
await build({entryPoints:['web/renderer.js'],outfile:'web/bundle.js',bundle:true,minify:true,format:'iife',target:'chrome120',legalComments:'linked'});
fs.copyFileSync('node_modules/katex/dist/katex.min.css','web/katex.min.css');
fs.cpSync('node_modules/katex/dist/fonts','web/fonts',{recursive:true});
fs.mkdirSync('licenses',{recursive:true});
// Include transitive runtime license texts, not just top-level package notices.
function licenses(dir){for(const d of fs.readdirSync(dir,{withFileTypes:true})){if(!d.isDirectory()||d.name.startsWith('.'))continue;const p=path.join(dir,d.name);if(d.name.startsWith('@')){licenses(p);continue;}const pkg=path.join(p,'package.json');if(!fs.existsSync(pkg))continue;const info=JSON.parse(fs.readFileSync(pkg));for(const f of fs.readdirSync(p))if(/^(licen[sc]e|copying|notice)(\.|$)/i.test(f)&&fs.statSync(path.join(p,f)).isFile())fs.copyFileSync(path.join(p,f),path.join('licenses',info.name.replaceAll('/','_')+'-'+f));if(fs.existsSync(path.join(p,'node_modules')))licenses(path.join(p,'node_modules'));}}
licenses('node_modules');
const files=[];function scan(dir){for(const d of fs.readdirSync(dir,{withFileTypes:true})){const p=path.join(dir,d.name);if(d.isDirectory())scan(p);else{let b=fs.readFileSync(p);if(!/\.(woff2?|ttf)$/i.test(p)){b=Buffer.from(b.toString().replace(/\r?\n/g,"\r\n"));fs.writeFileSync(p,b);}files.push({path:p.replaceAll('\\','/'),bytes:b.length,sha256:crypto.createHash('sha256').update(b).digest('hex')});}}}scan('web');scan('licenses');fs.writeFileSync('assets.json',JSON.stringify({schemaVersion:1,files},null,2).replace(/\n/g,'\r\n')+'\r\n');
