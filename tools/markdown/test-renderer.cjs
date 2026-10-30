const fs=require('fs'),path=require('path'),http=require('http'),assert=require('assert');
const {chromium}=require(process.env.HC_PLAYWRIGHT_MODULE||'playwright');
(async()=>{
 const root=path.join(__dirname,'web');const server=http.createServer((req,res)=>{
  const file=path.join(root,decodeURIComponent(req.url.split('?')[0]).replace(/^\/markdown\//,''));
  if(!file.startsWith(root+path.sep)||!fs.existsSync(file)){res.writeHead(404);res.end();return;}
  const mime={'.js':'application/javascript','.css':'text/css','.html':'text/html','.woff2':'font/woff2','.woff':'font/woff','.ttf':'font/ttf'}[path.extname(file)]||'application/octet-stream';res.setHeader('Content-Type',mime);
  let data=fs.readFileSync(file);if(file.endsWith('shell.html'))data=data.toString().replaceAll('qrc:/markdown/','/markdown/').replace('<script src="qrc:///qtwebchannel/qwebchannel.js"></script>','').replace("script-src qrc:","script-src 'self'").replace("style-src qrc:","style-src 'self'").replace("font-src qrc:","font-src 'self'");res.end(data);
 });await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({executablePath:process.env.HC_BROWSER||'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{const page=await browser.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));const origin=`http://127.0.0.1:${server.address().port}`;await page.goto(origin+'/markdown/shell.html');await page.waitForFunction(()=>window.hcMarkdown);
 const render=text=>page.evaluate(async({text,origin})=>{await hcMarkdown.render({text,revision:1,baseUrl:origin+'/markdown/',options:{dark:true}});},{text,origin});
 await render('---\ntitle: hidden\n---\n# 한국어\n\nSetext\n---\n\n```text\n# not heading\n```\n\n## 반복\n\n## 반복\n\n$x^2$\n\n$$E=mc^2$$\n\n```mermaid\ngraph LR\n A-->B\n```\n\n```cpp\nint x = 3;\n```\n');
 assert.equal(await page.locator('h1,h2').count(),4);assert.equal(await page.locator('.katex').count(),2);assert.equal(await page.locator('.mermaid svg').count(),1);assert.equal(await page.locator('.problem').count(),0);assert(await page.locator('.hljs-type,.hljs-keyword').count()>0);const ids=await page.locator('h1,h2').evaluateAll(es=>es.map(e=>e.id));assert.equal(new Set(ids).size,ids.length);
 await render('# 안전\n<script>window.bad=true</script>\n\n![image](https://example.com/no.png)\n\n$$\\unknowncommand$$\n\n정상 문단');assert.equal(await page.evaluate(()=>window.bad),undefined);assert.equal(await page.locator('.blocked').count(),1);assert.equal(await page.locator('.problem').count(),1);assert((await page.locator('#content').innerText()).includes('정상 문단'));
 await page.evaluate(async()=>{const a=hcMarkdown.render({text:'```mermaid\ngraph LR\n A-->B\n```',revision:2});const b=hcMarkdown.render({text:'# Latest',revision:3});await Promise.all([a,b]);});assert.equal(await page.locator('h1').innerText(),'Latest');assert.deepEqual(errors,[]);console.log('PASS: headings/front matter/fences, Korean/duplicate IDs, KaTeX, Mermaid, code highlighting, HTML isolation, remote images, partial errors, latest revision');
 }finally{await browser.close();server.close();}
})().catch(e=>{console.error(e);process.exit(1)});
