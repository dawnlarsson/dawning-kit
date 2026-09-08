// Isolated headless-Chrome checks for a generated standalone function atlas.
// Usage: node test/code_map_browser.mjs [artifact-directory]
import fs from 'node:fs/promises';
import {spawn} from 'node:child_process';
import {resolve, join} from 'node:path';
import {tmpdir} from 'node:os';
import {pathToFileURL, fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const dir=resolve(process.argv[2]||'artifacts/function-map-2026-09-08');
const html=join(dir,'index.html');
const profile=await fs.mkdtemp(join(tmpdir(),'dawning-atlas-qa-'));
const chrome=process.env.ATLAS_CHROME||'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const browser=spawn(chrome,['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--disable-background-networking','--remote-debugging-port=0','--user-data-dir='+profile,'about:blank'],{stdio:'ignore'});
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const checks=[],exceptions=[],logs=[],shots=[];
let socket;
const check=(name,pass,details={})=>checks.push({name,pass:!!pass,...details});
try {
 let port;
 for(let i=0;i<100;i++) {try{port=(await fs.readFile(join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];break;}catch{}await pause(100);}
 if(!port)throw Error('Isolated Chrome debugger did not start');
 const tabs=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();
 socket=new WebSocket(tabs.find(t=>t.url==='about:blank').webSocketDebuggerUrl);
 await new Promise((r,j)=>{socket.onopen=r;socket.onerror=j;});
 let serial=0;const pending=new Map();
 socket.onmessage=event=>{
  const m=JSON.parse(event.data);
  if(m.id){const p=pending.get(m.id);if(!p)return;pending.delete(m.id);clearTimeout(p.timer);m.error?p.reject(m.error):p.resolve(m.result);}
  else if(m.method==='Runtime.exceptionThrown')exceptions.push(m.params.exceptionDetails);
  else if(m.method==='Log.entryAdded'&&m.params.entry.level==='error')logs.push(m.params.entry);
 };
 const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++serial;const timer=setTimeout(()=>{pending.delete(id);reject(Error('CDP timeout: '+method));},15000);pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params}));});
 const evaluate=async expression=>{const r=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;};
 const select=async(id,value)=>evaluate(`(()=>{const x=document.getElementById(${JSON.stringify(id)});x.value=${JSON.stringify(value)};x.dispatchEvent(new Event('change',{bubbles:true}));return x.value;})()`);
 const click=async selector=>evaluate(`(()=>{const x=document.querySelector(${JSON.stringify(selector)});if(!x)throw Error('Missing selector: '+${JSON.stringify(selector)});x.click();return true;})()`);
 const query=async value=>evaluate(`(()=>{const x=document.getElementById('query');x.value=${JSON.stringify(value)};x.dispatchEvent(new Event('input',{bubbles:true}));})()`);
 const meta=async()=>evaluate(`({text:document.getElementById('view-meta').textContent,count:Number(document.getElementById('view-meta').textContent.split(' matching')[0].replaceAll(',',''))})`);
 const reset=async()=>{await click('#clear');await select('scope','production');await select('metric','attributed_loc');await select('sort','measure');};
 const layout=async()=>evaluate(`(()=>{const root=document.documentElement;const clips=[...document.querySelectorAll('body *')].filter(x=>{if(x.closest('.table-scroll'))return false;const s=getComputedStyle(x),r=x.getBoundingClientRect();return s.display!=='none'&&s.visibility!=='hidden'&&r.width>0&&r.right>innerWidth+1;}).slice(0,12).map(x=>({tag:x.tagName,id:x.id,class:x.className,right:x.getBoundingClientRect().right}));return {viewport:innerWidth,document:root.scrollWidth,overflow:root.scrollWidth>innerWidth+1,clips,background:getComputedStyle(document.body).backgroundColor};})()`);
 const screenshot=async label=>{await pause(180);const s=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});const filename='browser-'+label+'.png';await fs.writeFile(join(dir,filename),Buffer.from(s.data,'base64'));shots.push(filename);};
 await call('Runtime.enable');await call('Page.enable');await call('Log.enable');
 await call('Page.navigate',{url:pathToFileURL(html).href});
 for(let i=0;i<100;i++){if(await evaluate("document.readyState==='complete' && document.getElementById('view-meta').textContent.length>0"))break;await pause(100);}
 const initial=await evaluate(`({summary:DATA.summary,commit:DATA.commit,digest:DATA.source_digest,entries:F.length})`);
 for(const theme of ['light','dark'])for(const width of [1200,736,360]){
  await call('Emulation.setDeviceMetricsOverride',{width,height:960,deviceScaleFactor:1,mobile:false});
  await call('Emulation.setEmulatedMedia',{features:[{name:'prefers-color-scheme',value:theme}]});
  await reset();await click('[data-view="sinks"]');await evaluate('scrollTo(0,0)');
  let l=await layout();check(`overview layout ${width} ${theme}`,!l.overflow,{...l});
  await screenshot(`overview-${width}-${theme}`);
  if(width===360){
   const table=await evaluate("(()=>{const x=document.querySelector('#sinks-view .table-scroll');x.scrollLeft=x.scrollWidth;const moved=x.scrollLeft>0;x.scrollLeft=0;return {needed:x.scrollWidth>x.clientWidth,moved};})()");
   check(`mobile table scrolling ${theme}`,!table.needed||table.moved,table);
   await evaluate("document.getElementById('sinks-view').scrollIntoView({block:'start'})");await screenshot(`overview-content-${width}-${theme}`);
  }
  await click('[data-view="functions"]');await query('allocator_take');await click('#functions-body [data-function]');
  if(width>=1000)await evaluate("document.getElementById('functions-view').scrollIntoView({block:'start'})");
  await pause(350);l=await layout();check(`detail layout ${width} ${theme}`,!l.overflow,{...l});
  if(width===1200||width===360)await screenshot(`detail-${width}-${theme}`);
  for(const v of ['files','similarity','method']){await click(`[data-view="${v}"]`);l=await layout();check(`${v} layout ${width} ${theme}`,!l.overflow,l);if(v==='files'&&((width===1200&&theme==='light')||(width===360&&theme==='dark'))){await evaluate("document.getElementById('files-view').scrollIntoView({block:'start'})");await screenshot(`files-${width}-${theme}`);}}
 }
 // Pinned to the diagnostic fold: one shared assembly entry and five
 // additional support functions; the production C inventory is unchanged.
 // Scope values are an independently specified integration contract.
 await reset();await click('[data-view="functions"]');
 const scopeCounts={production:4245,c:3895,assembly:350,support:1575,all:5820};
 for(const [scope,want] of Object.entries(scopeCounts)){
  await select('scope',scope);const got=await meta();check('scope '+scope,got.count===want,{expected:want,...got});
 }
 await reset();await click('[data-view="files"]');
 for(const scope of ['production','c','assembly','support','all']){
  await select('scope',scope);
  const files=await evaluate(`(()=>{const scope=document.getElementById('scope').value;const symbolFiles=new Set(F.filter(f=>f.production&&(scope==='c'?!f.kind.startsWith('assembly'):f.kind.startsWith('assembly'))).map(f=>f.file));const expected=DATA.files.filter(f=>scope==='all'||scope==='production'&&f.production||scope==='support'&&!f.production||(scope==='c'||scope==='assembly')&&f.production&&symbolFiles.has(f.file));return {scope,rendered:[...document.querySelectorAll('#files-body [data-file]')].map(x=>x.dataset.file),expected:expected.map(f=>f.file),label:document.getElementById('view-meta').textContent,total:expected.reduce((n,f)=>n+f.physical_loc,0),metricHidden:document.getElementById('metric').closest('label').hidden};})()`);
  check('files scope '+scope,JSON.stringify(files.rendered.slice().sort())===JSON.stringify(files.expected.slice().sort())&&files.metricHidden&&files.label.includes(Number(files.total).toLocaleString('en-US')),{scope,renderedCount:files.rendered.length,expectedCount:files.expected.length,label:files.label});
  if(scope==='production')check('production file count',files.rendered.length===81,{count:files.rendered.length});
 }
 await reset();await click('[data-view="files"]');
 const fileArea=await evaluate("DATA.files.find(f=>f.file==='src/canvas/canvas.c').area");await select('area',fileArea);
 const fileAreaCheck=await evaluate("({rendered:document.querySelectorAll('#files-body [data-file]').length,expected:DATA.files.filter(f=>f.production&&f.area===document.getElementById('area').value).length})");
 check('files area filter',fileAreaCheck.rendered===fileAreaCheck.expected&&fileAreaCheck.expected>0,fileAreaCheck);
 await click('#clear');await query('src/standard/allocator.c');
 let fileResult=await evaluate("({rows:[...document.querySelectorAll('#files-body [data-file]')].map(b=>b.dataset.file),label:document.getElementById('view-meta').textContent,href:document.querySelector('#files-body a').href})");
 check('files path query',fileResult.rows.length===1&&fileResult.rows[0]==='src/standard/allocator.c',fileResult);
 const fileSource=new URL(fileResult.href);await call('Page.navigate',{url:fileResult.href});await pause(150);check('file source navigation',await evaluate("!!document.getElementById('L1')"),{href:fileResult.href});
 await call('Page.navigate',{url:pathToFileURL(html).href});await pause(300);await click('[data-view="files"]');
 await select('family','runtime.allocator');await select('role','storage_lifetime');await select('basis','body_reviewed');
 fileResult=await evaluate("({rows:[...document.querySelectorAll('#files-body [data-file]')].map(b=>b.dataset.file),shown:Number(document.querySelector('#files-body tr td:nth-child(2)').textContent.replaceAll(',','')),expected:DATA.files.find(f=>f.file==='src/standard/allocator.c').physical_loc})");
 check('files semantic filters retain whole-file total',fileResult.rows.length===1&&fileResult.rows[0]==='src/standard/allocator.c'&&fileResult.shown===fileResult.expected,fileResult);
 await click('#files-body [data-file]');
 const fileNav=await evaluate("({view:document.getElementById('view-title').textContent,query:document.getElementById('query').value,cleared:['family','role','basis'].every(id=>!document.getElementById(id).value),count:Number(document.getElementById('view-meta').textContent.split(' matching')[0].replaceAll(',','')),expected:F.filter(f=>f.production&&f.file==='src/standard/allocator.c').length})");
 check('file to function navigation',fileNav.view==='Functions'&&fileNav.query==='src/standard/allocator.c'&&fileNav.cleared&&fileNav.count===fileNav.expected,fileNav);
 await click('[data-view="files"]');await query('__atlas_no_matching_file_917771__');check('files empty query',await evaluate("document.querySelectorAll('#files-body [data-file]').length===0&&document.getElementById('view-meta').textContent.startsWith('0 matching files')"));
 await click('#clear');check('files clear filters',await evaluate("document.querySelectorAll('#files-body [data-file]').length===81"));
 await reset();await click('[data-view="functions"]');
 for(const scope of ['assembly','support']){
  await select('scope',scope);await click('#functions-body [data-function]');
  const links=await evaluate("[...document.querySelectorAll('#detail a[href]')].map(a=>a.href)");
  const broken=[];for(const href of links){const u=new URL(href);try{const text=await fs.readFile(fileURLToPath(u),'utf8');if(u.hash&&!text.includes(`id="${u.hash.slice(1)}"`))broken.push(href);}catch{broken.push(href);}}
  check(scope+' detail source links',links.length>0&&!broken.length,{checked:links.length,broken});
  if(scope==='assembly')check('assembly architecture details',await evaluate("document.getElementById('detail').textContent.includes('Architecture source spans')"));
 }
 await reset();await click('[data-view="sinks"]');
 const picked=await evaluate(`(()=>{const x=document.querySelector('#sinks-body [data-family]');return {id:x.dataset.family,title:x.textContent};})()`);
 await click('#sinks-body [data-family]');
 let state=await evaluate(`({family:document.getElementById('family').value,view:document.getElementById('view-title').textContent,rows:[...document.querySelectorAll('#functions-body tr')].length})`);
 check('family selection opens functions',state.family===picked.id&&state.view==='Functions'&&state.rows>0,{picked,...state});
 await click('#clear');
 const area=await evaluate("F.find(f=>f.name==='allocator_take').area");await select('area',area);
 const areaCheck=await evaluate("({rendered:Number(document.getElementById('view-meta').textContent.split(' matching')[0].replaceAll(',','')),expected:F.filter(f=>f.production&&f.area===document.getElementById('area').value).length})");
 check('area filtering',areaCheck.rendered===areaCheck.expected&&areaCheck.expected>0,areaCheck);await select('area','');
 await query('allocator_take');await select('family','runtime.allocator');await select('role','storage_lifetime');await select('basis','body_reviewed');
 let got=await meta();check('combined query family role evidence',got.count===1,got);
 await click('#functions-body [data-function]');
 let detail=await evaluate(`({name:document.querySelector('#detail h2').textContent,href:document.querySelector('#detail a').href,text:document.getElementById('detail').textContent})`);
 check('function detail selection',detail.name==='allocator_take'&&detail.text.includes('Family constraints')&&detail.text.includes('Possible callers'),{name:detail.name,href:detail.href});
 const sourceUrl=new URL(detail.href),source=await fs.readFile(fileURLToPath(sourceUrl),'utf8');
 check('source snapshot and line anchor',source.includes(`id="${sourceUrl.hash.slice(1)}"`),{path:fileURLToPath(sourceUrl),anchor:sourceUrl.hash});
 // Open the selected source in the isolated target, then return to the atlas.
 await call('Page.navigate',{url:detail.href});await pause(200);
 check('source browser navigation',await evaluate(`!!document.getElementById(${JSON.stringify(sourceUrl.hash.slice(1))})`));
 await call('Page.navigate',{url:pathToFileURL(html).href});await pause(300);
 await click('[data-view="functions"]');await query('allocator_take');await click('#functions-body [data-function]');
 const caller=await evaluate(`(()=>{const h=[...document.querySelectorAll('#detail h3')].find(x=>x.textContent.startsWith('Possible callers'));const b=h?.nextElementSibling?.querySelector('button[data-function]');if(!b)return null;return {name:b.textContent,index:b.dataset.function};})()`);
 check('possible caller link exists',!!caller,caller||{});
 if(caller){await click(`#detail [data-function="${caller.index}"]`);const name=await evaluate("document.querySelector('#detail h2').textContent");check('possible caller navigation',name===caller.name,{expected:caller.name,actual:name});}
 await click('#clear');got=await meta();check('clear restores production count',got.count===scopeCounts.production,got);
 const first=await evaluate("document.querySelector('#functions-body [data-function]').textContent");
 await click('#next');let page=await evaluate("({label:document.getElementById('page-label').textContent,first:document.querySelector('#functions-body [data-function]').textContent,prev:document.getElementById('prev').disabled})");
 check('next page',page.label.startsWith('76–150')&&page.first!==first&&!page.prev,page);
 await click('#prev');page=await evaluate("({label:document.getElementById('page-label').textContent,first:document.querySelector('#functions-body [data-function]').textContent,prev:document.getElementById('prev').disabled})");
 check('previous page',page.label.startsWith('1–75')&&page.first===first&&page.prev,page);
 for(const sort of ['name','callers','branches','measure']){await select('sort',sort);const values=await evaluate(`[...document.querySelectorAll('#functions-body [data-function]')].map(b=>{const f=F[Number(b.dataset.function)];return {name:f.name,value:f[document.getElementById('metric').value]||0,callers:f.production_caller_count,branches:f.branches};})`);check('sort '+sort,values.every((x,i)=>!i||(sort==='name'?values[i-1].name.localeCompare(x.name)<=0:sort==='callers'?values[i-1].callers>=x.callers:sort==='branches'?values[i-1].branches>=x.branches:values[i-1].value>=x.value)),{rows:values.length});}
 for(const metric of ['attributed_loc','attributed_code_lines','attributed_tokens']){await select('metric',metric);const data=await evaluate(`({label:document.getElementById('view-meta').textContent,expected:F.filter(f=>f.production).reduce((n,f)=>n+(f[document.getElementById('metric').value]||0),0)})`);check('measure '+metric,data.label.includes(Number(data.expected).toLocaleString('en-US')),data);}
 await query('__atlas_no_matching_function_919279__');got=await meta();check('empty results',got.count===0&&await evaluate("document.getElementById('page-label').textContent==='0 results'&&document.getElementById('next').disabled&&document.getElementById('prev').disabled"),got);
 await click('#clear');await click('[data-view="similarity"]');
 const sim=await evaluate(`({title:document.getElementById('view-title').textContent,rows:document.querySelectorAll('#similarity-body tr').length,links:document.querySelectorAll('#similarity-body [data-function]').length,notice:document.querySelector('#similarity-view .notice').textContent})`);
 check('similarity view',sim.title==='Similarity leads'&&sim.rows>0&&sim.notice.includes('does not establish'),sim);
 if(sim.links){const name=await evaluate("document.querySelector('#similarity-body [data-function]').textContent");await click('#similarity-body [data-function]');check('similarity function navigation',await evaluate("document.querySelector('#detail h2').textContent")==name,{expected:name});}
 await click('[data-view="method"]');const method=await evaluate("document.getElementById('method-view').textContent");
 check('method and exports',method.includes('Coverage is not review completion')&&method.includes('Data exports')&&method.includes(scopeCounts.c.toLocaleString('en-US')),{characters:method.length});
 for(const filename of ['functions.csv','functions.json','summary.json']){const stat=await fs.stat(join(dir,filename));check('export '+filename,stat.size>0,{bytes:stat.size});}
 check('no runtime exceptions',exceptions.length===0,{count:exceptions.length});
 check('no browser error log',logs.length===0,{count:logs.length});
 const hash=createHash('sha256').update(await fs.readFile(html)).digest('hex');
 const report={started_with:initial,html_sha256:hash,checks,exceptions,logs,screenshots:shots,passed:checks.filter(c=>c.pass).length,failed:checks.filter(c=>!c.pass).length};
 await fs.writeFile(join(dir,'browser-results.json'),JSON.stringify(report,null,2)+'\n');
 console.log(JSON.stringify({passed:report.passed,failed:report.failed,failures:checks.filter(c=>!c.pass),screenshots:shots},null,2));
 if(report.failed)process.exitCode=1;
 await call('Browser.close').catch(()=>{});
} catch(error) {
 await fs.writeFile(join(dir,'browser-failure.log'),String(error.stack||error)+'\n');
 await fs.writeFile(join(dir,'browser-results.json'),JSON.stringify({checks,exceptions,logs,screenshots:shots,fatal:String(error.stack||error)},null,2)+'\n');
 throw error;
} finally {
 socket?.close();browser.kill();await pause(100);await fs.rm(profile,{recursive:true,force:true}).catch(()=>{});
}
