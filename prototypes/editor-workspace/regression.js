// playwright-cli -s=workspace-prototype run-code --filename prototypes/editor-workspace/regression.js
async(page)=>{
  const origin=await page.evaluate(()=>location.origin),checks=[],errors=[],requests=[];
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Refusing nonlocal test target');
  const check=(name,pass,metrics={})=>checks.push({name,pass,...metrics});
  const onError=e=>errors.push(String(e));
  const onRequest=r=>requests.push({url:r.url(),method:r.method()});
  page.on('pageerror',onError);page.on('request',onRequest);
  await page.goto(origin);
  const saved=await page.evaluate(()=>localStorage.getItem(Workspace.key));
  const ready=()=>page.waitForFunction(()=>Workspace.image&&Workspace.ready===Workspace.revision&&!Workspace.error,{},{timeout:20000});
  const frame=()=>page.evaluate(()=>Workspace.frame.toDataURL());
  const input=async(id,value,commit=true)=>page.locator('#'+id).evaluate((e,{value,commit})=>{e.value=String(value);e.dispatchEvent(new Event('input',{bubbles:true}));if(commit)e.dispatchEvent(new Event('change',{bubbles:true}));},{value,commit});
  const download=async(name,data)=>{const wait=page.waitForEvent('download');await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});await(await wait).saveAs('output/workspace-prototype/'+name);};
  try{
    await page.evaluate(()=>localStorage.removeItem(Workspace.key));await page.reload();await ready();
    const session=await(await page.request.get(origin+'/api/session')).json();
    if(session.app!=='msp-editor-workspace-prototype'||session.deviceAccess!==false)throw Error('Not an isolated prototype');
    check('loopback simulated session',session.prototype&&session.deviceAccess===false&&session.panels===39,session);
    const profiles=await page.evaluate(()=>Workspace.catalog);
    check('unique valid descriptor inventory',new Set(profiles.map(p=>p.id)).size===39&&profiles.find(p=>p.id===47).width===128&&profiles.find(p=>p.id===43).family==='e6');
    const palette={bw:[0,0xffffff],bwr:[0,0xffffff,0xc0392b],bwry:[0,0xffffff,0xc0392b,0xd2a518],e6:[0,0xffffff,0xc4241c,0xffe800,0x1450b9,0x28965a]};
    for(const profile of profiles){
      await page.locator('#panelSelect').selectOption(String(profile.id));await ready();
      const m=await page.evaluate(allowed=>{
        const w=Workspace.profile.width,h=Workspace.profile.height,d=Workspace.frame.getContext('2d').getImageData(0,0,w,h).data;let illegal=0;
        for(let i=0;i<d.length;i+=4)if(!allowed.includes((d[i]<<16)|(d[i+1]<<8)|d[i+2])||d[i+3]!==255)illegal++;
        return {width:Workspace.frame.width,height:Workspace.frame.height,illegal,colorHidden:document.querySelector('[data-mode="color"]').hidden,revision:Workspace.ready};
      },palette[profile.family]);
      check('panel '+profile.id+' dimensions and legal inks',m.width===profile.width&&m.height===profile.height&&m.illegal===0&&m.colorHidden===(profile.family==='bw'),m);
    }
    await page.locator('#panelSelect').selectOption('34');await ready();
    const base=await frame(),hist=await page.evaluate(()=>Workspace.history.length);
    await input('param-brightness',7,false);await input('param-brightness',13,false);await input('param-brightness',19);await ready();
    check('real slider updates frame',(await frame())!==base);
    check('slider gesture is one history item',await page.evaluate(h=>Workspace.history.length===h+1,hist));
    await page.locator('#undo').click();await ready();check('undo restores pixels',(await frame())===base);
    await page.locator('#redo').click();await ready();check('redo restores value',await page.evaluate(()=>Workspace.state.params.color.brightness===19));
    await page.locator('#reset').click();await ready();
    await page.locator('[data-tab="compose"]').click();await page.locator('#fitMode').selectOption('cover');await ready();await input('cropZoom',2);await ready();
    const beforeCrop=await frame(),c0=await page.evaluate(()=>Workspace.state.composition.cx),r=await page.locator('#display').boundingBox();
    await page.mouse.move(r.x+r.width*.6,r.y+r.height*.5);await page.mouse.down();await page.mouse.move(r.x+r.width*.4,r.y+r.height*.5,{steps:6});await page.mouse.up();await ready();
    check('drag adjusts crop',await page.evaluate(c=>Workspace.state.composition.cx!==c,c0));check('crop changes output',(await frame())!==beforeCrop);
    const params=await page.evaluate(()=>JSON.stringify(Workspace.state)),native=await frame();
    const effect=await page.evaluate(()=>document.getElementById('display').toDataURL());
    const eye=await page.locator('#compare').boundingBox();await page.mouse.move(eye.x+eye.width/2,eye.y+eye.height/2);await page.mouse.down();
    check('hold compare shows original',await page.evaluate(previous=>Workspace.compare&&!document.getElementById('compareLabel').hidden&&document.getElementById('display').toDataURL()!==previous,effect));
    await page.screenshot({path:'output/workspace-prototype/compare-original.png'});await page.mouse.up();
    check('compare does not mutate processing',await page.evaluate(s=>!Workspace.compare&&JSON.stringify(Workspace.state)===s,params)&&(await frame())===native);
    await page.locator('#actualView').click();await page.locator('#zoomSelect').selectOption('200');await page.locator('#fitView').click();
    check('view zoom does not mutate output',(await frame())===native);
    const splitter=await page.locator('#splitter').boundingBox();await page.mouse.move(splitter.x+splitter.width/2,splitter.y+splitter.height/2);await page.mouse.down();await page.mouse.move(splitter.x-60,splitter.y+splitter.height/2);await page.mouse.up();
    check('sidebar can resize without altering frame',await page.evaluate(()=>Workspace.sidebar>=350&&Workspace.sidebar<=380)&&(await frame())===native);
    await page.locator('#toggleSidebar').click();check('sidebar collapses',await page.evaluate(()=>document.body.classList.contains('sidebar-closed')));await page.locator('#toggleSidebar').click();
    await page.evaluate(()=>{Workspace.selectPanel(19);Workspace.selectPanel(37);Workspace.selectPanel(43);});await ready();check('rapid panel changes keep newest dimensions',await page.evaluate(()=>Workspace.profile.id===43&&Workspace.frame.width===792&&Workspace.frame.height===528));
    await page.evaluate(async()=>{const blob=await(await fetch('/assets/photo.jpg')).blob();Workspace.load(blob,'loading-race.jpg');Workspace.selectPanel(47);});await ready();check('panel change during file decode retains image',await page.evaluate(()=>Workspace.profile.id===47&&Workspace.image.width===1280&&Workspace.frame.width===128&&Workspace.frame.height===250));
    await page.locator('#panelSelect').selectOption('34');await ready();
    await input('param-brightness',11);await ready();await page.locator('#panelSelect').selectOption('37');await ready();await input('param-brightness',6);await ready();await page.locator('#panelSelect').selectOption('34');await ready();
    check('panel settings stay independent',await page.evaluate(()=>Workspace.state.params.color.brightness===11));
    const workerFrame=await frame();await page.evaluate(()=>{Workspace.stop();Workspace.failedWorker=true;Workspace.request();});await ready();check('worker fallback matches output',(await frame())===workerFrame);
    await page.evaluate(()=>{Workspace.stop();Workspace.failedWorker=false;Workspace.request();});await ready();
    await page.evaluate(()=>{
      const original=window.fetch;window.testFetchOriginal=original;
      window.fetch=async(input,init)=>{
        if(String(input).startsWith('/api/mock-upload')){const bitmap=await createImageBitmap(init.body),c=document.createElement('canvas');c.width=bitmap.width;c.height=bitmap.height;c.getContext('2d').drawImage(bitmap,0,0);bitmap.close();window.testPostedPng=c.toDataURL();}
        return original(input,init);
      };
    });
    const postBefore=requests.filter(r=>r.method==='POST').length,png=await frame();await page.locator('#upload').click();await page.waitForFunction(()=>!Workspace.busy&&Workspace.notice.includes('模拟完成'));
    const posts=requests.filter(r=>r.method==='POST');check('only mock upload is used',posts.length===postBefore+1&&posts.at(-1).url.startsWith(origin+'/api/mock-upload'));
    const sent=await page.evaluate(()=>{window.fetch=window.testFetchOriginal;delete window.testFetchOriginal;const png=window.testPostedPng;delete window.testPostedPng;return png;});check('mock receives native final frame',sent===png);
    check('real device routes rejected',(await page.request.post(origin+'/upload',{data:'x'})).status()===405&&(await page.request.post(origin+'/panel_config',{data:'x'})).status()===405);
    check('non-allowlisted workspace files are private',(await page.request.get(origin+'/main/epd_stub.c')).status()===404);
    check('invalid mock upload rejected',(await page.request.post(origin+'/api/mock-upload?panel=34',{data:'not png'})).status()===400);
    await page.evaluate(()=>{Workspace.resizeSidebar(300);Workspace.setTab('adjust');});await page.locator('#reset').click();await ready();
    for(const [width,height] of [[1440,900],[1280,720],[1024,600],[390,844],[360,800],[844,390]]){
      await page.setViewportSize({width,height});await page.evaluate(()=>Workspace.paint());
      const layout=await page.evaluate(()=>{
        const v=document.getElementById('viewport').getBoundingClientRect(),p=document.getElementById('parameterScroll').getBoundingClientRect(),b=document.getElementById('param-brightness').getBoundingClientRect();
        return {bodyOverflow:document.documentElement.scrollWidth>innerWidth||document.documentElement.scrollHeight>innerHeight,viewport:{w:v.width,h:v.height},both:v.height>=100&&v.width>=130&&b.top>=0&&b.bottom<=innerHeight,overlap:Math.min(v.right,p.right)>Math.max(v.left,p.left)&&Math.min(v.bottom,p.bottom)>Math.max(v.top,p.top),icons:[...document.querySelectorAll('img')].filter(i=>getComputedStyle(i).display!=='none').every(i=>i.complete&&i.naturalWidth>0)};
      });
      check(width+'x'+height+' usable workspace',!layout.bodyOverflow&&layout.both&&!layout.overlap&&layout.icons,layout);
      await page.screenshot({path:'output/workspace-prototype/workspace-'+width+'x'+height+'.png'});
    }
    await page.setViewportSize({width:1440,height:900});
    for(const id of [47,19,20,10,37,44,43]){await page.locator('#panelSelect').selectOption(String(id));await ready();await page.screenshot({path:'output/workspace-prototype/panel-'+id+'.png'});}
    check('no browser exceptions',errors.length===0,{errors});
    check('browser never calls nonlocal servers',requests.every(r=>r.url.startsWith(origin)||r.url.startsWith('data:')||r.url.startsWith('blob:')));
    const report={finishedAt:new Date().toISOString(),phase:1,deviceAccess:false,checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass)};
    await download('regression-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));
    if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed,profiles:profiles.length};
  }finally{
    await page.evaluate(saved=>{if(window.testFetchOriginal){window.fetch=window.testFetchOriginal;delete window.testFetchOriginal;}delete window.testPostedPng;Workspace.stop();if(saved===null)localStorage.removeItem(Workspace.key);else localStorage.setItem(Workspace.key,saved);},saved);
    page.removeListener('pageerror',onError);page.removeListener('request',onRequest);
  }
}
