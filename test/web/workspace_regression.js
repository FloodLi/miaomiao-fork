// Run through playwright-cli against workspace_server.cjs. All device writes are intercepted.
async(page)=>{
  await page.setViewportSize({width:1440,height:900});
  const origin=await page.evaluate(()=>location.origin);
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local test server required');
  const profiles=await(await page.request.get(origin+'/profiles.json')).json();
  let status=profiles.find(p=>p.panel===34),posts=[],failUpload=false,offline=false;
  const checks=[],errors=[],requests=[];
  const check=(name,pass,extra={})=>checks.push({name,pass,...extra});
  const intercept=async route=>{
    const req=route.request(),url={pathname:req.url().slice(origin.length).split('?')[0]};
    if(!req.url().startsWith(origin+'/'))return route.abort();
    if(req.method()==='POST'){
      if(url.pathname!=='/upload')throw Error('Unexpected device mutation');
      posts.push({png:req.postDataBuffer().toString('base64'),headers:req.headers(),panel:status.panel});
      return route.fulfill({status:failUpload?500:200,body:failUpload?'injected refresh failure':'OK'});
    }
    if(url.pathname==='/status')return route.fulfill({status:offline?503:200,json:offline?{error:'offline'}:status});
    if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
    return route.continue();
  };
  const ready=()=>page.waitForFunction(()=>Workspace.image&&!Workspace.loading&&Workspace.ready===Workspace.revision&&!Workspace.error,{},{timeout:30000});
  const frame=()=>page.evaluate(()=>Workspace.frame.toDataURL());
  const save=async(name,data)=>{const pending=page.waitForEvent('download');await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});await(await pending).saveAs('output/web-tests/workspace/'+name);};
  const slider=async(id,value,commit=true)=>page.locator('#param-'+id).evaluate((el,{value,commit})=>{el.value=value;el.dispatchEvent(new Event('input'));if(commit)el.dispatchEvent(new Event('change'));},{value,commit});
  const changePanel=async id=>{status=profiles.find(p=>p.panel===id);await page.evaluate(()=>Workspace.readStatus());await ready();};
  const onError=e=>errors.push(String(e)),onRequest=r=>requests.push(r.url());
  page.on('pageerror',onError);page.on('request',onRequest);await page.route('**/*',intercept);
  await page.goto(origin);const saved=await page.evaluate(()=>Object.fromEntries(Object.entries(localStorage)));
  try{
    await page.evaluate(()=>{localStorage.clear();localStorage.setItem('epd_onboard_done','1');localStorage.setItem('epd_auth_u','test');localStorage.setItem('epd_auth_p','local-only');});
    await page.reload();await page.locator('#f').setInputFiles('test/web/fixtures/reference.png');await ready();
    check('selection enters isolated full workspace',await page.evaluate(()=>Workspace.opened&&document.querySelector('.wrap').inert&&!!document.getElementById('imageWorkspace').shadowRoot&&!document.getElementById('previewDialog')));
    check('ID34 neutral tuning keeps candidate protection',await page.evaluate(()=>Workspace.profile.id===34&&Workspace.state.params.color.neutralProtection===35&&Workspace.state.params.color.contrast===100&&Workspace.state.params.color.sharp===0));
    const golden=await frame();
    for(const s of profiles){
      await changePanel(s.panel);
      const m=await page.evaluate(()=>{const p=Workspace.profile,d=Workspace.frame.getContext('2d').getImageData(0,0,p.width,p.height).data,allowed=ImageProcessor.profilePalette(p).map(c=>c.join(','));let illegal=0;for(let i=0;i<d.length;i+=4)if(!allowed.includes([d[i],d[i+1],d[i+2]].join(','))||d[i+3]!==255)illegal++;return {width:Workspace.frame.width,height:Workspace.frame.height,illegal,profile:p.profileId,colorHidden:document.getElementById('imageWorkspace').shadowRoot.querySelector('[data-mode="color"]').hidden};});
      check('panel '+s.panel+' actual geometry and inks',m.width===s.panel_width&&m.height===s.panel_height&&!m.illegal&&m.colorHidden===(s.image_profile.color_type==='bw'),m);
      if([47,19,20,10,34,37,44,43].includes(s.panel)){
        const expected=await frame();await page.locator('#upload').click();await page.waitForFunction(()=>!Workspace.busy&&!Workspace.encoding&&Workspace.notice.includes('上传完成'));
        check('panel '+s.panel+' native PNG POST matches preview',await page.evaluate(async({png,expected,w,h})=>{const im=new Image();im.src='data:image/png;base64,'+png;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;c.getContext('2d').drawImage(im,0,0);return c.width===w&&c.height===h&&c.toDataURL()===expected;},{png:posts.at(-1).png,expected,w:s.panel_width,h:s.panel_height}));
      }
    }
    await changePanel(34);check('panel switching preserves accepted ID34 image',await frame()===golden);
    await save('photo-id34.png',await frame());await changePanel(37);await save('photo-id37.png',await frame());
    check('ID37 correction is independent',await page.evaluate(()=>Workspace.state.params.color.neutralProtection===0));
    const base=await frame(),hist=await page.evaluate(()=>Workspace.history.length);
    await slider('brightness',7,false);await slider('brightness',12,false);await slider('brightness',19);await ready();
    check('slider gesture adds one history entry and changes image',await frame()!==base&&await page.evaluate(h=>Workspace.history.length===h+1,hist));
    await page.locator('#undo').click();await ready();check('undo restores exact frame',await frame()===base);
    await page.locator('#redo').click();await ready();check('redo restores tuning',await page.evaluate(()=>Workspace.state.params.color.brightness===19));
    await page.locator('#reset').click();await ready();
    const state=await page.evaluate(()=>JSON.stringify(Workspace.state));
    for(const [theme,fill] of [['loopy','rgb(255, 230, 238)'],['kitty','rgb(255, 237, 241)'],['lotso','rgb(248, 232, 239)']]){await page.evaluate(theme=>setTheme(theme),theme);check(theme+' adapts controls without tinting canvas or output',await page.evaluate(({state,base,fill})=>{const root=document.getElementById('imageWorkspace').shadowRoot;return getComputedStyle(root.getElementById('editorShell')).backgroundColor==='rgb(255, 255, 255)'&&getComputedStyle(root.querySelector('.canvas-area')).backgroundColor==='rgb(233, 235, 237)'&&getComputedStyle(root.getElementById('upload')).backgroundColor===fill&&JSON.stringify(Workspace.state)===state&&Workspace.frame.toDataURL()===base;},{state,base,fill}));}
    await page.locator('[data-tab="compose"]').click();await page.locator('#fitMode').selectOption('cover');
    await page.locator('#cropZoom').evaluate(el=>{el.value=2;el.dispatchEvent(new Event('input'));el.dispatchEvent(new Event('change'));});await ready();
    const before=await page.evaluate(()=>Workspace.state.composition.cx),r=await page.locator('#display').boundingBox();
    await page.mouse.move(r.x+r.width*.6,r.y+r.height*.5);await page.mouse.down();await page.mouse.move(r.x+r.width*.4,r.y+r.height*.5,{steps:5});await page.mouse.up();await ready();
    check('crop drag moves constrained center',await page.evaluate(cx=>Workspace.state.composition.cx!==cx,before));
    for(const rot of [90,180,270,0]){await page.locator('[data-rot="'+rot+'"]').click();await ready();check('rotation '+rot+' constrains crop',await page.evaluate(()=>{const p=Workspace.profile,c=ImageProcessor.crop(Workspace.image.width,Workspace.image.height,p.width,p.height,Workspace.state.composition);return c.cx*c.sw-c.cw/2>=-.001&&c.cx*c.sw+c.cw/2<=c.sw+.001&&c.cy*c.sh-c.ch/2>=-.001&&c.cy*c.sh+c.ch/2<=c.sh+.001;}));}
    const crop=await frame();await page.locator('#flipX').check();await ready();check('mirror changes crop pixels',await frame()!==crop);
    const untouched=await frame(),params=await page.evaluate(()=>JSON.stringify(Workspace.state)),eye=await page.locator('#compare').boundingBox();
    await page.mouse.move(eye.x+10,eye.y+10);await page.mouse.down();check('hold original uses same composition',await page.evaluate(()=>Workspace.compare));await page.mouse.up();
    await page.locator('#actualView').click();await page.locator('#zoomSelect').selectOption('200');await page.locator('#fitView').click();
    check('view and comparison never change upload image',await frame()===untouched&&await page.evaluate(s=>JSON.stringify(Workspace.state)===s,params));
    await page.locator('#reset').click();await ready();await page.locator('[data-tab="adjust"]').click();
    for(const mode of ['mono','text','color']){await page.locator('[data-mode="'+mode+'"]').click();await ready();check(mode+' exposes only used controls',await page.evaluate(mode=>Workspace.supports('threshold')===(mode==='text')&&Workspace.supports('saturation')===(mode==='color')&&Workspace.supports('dither')===(mode!=='text'),mode));}
    const queue=await page.evaluate(()=>{for(let n=1;n<=9;n++)Workspace.change(()=>{Workspace.state.params.color.brightness=n;},n===9);const job=Workspace.job;Workspace.complete({...job,epoch:job.epoch-1,buffer:new ArrayBuffer(job.width*job.height*4)});return Workspace.job===job&&Workspace.pending.revision===Workspace.revision&&document.getElementById('imageWorkspace').shadowRoot.getElementById('upload').disabled;});check('latest-only queue rejects stale completion',queue);await ready();
    const native=await frame();await page.evaluate(()=>{Workspace.stop();Workspace.failedWorker=true;Workspace.request();});await ready();check('fallback uses identical processor',await frame()===native);
    await page.evaluate(()=>{window.processOriginal=ImageProcessor.processProfile;ImageProcessor.processProfile=()=>{throw Error('injected processor fault');};Workspace.request();});await page.waitForFunction(()=>Workspace.error.includes('injected'));check('processor failure disables upload',await page.locator('#upload').isDisabled());
    await page.evaluate(()=>{ImageProcessor.processProfile=window.processOriginal;delete window.processOriginal;Workspace.stop();Workspace.failedWorker=false;Workspace.request();});await ready();
    const postedFrame=await frame();await page.locator('#upload').click();await page.waitForFunction(()=>!Workspace.busy&&!Workspace.encoding&&Workspace.notice.includes('上传完成'));
    check('POST carries existing auth',posts.at(-1).headers.authorization==='Basic dGVzdDpsb2NhbC1vbmx5');
    check('POST pixels equal latest native result',await page.evaluate(async({png,expected})=>{const im=new Image();im.src='data:image/png;base64,'+png;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;c.getContext('2d').drawImage(im,0,0);return c.width===400&&c.height===300&&c.toDataURL()===expected;},{png:posts.at(-1).png,expected:postedFrame}));
    failUpload=true;await page.locator('#upload').click();await page.waitForFunction(()=>!Workspace.encoding&&Workspace.notice.includes('injected refresh'));check('HTTP error unlocks tuning',await page.locator('#param-brightness').isEnabled());failUpload=false;
    let count=posts.length;await page.evaluate(()=>{window.encodeOriginal=Workspace.frame.toBlob;Workspace.frame.toBlob=function(cb,type){window.encodeOriginal.call(this,blob=>{Workspace.change(()=>{Workspace.state.params.color.brightness=1;},true);cb(blob);},type);};});await page.evaluate(()=>Workspace.upload());check('encoding race cannot POST old image',posts.length===count);await page.evaluate(()=>{Workspace.frame.toBlob=window.encodeOriginal;delete window.encodeOriginal;});await ready();
    status=profiles.find(p=>p.panel===43);count=posts.length;await page.evaluate(()=>Workspace.upload());await ready();check('preflight panel change aborts old-size POST',posts.length===count&&await page.evaluate(()=>Workspace.profile.id===43&&Workspace.frame.width===792));
    await changePanel(37);await page.locator('#reset').click();await ready();const valid=status;
    for(const invalid of [{...valid,image_profile:null},{...valid,panel_width:0},{...valid,panel:0,image_profile:{id:'e6-v1',color_type:'e6',inks:['black','white','red','yellow','blue','green']}}]){status=invalid;await page.evaluate(()=>Workspace.readStatus().catch(()=>{}));check('invalid capability blocks upload',await page.locator('#upload').isDisabled());}
    status=valid;await page.evaluate(()=>Workspace.readStatus());await ready();offline=true;await page.evaluate(()=>Workspace.readStatus().catch(()=>{}));check('connection failure disables upload',await page.locator('#upload').isDisabled());offline=false;await page.evaluate(()=>Workspace.readStatus());await ready();
    const keep=await frame();await page.keyboard.press('Escape');check('escape returns without resetting editor',await page.evaluate(()=>!Workspace.opened&&!document.querySelector('.wrap').inert));await page.locator('#up').click();await ready();check('reopen keeps original and frame',await frame()===keep);
    for(const [width,height] of [[1440,900],[1280,720],[1024,600],[390,844],[360,800],[844,390]]){
      await page.setViewportSize({width,height});await page.evaluate(()=>Workspace.paint());const m=await page.evaluate(()=>{const root=document.getElementById('imageWorkspace').shadowRoot,v=root.getElementById('viewport').getBoundingClientRect(),b=root.getElementById('param-brightness').getBoundingClientRect(),shell=root.getElementById('editorShell'),u=root.getElementById('upload').getBoundingClientRect(),main=root.getElementById('workspace').getBoundingClientRect();return {overflow:shell.scrollWidth>innerWidth||shell.scrollHeight>innerHeight,both:v.width>120&&v.height>100&&b.top>=0&&b.bottom<=innerHeight,commandsFit:u.bottom<=main.top,icons:[...root.querySelectorAll('img')].every(i=>i.complete&&i.naturalWidth>0)};});check(width+'x'+height+' preview and controls visible',!m.overflow&&m.both&&m.icons&&m.commandsFit,m);await page.locator('#upload').click({trial:true});await page.screenshot({path:'output/web-tests/workspace/workspace-'+width+'x'+height+'.png'});
    }
    await page.setViewportSize({width:1440,height:900});
    await page.locator('#file').setInputFiles('test/web/fixtures/invalid.png');await page.waitForFunction(()=>!!Workspace.error);check('bad file blocks old result',await page.evaluate(()=>!Workspace.image&&Workspace.rendered===-1));
    await page.locator('#file').setInputFiles('test/web/fixtures/qr.png');await ready();await page.locator('[data-mode="text"]').click();await ready();check('native QR text preserves pixels',await page.evaluate(()=>Workspace.frame.toDataURL()===Workspace.original.toDataURL()));
    await page.evaluate(()=>{Workspace.close();localStorage.removeItem('upload_editor_v4_panel_37');localStorage.setItem('upload_img_opts_panel_37',JSON.stringify({preset:'custom',brightness:12,contrast:113,saturation:88,rot:90,fit:'cover'}));});
    await page.reload();await page.locator('#up').click();await page.waitForFunction(()=>Workspace.profile?.id===37);
    check('custom migration imports tone but not old crop',await page.evaluate(()=>Workspace.state.params.color.brightness===12&&Workspace.state.params.color.contrast===113&&Workspace.state.composition.rot===0&&Workspace.state.composition.fit==='contain'&&JSON.parse(localStorage.getItem('upload_img_opts_panel_37')).rot===90));
    await page.evaluate(()=>{localStorage.removeItem('upload_editor_v4_panel_37');localStorage.setItem('upload_img_opts_panel_37',JSON.stringify({preset:'photo',brightness:25,contrast:180}));});await page.reload();await page.locator('#up').click();await page.waitForFunction(()=>Workspace.profile?.id===37);
    check('named legacy preset migrates to neutral mode',await page.evaluate(()=>Workspace.state.mode==='color'&&Workspace.state.params.color.brightness===0&&Workspace.state.params.color.contrast===100));
    await page.evaluate(()=>{Workspace.close();localStorage.setItem('upload_editor_v3_panel_34',JSON.stringify({version:3,mode:'mono',profiles:{mono:{brightness:9,contrast:111}}}));localStorage.removeItem('upload_editor_v4_panel_34');});
    status=profiles.find(p=>p.panel===34);await page.reload();await page.locator('#up').click();await page.waitForFunction(()=>Workspace.profile?.id===34);
    check('v3 mode and tone migrate without overwriting source',await page.evaluate(()=>Workspace.state.mode==='mono'&&Workspace.state.params.mono.brightness===9&&!!localStorage.getItem('upload_editor_v3_panel_34')));
    await page.evaluate(async()=>{
      window.RealImage=window.Image;window.delayedLoad=null;let first=true;
      window.Image=function(){const img=new window.RealImage();const delay=first;first=false;Object.defineProperty(img,'onload',{set(fn){img.addEventListener('load',()=>{if(delay)window.delayedLoad=()=>fn.call(img);else fn.call(img);});}});return img;};
      const make=async(w,h,name)=>{const c=document.createElement('canvas');c.width=w;c.height=h;c.getContext('2d').fillRect(0,0,w,h);const blob=await new Promise(resolve=>c.toBlob(resolve));Workspace.load(blob,name);};
      await make(320,240,'slow.png');await make(480,320,'latest.png');
    });await ready();await page.waitForFunction(()=>!!window.delayedLoad);
    const newest=await frame();await page.evaluate(()=>{window.delayedLoad();window.Image=window.RealImage;delete window.RealImage;delete window.delayedLoad;});
    check('late decoded file cannot replace latest image',await frame()===newest&&await page.evaluate(()=>Workspace.fileName==='latest.png'&&Workspace.image.width===480));
    const slowStatus=async route=>{await page.waitForTimeout(150);await route.fulfill({json:profiles.find(p=>p.panel===34)});};
    await page.route('**/status',slowStatus,{times:1});await page.evaluate(()=>{window.oldStatus=Workspace.readStatus().catch(()=>{});});
    status=profiles.find(p=>p.panel===37);await page.evaluate(()=>Workspace.readStatus());await page.evaluate(()=>window.oldStatus);await ready();await page.unroute('**/status',slowStatus);
    check('late status cannot roll panel back',await page.evaluate(()=>Workspace.profile.id===37&&Workspace.online&&Workspace.frame.width===400));
    const failedModule=route=>route.abort();await page.route('**/image_processor.js',failedModule);await page.reload();
    await page.evaluate(()=>refresh());
    check('missing processor visibly blocks upload entry',await page.evaluate(()=>Workspace.missing&&document.getElementById('up').disabled&&document.getElementById('f').disabled&&document.getElementById('log').textContent.includes('图片处理模块加载失败')));
    await page.unroute('**/image_processor.js',failedModule);
    check('no browser runtime errors',errors.length===0,{errors});check('no external browser traffic',requests.every(u=>u.startsWith(origin)||u.startsWith('data:')||u.startsWith('blob:')));
    const report={checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass),profiles:profiles.length};
    await save('web-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed,profiles:profiles.length};
  }finally{await page.evaluate(saved=>{Workspace.stop?.();localStorage.clear();for(const[k,v]of Object.entries(saved))localStorage.setItem(k,v);},saved);await page.unroute('**/*',intercept);page.removeListener('pageerror',onError);page.removeListener('request',onRequest);}
}
