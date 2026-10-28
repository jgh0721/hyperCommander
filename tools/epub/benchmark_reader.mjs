import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import net from 'node:net';
const freePort=()=>new Promise((resolve,reject)=>{const server=net.createServer();server.once('error',reject);server.listen(0,'127.0.0.1',()=>{const port=server.address().port;server.close(()=>resolve(port));});});
// Diagnostic renderer benchmark. Product launch/acquisition must be measured separately.
// node tools/epub/benchmark_reader.mjs --manifest selected.json --output NEW_DIR
//   --helper hc_epub_render_poc.exe --bundle BUNDLE --kit QT_KIT --repeats 30
//   [--ids b03,b05] [--all 1] [--cold-images 1]
const options={};for(let i=2;i<process.argv.length;i+=2){if(!process.argv[i].startsWith('--') || !process.argv[i+1])throw Error('Expected --option value');options[process.argv[i].slice(2)]=process.argv[i+1];}
for(const key of ['manifest','output','helper','bundle','kit'])if(!options[key])throw Error('Missing --'+key);
const repeats=Number(options.repeats||30);if(!Number.isInteger(repeats)||repeats<3||repeats>100)throw Error('repeats must be 3..100');
const root=path.resolve(options.output);fs.mkdirSync(root,{recursive:false});
const kit=path.resolve(options.kit),helper=path.resolve(options.helper),bundle=path.resolve(options.bundle);
const selected=JSON.parse(fs.readFileSync(options.manifest,'utf8'));
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const save=(name,v)=>fs.writeFileSync(path.join(root,name),JSON.stringify(v,null,2).replace(/\n/g,'\r\n'));
const ids=options.ids?options.ids.split(','):[];
for(const row of selected.filter(r=>(ids.length?ids.includes(r.id):true))) {
  const port=await freePort();let child,ws;const record={id:row.id,mode:'standalone Qt diagnostic helper',helper,bundle,repeats,operations:[]};
  try {
    const log=fs.openSync(path.join(root,`render-${row.id}.log`),'w');
    const started=performance.now();
    child=spawn(helper,['--epub',row.path,'--bundle',bundle,'--settings-file',path.join(root,`render-settings-${row.id}-${Date.now()}.json`)],{windowsHide:true,stdio:['ignore',log,log],env:{...process.env,PATH:`${kit}/bin;${process.env.PATH}`,QT_PLUGIN_PATH:`${kit}/plugins`,QTWEBENGINEPROCESS_PATH:`${kit}/bin/QtWebEngineProcess.exe`,QTWEBENGINE_RESOURCES_PATH:`${kit}/resources`,QTWEBENGINE_LOCALES_PATH:`${kit}/translations/qtwebengine_locales`,QTWEBENGINE_REMOTE_DEBUGGING:`127.0.0.1:${port}`}});
    fs.closeSync(log);
    let target;
    for(let i=0;i<400;i++) {if(child.exitCode!==null)throw Error('helper exited '+child.exitCode);try{const targets=await fetch(`http://127.0.0.1:${port}/json`).then(r=>r.json());target=targets.find(t=>t.url==='epub://app/app/index.html');if(target)break;}catch{}await pause(50);}
    if(!target)throw Error('CDP not available');
    ws=new WebSocket(target.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
    let serial=0;const pending=new Map();
    ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.reject(m.error):p.resolve(m.result);}};
    function send(method,params={}){return new Promise((resolve,reject)=>{const id=++serial;const timer=setTimeout(()=>{pending.delete(id);reject(Error("CDP command timeout"));},60000);pending.set(id,{resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}});ws.send(JSON.stringify({id,method,params}));});}
    async function evaluate(expression){const r=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
    for(let i=0;i<600;i++){const state=await evaluate('window.hcEvidence?.status');if(state==='error')throw Error(await evaluate('window.hcEvidence.message'));if(state==='ready'&&await evaluate('window.hcStartupDiagnostics().pendingCommands===0'))break;if(i===599)throw Error('ready timeout');await pause(50);}
    record.processToReadyMs=Math.round(performance.now()-started);
    record.initial=await evaluate('window.hcStartupDiagnostics()');
    await evaluate(`(()=>{window.__perfLong=[];window.__perfObserver=new PerformanceObserver(list=>{for(const e of list.getEntries())window.__perfLong.push({start:e.startTime,duration:e.duration});});window.__perfObserver.observe({type:'longtask'});return true;})()`);
    const info=await evaluate(`fetch('epub://app/app/publication.json').then(r=>r.json()).then(p=>({spine:p.spine,resources:p.resources.map(r=>({path:r.path,bytes:r.bytes,mediaType:r.mediaType})),images:p.images||[]}))`);
    record.book={chapters:info.spine.length,resourceBytes:info.resources.reduce((a,r)=>a+r.bytes,0)};
    const biggest=[...info.spine].sort((a,b)=>(info.resources.find(r=>r.path===b.path)?.bytes||0)-(info.resources.find(r=>r.path===a.path)?.bytes||0))[0];
    const largestIndex=info.spine.findIndex(s=>s.path===biggest.path);
    const imageCounts=info.spine.map(s=>info.images.filter(i=>i.document===s.path).length);
    const imageIndex=imageCounts.indexOf(Math.max(...imageCounts));
    record.targets={largestIndex,imageIndex,imageCounts};
    async function operation(label,command,value){
      const result=await evaluate(`(async()=>{const beforeChapter=window.hcSnapshot().chapterIndex,t=performance.now(),longStart=window.__perfLong.length;window.hcCommand(${JSON.stringify(command)},${JSON.stringify(value)});await new Promise((resolve,reject)=>{const poll=()=>{if(window.hcEvidence.status==='error')return reject(Error(window.hcEvidence.message));if(!window.hcStartupDiagnostics().pendingCommands && !window.hcSnapshot().readerSearch?.busy)return resolve();if(performance.now()-t>45000)return reject(Error('command timeout'));setTimeout(poll,8);};poll();});const ms=performance.now()-t;const s=window.hcSnapshot();const docs=[...document.querySelectorAll('#reader iframe')].filter(f=>!f.closest('[data-hc-retired]')).map(f=>f.contentDocument).filter(Boolean);return {ms,beforeChapter,changedChapter:s.chapterIndex!==beforeChapter,chapter:s.chapterIndex,page:s.chapterPage,pages:s.chapterPages,status:s.status,actionError:s.readerAction?.error,diag:window.hcStartupDiagnostics(),longTasks:window.__perfLong.slice(longStart),images:docs.flatMap(d=>[...d.images].map(i=>({w:i.naturalWidth,h:i.naturalHeight,complete:i.complete}))),nodes:docs.reduce((n,d)=>n+d.querySelectorAll('*').length,0),textChars:docs.reduce((n,d)=>n+(d.body?.textContent.length||0),0)};})()`);
      record.operations.push({label,...result});save(`render-${row.id}.json`,record);console.log(JSON.stringify({id:row.id,label,ms:Math.round(result.ms),chapter:result.chapter,images:result.images.length,longTasks:result.longTasks.map(t=>Math.round(t.duration))}));return result;
    }
    if(options['cold-images']==='1') {
      const initialChapter=await evaluate('window.hcSnapshot().chapterIndex');
      const entry=await operation('first-image-chapter','chapter',imageIndex);
      record.firstImageEntry={initialChapter,alreadyDisplayed:initialChapter===imageIndex,entryMs:entry.ms,
        ...(await evaluate(`(async()=>{const t=performance.now();const docs=[...document.querySelectorAll('#reader iframe')].filter(f=>!f.closest('[data-hc-retired]')).map(f=>f.contentDocument);const images=docs.flatMap(d=>[...d.images]);await Promise.all(images.map(i=>i.decode()));return {decodeWaitMs:performance.now()-t,imageCount:images.length,decodedPixels:images.reduce((n,i)=>n+i.naturalWidth*i.naturalHeight,0),resources:docs.flatMap(d=>d.defaultView.performance.getEntriesByType('resource').filter(r=>r.initiatorType==='img').map(r=>({name:r.name,duration:r.duration,transferSize:r.transferSize,decodedBodySize:r.decodedBodySize})))};})()`))};
      save(`render-${row.id}.json`,record);
    }
    for(let repeat=0;repeat<3;repeat++) {
      await operation(`chapter1-${repeat}`,'chapter',Math.min(1,info.spine.length-1));
      await operation(`largest-${repeat}`,'chapter',largestIndex);
      await operation(`page-next-${repeat}`,'read',{action:'next',serial:100+repeat});
      await operation(`image-chapter-${repeat}`,'chapter',imageIndex);
      await operation(`next-chapter-${repeat}`,'read',{action:'next-chapter',serial:200+repeat});
    }
    for(let repeat=0;repeat<repeats;repeat++) {
      const target=Math.max(1,imageIndex);
      await operation(`boundary-setup-${repeat}`,'chapter',target-1);
      await evaluate(`(()=>{const e=document.querySelector('#reader .epub-container');if(!e)throw Error('missing scroller');e.scrollTop=e.scrollHeight;e.scrollLeft=e.scrollWidth;return {h:e.scrollHeight,w:e.scrollWidth};})()`);
      await pause(Math.max(50,Math.min(2000,Number(options.dwell||50))));
      const boundary=await operation(`page-boundary-${repeat}`,'read',{action:'next',serial:600+repeat});
      if(!boundary.changedChapter || boundary.chapter!==target)throw Error('Boundary command did not enter expected chapter');
    }
    await operation('image-open-source','chapter',imageIndex);
    if(Math.max(...imageCounts)>0){await operation('image-popup','read',{action:'image',value:0,serial:301});record.popup=await evaluate(`(async()=>{const t=performance.now();const images=[...document.querySelectorAll('dialog img')];await Promise.all(images.map(i=>i.decode()));return {decodeWaitMs:performance.now()-t,images:images.map(i=>({w:i.naturalWidth,h:i.naturalHeight}))};})()`);await operation('image-close','read',{action:'close-popup',serial:302});}
    await operation('paginated','profile',{layout:'paginated',columns:1});
    await operation('paginated-largest','chapter',largestIndex);
    for(let i=0;i<3;i++)await operation(`paginated-next-${i}`,'read',{action:'next',serial:400+i});
    await operation('search-no-match','read',{action:'search',value:'HC_PERF_NONEXISTENT_20261028',serial:500});
    record.snapshotCost=await evaluate(`(()=>{const samples=[];for(let i=0;i<30;i++){const t=performance.now();JSON.stringify(window.hcSnapshot());samples.push(performance.now()-t);}return samples;})()`);
    record.nativeSnapshotCost=await evaluate(`(()=>{if(!window.hcNativeSnapshot)return null;const samples=[];for(let i=0;i<30;i++){const t=performance.now();JSON.stringify(window.hcNativeSnapshot());samples.push(performance.now()-t);}return {samples,fullBytes:JSON.stringify(window.hcSnapshot()).length,compactBytes:JSON.stringify(window.hcNativeSnapshot()).length};})()`);
    await send('Performance.enable');record.cdpMetrics=await send('Performance.getMetrics');
    if(options.all==='1') {
      for(let i=0;i<info.spine.length;i++){const current=await operation('all-spine-'+i,'chapter',i);if(current.chapter!==i)throw Error('Spine mismatch');}
      record.fonts=await evaluate('window.hcBookFonts()');record.fontChecks=[];
      for(const font of record.fonts){await operation('font-'+font,'profile',{font});await evaluate(`Promise.all([...document.querySelectorAll('#reader iframe')].filter(f=>!f.closest('[data-hc-retired]')).map(f=>f.contentDocument.fonts.load(${JSON.stringify("19px "+JSON.stringify(font))},"ABC한글あいう")))`);record.fontStatus=await evaluate("[...document.querySelectorAll('#reader iframe')].filter(f=>!f.closest('[data-hc-retired]')).flatMap(f=>[...f.contentDocument.fonts].map(face=>({family:face.family,status:face.status})))");record.fontChecks.push({font,faces:record.fontStatus});if(record.fontStatus.some(face=>face.status==='error'))throw Error('Book font failed');}
    }
    record.completed=true;save(`render-${row.id}.json`,record);
  } catch(e){process.exitCode=1;record.error=String(e);save(`render-${row.id}.json`,record);console.log(row.id,record.error);}
  finally {ws?.close();if(child&&child.exitCode===null){child.kill();await new Promise(resolve=>{child.once('exit',resolve);setTimeout(resolve,5000);});}await pause(600);}
}
