const fs=require('fs'),path=require('path'),http=require('http'),assert=require('assert');
const {chromium}=require(process.env.HC_PLAYWRIGHT_MODULE||'playwright');
(async()=>{
 const root=path.join(__dirname,'web');const server=http.createServer((req,res)=>{
  if(req.url.startsWith('/markdown/delayed.woff2')){setTimeout(()=>{res.setHeader('Content-Type','font/woff2');res.end(fs.readFileSync(path.join(root,'fonts/KaTeX_Main-Regular.woff2')));},700);return;}
  if(req.url.startsWith('/markdown/delayed.svg')){setTimeout(()=>{res.setHeader('Content-Type','image/svg+xml');res.end('<svg xmlns="http://www.w3.org/2000/svg" width="300" height="700"><rect width="300" height="700" fill="blue"/></svg>');},700);return;}
  const file=path.join(root,decodeURIComponent(req.url.split('?')[0]).replace(/^\/markdown\//,''));
  if(!file.startsWith(root+path.sep)||!fs.existsSync(file)){res.writeHead(404);res.end();return;}
  const mime={'.js':'application/javascript','.css':'text/css','.html':'text/html','.woff2':'font/woff2','.woff':'font/woff','.ttf':'font/ttf'}[path.extname(file)]||'application/octet-stream';res.setHeader('Content-Type',mime);
  let data=fs.readFileSync(file);if(file.endsWith('shell.html'))data=data.toString().replaceAll('qrc:/markdown/','/markdown/').replace('<script src="qrc:///qtwebchannel/qwebchannel.js"></script>','').replace("script-src qrc:","script-src 'self'").replace("style-src qrc:","style-src 'self'").replace("font-src qrc:","font-src 'self'");res.end(data);
 });await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({executablePath:process.env.HC_BROWSER||'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{const page=await browser.newPage({deviceScaleFactor:Number(process.env.HC_DPR||1)});const errors=[];page.on('pageerror',e=>errors.push(e.message));const origin=`http://127.0.0.1:${server.address().port}`;await page.goto(origin+'/markdown/shell.html');await page.waitForFunction(()=>window.hcMarkdown);
 const render=text=>page.evaluate(async({text,origin})=>{await hcMarkdown.render({text,revision:1,baseUrl:origin+'/markdown/',options:{dark:true}});},{text,origin});
 await render('---\ntitle: hidden\n---\n# 한국어\n\nSetext\n---\n\n```text\n# not heading\n```\n\n## 반복\n\n## 반복\n\n$x^2$\n\n$$E=mc^2$$\n\n```mermaid\ngraph LR\n A-->B\n```\n\n```cpp\nint x = 3;\n```\n');
 assert.equal(await page.locator('h1,h2').count(),4);assert.equal(await page.locator('.katex').count(),2);assert.equal(await page.locator('.mermaid svg').count(),1);assert.equal(await page.locator('.problem').count(),0);assert(await page.locator('.hljs-type,.hljs-keyword').count()>0);const ids=await page.locator('h1,h2').evaluateAll(es=>es.map(e=>e.id));assert.equal(new Set(ids).size,ids.length);
 await render('# 안전\n<script>window.bad=true</script>\n\n![image](https://example.com/no.png)\n\n$$\\unknowncommand$$\n\n정상 문단');assert.equal(await page.evaluate(()=>window.bad),undefined);assert.equal(await page.locator('.blocked').count(),1);assert.equal(await page.locator('.problem').count(),1);assert((await page.locator('#content').innerText()).includes('정상 문단'));
 await page.evaluate(async()=>{const a=hcMarkdown.render({text:'```mermaid\ngraph LR\n A-->B\n```',revision:2});const b=hcMarkdown.render({text:'# Latest',revision:3});await Promise.all([a,b]);});assert.equal(await page.locator('h1').innerText(),'Latest');assert.deepEqual(errors,[]);
 const stableText='# Stable\n\n```mermaid\ngraph LR\n A-->B\n```\n\n'+Array.from({length:35},(_,i)=>`Paragraph ${i} with text to keep the viewport stable.`).join('\n\n');
 await render(stableText);await page.evaluate(()=>{scrollTo(0,350);window.__keptDiagram=document.querySelector('#content .mermaid');});
 await page.evaluate(({text,origin})=>{window.__oldHtml=document.getElementById('content').innerHTML;window.__oldY=scrollY;window.__pending=hcMarkdown.render({text,revision:4,baseUrl:origin+'/markdown/',options:{dark:true}});},{text:stableText+'\n\n![Delayed](delayed.svg)',origin});
 await page.waitForSelector('[data-preview-staging]',{state:'attached'});
 const during=await page.evaluate(async()=>{const out=[];for(let i=0;i<12;i++){await new Promise(requestAnimationFrame);out.push({same:document.getElementById('content').innerHTML===__oldHtml,y:scrollY===__oldY});}return out;});
 assert(during.every(x=>x.same&&x.y),'Displayed body moved while the new image was pending');
 await page.evaluate(()=>__pending);assert(await page.evaluate(()=>document.querySelector('#content .mermaid')===__keptDiagram),'Unchanged diagram was replaced');
 const settled=await page.evaluate(async()=>{const samples=[];for(let i=0;i<15;i++){await new Promise(requestAnimationFrame);samples.push([scrollY,document.getElementById('content').getBoundingClientRect().height]);}return samples;});
 assert(settled.every(x=>JSON.stringify(x)===JSON.stringify(settled[0])),'Layout moved after the new frame became visible');
 assert.equal(await page.locator('[data-preview-staging]').count(),0);assert.deepEqual(errors,[]);
 await page.addStyleTag({content:`@font-face{font-family:DelayedPreview;src:url('${origin}/markdown/delayed.woff2')}`});
 await page.evaluate(({origin})=>{window.__oldHtml=document.getElementById('content').innerHTML;window.__pending=hcMarkdown.render({text:'# Font arrival\n\n'+('Text paragraph.\n\n'.repeat(50)),revision:5,baseUrl:origin+'/markdown/',options:{previewFonts:{enabled:true,mode:'user',bodyFamily:'DelayedPreview',bodySizePt:15}}});},{origin});
 await page.waitForSelector('[data-preview-staging]',{state:'attached'});
 assert(await page.evaluate(async()=>{for(let i=0;i<12;i++){await new Promise(requestAnimationFrame);if(document.getElementById('content').innerHTML!==__oldHtml)return false;}return true;}),'Displayed body changed before its font arrived');
 await page.evaluate(()=>__pending);assert(await page.evaluate(()=>document.fonts.check('15pt DelayedPreview')));assert.deepEqual(errors,[]);
 // Context menu without the bridge (still loading): Chromium keeps the event, so its request reaches the host fallback.
 await render('# Menu\n\nPlain [link](https://example.com/) text.\n');
 await page.evaluate(()=>{window.__prevented=null;addEventListener('contextmenu',e=>{window.__prevented=e.defaultPrevented;});});
 await page.locator('#content a').click({button:'right'});assert.equal(await page.evaluate(()=>window.__prevented),false,'Context menu was taken without the bridge');
 // Context menu with the bridge: the page reports the point, selection, link and source position; Chromium's menu never opens.
 const bridged=await browser.newPage({deviceScaleFactor:Number(process.env.HC_DPR||1)});bridged.on('pageerror',e=>errors.push(e.message));
 await bridged.addInitScript(()=>{window.__reports=[];window.qt={webChannelTransport:{}};window.QWebChannel=function(_,ready){ready({objects:{markdown:{report:s=>__reports.push(JSON.parse(s)),request:{connect:f=>{window.__request=f;}}}}});};});
 await bridged.goto(origin+'/markdown/shell.html');await bridged.waitForFunction(()=>window.hcMarkdown&&window.__request&&__reports.some(r=>r.type==='ready'));
 const menuText='# Title\n\nFirst paragraph with [a link](https://example.com/page) inside.\n\nSecond [jump](#title) paragraph.\n\n'+Array.from({length:60},(_,i)=>`Filler paragraph ${i}.`).join('\n\n');
 await bridged.evaluate(async({text,origin})=>{await hcMarkdown.render({text,revision:7,baseUrl:origin+'/markdown/',options:{}});},{text:menuText,origin});
 await bridged.evaluate(()=>{window.__prevented=null;addEventListener('contextmenu',e=>{window.__prevented=e.defaultPrevented;});});
 const context=async(locator,options={})=>{await bridged.evaluate(()=>{__reports.length=0;window.__prevented=null;});await locator.click({button:'right',...options});
  await bridged.waitForFunction(()=>__reports.some(r=>r.type==='context'));assert.equal(await bridged.evaluate(()=>window.__prevented),true,'Chromium context menu was not suppressed');
  return bridged.evaluate(()=>__reports.find(r=>r.type==='context'));};
 const external=bridged.locator('a[href^="https"]');let menu=await context(external);const box=await external.boundingBox();
 assert.equal(menu.link,'https://example.com/page');assert.equal(menu.fragment,'');assert.equal(menu.selection,'');assert.equal(menu.revision,7);
 assert.equal(menu.line,3);assert(menu.column>=22&&menu.column<=28,`Link column ${menu.column}`);
 assert(Math.abs(menu.x-(box.x+box.width/2))<=1&&Math.abs(menu.y-(box.y+box.height/2))<=1,'Context point is not in viewport CSS px');
 await bridged.evaluate(()=>getSelection().selectAllChildren(document.querySelector('#content p')));
 menu=await context(external);assert(menu.selection.startsWith('First paragraph'),`Selection ${JSON.stringify(menu.selection)}`);
 await bridged.evaluate(()=>getSelection().removeAllRanges());
 menu=await context(bridged.locator('a[href="#title"]'));assert.equal(menu.fragment,'title');assert.equal(menu.line,5);assert.equal(menu.selection,'');
 // Outside the text column there is no link, and the line still locates the source.
 menu=await context(bridged.locator('body'),{position:{x:4,y:300}});assert.equal(menu.link,'');assert(menu.line>=1,`Line ${menu.line}`);
 // The host opens an in-document link through an anchor request.
 await bridged.evaluate(()=>scrollTo(0,document.documentElement.scrollHeight));assert(await bridged.evaluate(()=>scrollY>100),'Document too short to scroll');
 await bridged.evaluate(()=>__request(JSON.stringify({type:'anchor',id:'title',revision:7})));
 await bridged.waitForFunction(()=>Math.abs(document.getElementById('title').getBoundingClientRect().top)<=1);
 assert.deepEqual(errors,[]);
 console.log('PASS: context menu reports point, selection, link, fragment and source line; Chromium menu only without the bridge; anchor request');
 console.log('PASS: delayed font preserves last good frame');
 console.log('PASS: retained visible frame during delayed image, stable committed layout, unchanged diagram node');console.log('PASS: headings/front matter/fences, Korean/duplicate IDs, KaTeX, Mermaid, code highlighting, HTML isolation, remote images, partial errors, latest revision');
 }finally{await browser.close();server.close();}
})().catch(e=>{console.error(e);process.exit(1)});
