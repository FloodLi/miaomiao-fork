// playwright-cli -s=editor-v3 run-code --filename test/web/editor_regression.js
async(page)=>{
  const origin='http://127.0.0.1:8765',checks=[],errors=[];let panel=34,posts=[],failUpload=false;
  const check=(name,pass,extra={})=>{checks.push({name,pass,...extra});};
  const intercept=async route=>{
    const req=route.request(),path=req.url().slice(origin.length).split('?')[0];
    if(!req.url().startsWith(origin+'/'))return route.abort();
    if(req.method()==='POST'){
      if(path!=='/upload')return route.abort();posts.push(req.postDataBuffer().toString('base64'));
      return route.fulfill({status:failUpload?503:200,body:failUpload?'injected failure':'mock upload'});
    }
    if(path==='/status')return route.fulfill({json:{panel,panel_width:panel===34?800:400,panel_height:panel===34?480:300,panel_has_red:true,panel_has_yellow:true,panel_name:'editor test',version:'local-test',spiffs_ok:true,spiffs_total:1048576,spiffs_used:0}});
    if(path==='/images')return route.fulfill({json:{items:[]}});
    if(path==='/wifi_status')return route.fulfill({json:{mode:3,sta_connected:false}});
    if(path==='/'||/\.(html|js|png|ico)$/.test(path))return route.continue();
    return route.fulfill({json:{}});
  };
  const download=async(name,data)=>{
    const wait=page.waitForEvent('download');await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});
    await(await wait).saveAs('output/playwright/'+name);
  };
  const ready=()=>page.waitForFunction(()=>Editor.img&&Editor.ready===Editor.revision&&!Editor.error,{},{timeout:20000});
  const result=()=>page.evaluate(()=>({mode:Editor.state.mode,state:Editor.clone(Editor.state),frame:Editor.frame.toDataURL(),revision:Editor.revision,ready:Editor.ready}));
  const input=async(id,value,commit=true)=>{
    await page.locator('#'+id).evaluate((el,{value,commit})=>{el.value=String(value);el.dispatchEvent(new Event('input',{bubbles:true}));if(commit)el.dispatchEvent(new Event('change',{bubbles:true}));},{value,commit});
  };
  await page.route('**/*',intercept);await page.goto(origin+'/index.html');
  const saved=await page.evaluate(()=>Object.fromEntries(Object.entries(localStorage)));
  const error=e=>errors.push(String(e));page.on('pageerror',error);
  try{
    await page.evaluate(()=>{localStorage.removeItem('upload_editor_v3_panel_34');localStorage.removeItem('upload_img_opts_panel_34');localStorage.setItem('epd_onboard_done','1');});
    await page.reload();await page.evaluate(async()=>{await refresh();});
    const fixture=await page.evaluate(()=>{
      const c=document.createElement('canvas');c.width=1280;c.height=960;const x=c.getContext('2d');
      for(const [color,a,b] of [['#ff0000',0,0],['#ffff00',640,0],['#000000',0,480],['#ffffff',640,480]]){x.fillStyle=color;x.fillRect(a,b,640,480);}
      x.fillStyle='#808080';x.fillRect(400,320,480,320);x.fillStyle='#fff';x.font='bold 40px sans-serif';x.fillText('EDITOR 1280 x 960',25,55);
      return c.toDataURL();
    });
    await download('editor-fixture.png',fixture);
    await page.locator('#f').setInputFiles('output/playwright/editor-fixture.png');await ready();
    check('ID34 uses integrated editor and three modes',await page.evaluate(()=>Editor.active&&document.querySelectorAll('#edMode button').length===3&&getComputedStyle(document.getElementById('previewWrap')).display==='none'));
    check('worker is active',await page.evaluate(()=>!!Editor.worker&&!Editor.workerFailed));
    const baseline=await result();
    for(const mode of ['mono','text','color']){
      await page.locator('#edMode [data-mode="'+mode+'"]').click();await ready();
      const m=await page.evaluate(()=>{
        const d=Editor.frame.getContext('2d').getImageData(0,0,800,480).data;let colored=0;
        for(let i=0;i<d.length;i+=4)if(d[i]!==d[i+1]||d[i]!==d[i+2])colored++;
        return {mode:Editor.state.mode,colored,comp:Editor.state.composition};
      });
      check(mode+' output palette',mode==='color'?m.colored>0:m.colored===0,m);
      check(mode+' switch preserves composition',JSON.stringify(m.comp)===JSON.stringify(baseline.state.composition));
    }
    const beforeHistory=await page.evaluate(()=>Editor.history.length);
    await input('ed-brightness',10,false);await input('ed-brightness',17,false);await input('ed-brightness',25);
    await ready();check('one slider gesture produces one undo step',await page.evaluate(n=>Editor.history.length===n+1,beforeHistory));
    await page.locator('#edUndo').click();await ready();check('undo restores full parameters and pixels',(await result()).frame===baseline.frame);
    await page.locator('#edRedo').click();await ready();check('redo restores slider',await page.evaluate(()=>Editor.state.profiles.color.brightness===25));
    await page.locator('#edMode [data-mode="mono"]').click();await ready();await input('ed-brightness',-11);await ready();
    await page.locator('#edMode [data-mode="color"]').click();await ready();check('mode adjustments are independent',await page.evaluate(()=>Editor.state.profiles.color.brightness===25&&Editor.state.profiles.mono.brightness===-11));
    await page.locator('[data-reset="brightness"]').click();await ready();check('individual reset is neutral',await page.evaluate(()=>Editor.state.profiles.color.brightness===0));
    await page.locator('#edFitMode').selectOption('cover');await ready();await input('edCropZoom',2);await ready();
    await page.locator('#edTool [data-tool="crop"]').click();
    const cropStart=await page.evaluate(()=>({c:Editor.clone(Editor.state.composition),history:Editor.history.length}));
    const canvasRect=await page.locator('#edCanvas').boundingBox();
    await page.mouse.move(canvasRect.x+canvasRect.width*.6,canvasRect.y+canvasRect.height*.5);await page.mouse.down();
    await page.mouse.move(canvasRect.x+canvasRect.width*.4,canvasRect.y+canvasRect.height*.5,{steps:6});await page.mouse.up();await ready();
    const cropped=await result();check('drag changes bounded crop center',cropped.state.composition.cx!==cropStart.c.cx&&cropped.state.composition.cx>=0&&cropped.state.composition.cx<=1);
    check('crop drag groups history',await page.evaluate(n=>Editor.history.length===n+1,cropStart.history));
    for(const angle of [90,180,270,0]){await page.locator('#edRotation [data-rot="'+angle+'"]').click();await ready();check('rotation '+angle+' stays in bounds',await page.evaluate(()=>{const c=ImageProcessor.crop(Editor.img.width,Editor.img.height,800,480,Editor.state.composition);return c.cx*c.sw-c.cw/2>=-.001&&c.cy*c.sh+c.ch/2<=c.sh+.001;}));}
    await page.locator('#edFlipX').check();await ready();check('mirror changes output',(await result()).frame!==cropped.frame);
    await page.locator('#edResetComposition').click();await ready();const reset=await result();
    const resetDiff=await page.evaluate(async({a,b})=>{
      const read=async url=>{const im=new Image();im.src=url;await im.decode();const c=document.createElement('canvas');c.width=800;c.height=480;c.getContext('2d').drawImage(im,0,0);return c.getContext('2d').getImageData(0,0,800,480).data;};
      const x=await read(a),y=await read(b);let count=0;const samples=[];for(let i=0;i<x.length;i+=4)if(x[i]!==y[i]||x[i+1]!==y[i+1]||x[i+2]!==y[i+2]){count++;if(samples.length<5)samples.push({x:i/4%800,y:Math.floor(i/4/800),before:[...x.slice(i,i+4)],after:[...y.slice(i,i+4)]});}return {count,samples};
    },{a:baseline.frame,b:reset.frame});
    check('composition reset restores neutral image',resetDiff.count===0,{before:baseline.state,after:reset.state,...resetDiff});
    const queue=await page.evaluate(()=>{
      const el=document.getElementById('ed-contrast');for(let n=100;n<=120;n++){el.value=n;el.dispatchEvent(new Event('input'));}el.dispatchEvent(new Event('change'));
      const job=Editor.inFlight,rev=Editor.revision;Editor.complete({epoch:Editor.epoch-1,revision:job.revision,buffer:new ArrayBuffer(800*480*4)});
      return {pending:Editor.pending?.revision===rev,inFlight:Editor.inFlight===job,disabled:document.getElementById('up').disabled};
    });check('latest-only queue, stale response ignored, upload blocked',queue.pending&&queue.inFlight&&queue.disabled,queue);
    const postCount=posts.length;await page.evaluate(()=>Editor.upload());check('pending frame cannot upload',posts.length===postCount);
    await ready();check('rapid input ends at latest parameters',await page.evaluate(()=>Editor.state.profiles.color.contrast===120&&Editor.ready===Editor.revision));
    const workerFrame=(await result()).frame;
    await page.evaluate(()=>{Editor.stop();Editor.workerFailed=false;const Original=window.Worker;window.Worker=function(){throw new Error('worker unavailable');};try{Editor.request();}finally{window.Worker=Original;}});await ready();
    check('fallback exactly matches worker',(await result()).frame===workerFrame);
    await page.evaluate(()=>{window.realProcessor=ImageProcessor.process;ImageProcessor.process=()=>{throw new Error('injected processing failure');};Editor.request();});
    await page.waitForFunction(()=>Editor.error.includes('injected'));check('processing failure blocks upload',await page.locator('#up').isDisabled());
    await page.evaluate(()=>{ImageProcessor.process=window.realProcessor;delete window.realProcessor;Editor.request();});await ready();
    await page.evaluate(()=>{Editor.stop();Editor.workerFailed=false;Editor.request();});await ready();
    const beforeUpload=await result();await page.locator('#up').click();await page.waitForFunction(()=>!Editor.uploading);
    check('real POST captured',posts.length===postCount+1);
    const decoded=await page.evaluate(async base64=>{const img=new Image();img.src='data:image/png;base64,'+base64;await img.decode();const c=document.createElement('canvas');c.width=img.width;c.height=img.height;c.getContext('2d').drawImage(img,0,0);return {width:c.width,height:c.height,data:c.toDataURL()};},posts.at(-1));
    check('POST is latest full-size frame',decoded.width===800&&decoded.height===480&&decoded.data===beforeUpload.frame);
    failUpload=true;await page.locator('#up').click();await page.waitForFunction(()=>!Editor.uploading);check('HTTP failure unlocks controls',await page.evaluate(()=>!document.getElementById('edControls').disabled&&document.getElementById('log').textContent.includes('injected failure')));failUpload=false;
    await page.evaluate(()=>{window.encodeOriginal=Editor.frame.toBlob;Editor.frame.toBlob=function(cb,type){window.encodeOriginal.call(this,blob=>{Editor.revision++;cb(blob);},type);};});
    const beforeStale=posts.length;await page.evaluate(()=>Editor.upload());check('parameter change during PNG encoding aborts POST',posts.length===beforeStale);
    await page.evaluate(()=>{Editor.frame.toBlob=window.encodeOriginal;delete window.encodeOriginal;Editor.request();});await ready();
    await download('editor-bad.png','data:application/octet-stream;base64,bm90IGEgcG5n');
    await page.locator('#f').setInputFiles('output/playwright/editor-bad.png');await page.waitForFunction(()=>!!Editor.error);
    check('bad file clears image and blocks upload',await page.evaluate(()=>!Editor.img&&Editor.rendered===-1&&document.getElementById('up').disabled));
    await page.locator('#f').setInputFiles('output/playwright/editor-fixture.png');await ready();
    check('new file resets crop and undo but preserves tone',await page.evaluate(()=>Editor.history.length===0&&Editor.state.composition.fit==='contain'&&Editor.state.profiles.color.contrast===120));
    await page.locator('#edResetTone').click();await ready();
    for(const [width,height] of [[1440,900],[1280,720],[390,844],[844,390]]){
      await page.setViewportSize({width,height});await page.locator('#jdEditor').scrollIntoViewIfNeeded();
      const layout=await page.evaluate(()=>{const v=document.getElementById('edViewport').getBoundingClientRect(),s=document.getElementById('ed-brightness').getBoundingClientRect(),c=document.getElementById('edControls'),r=document.getElementById('jdEditor').getBoundingClientRect();return {visible:v.width>150&&v.height>100&&v.top>=-1&&v.bottom<=innerHeight+1&&s.top>=0&&s.bottom<=innerHeight+1,overlap:Math.min(v.right,s.right)>Math.max(v.left,s.left)&&Math.min(v.bottom,s.bottom)>Math.max(v.top,s.top),overflow:document.documentElement.scrollWidth>innerWidth,sidebar:c.clientWidth,editorHeight:r.height};});
      check(width+' preview and common adjustment visible together',layout.visible&&!layout.overlap&&!layout.overflow,layout);
      await input('ed-brightness',5);await ready();await page.screenshot({path:'output/playwright/editor-'+width+'.png',fullPage:true});
      await page.locator('#edExpand').click();await input('ed-brightness',8);await ready();
      check(width+' expanded editor still contains controls and upload',await page.evaluate(()=>document.getElementById('previewDialog').contains(document.getElementById('up'))&&document.getElementById('previewDialog').contains(document.getElementById('ed-brightness'))&&Editor.state.profiles.color.brightness===8));
      await page.screenshot({path:'output/playwright/editor-expanded-'+width+'.png'});await page.keyboard.press('Escape');
      await page.waitForFunction(()=>!document.getElementById('previewDialog').open&&!document.body.classList.contains('preview-expanded')&&Editor.home.contains(Editor.root));
      check(width+' Escape restores editor',await page.evaluate(()=>!document.getElementById('previewDialog').open&&!document.body.classList.contains('preview-expanded')&&Editor.home.contains(Editor.root)));
      const a=(await result()).frame;await page.locator('#edActual').click();await input('edViewZoom',200);await page.locator('#edFit').click();check(width+' view zoom does not alter frame',(await result()).frame===a);
    }
    const geometry=await page.evaluate(()=>{
      const saved=Editor.clone(Editor.state.composition),results=[];
      try{
        for(const [rot,expected] of [[0,[[255,0,0],[255,255,0],[0,0,0],[255,255,255]]],[90,[[0,0,0],[255,0,0],[255,255,255],[255,255,0]]]]){
          Editor.state.composition=ImageProcessor.composition({rot});const im=Editor.source(),c=ImageProcessor.crop(Editor.img.width,Editor.img.height,800,480,Editor.state.composition);
          const samples=[[.1,.1],[.9,.1],[.1,.9],[.9,.9]].map(([x,y])=>{const p=(Math.floor(c.dy+c.dh*y)*800+Math.floor(c.dx+c.dw*x))*4;return [...im.data.slice(p,p+3)];});
          results.push({rot,pass:JSON.stringify(samples)===JSON.stringify(expected),samples});
        }
        Editor.state.composition=ImageProcessor.composition({fit:'cover',zoom:4,cx:.2,cy:.2});const d=Editor.source().data;let bad=0;
        for(let i=0;i<d.length;i+=4)if(d[i]!==255||d[i+1]!==0||d[i+2]!==0||d[i+3]!==255)bad++;results.push({crop:true,pass:bad===0,bad});
      }finally{Editor.state.composition=saved;}return results;
    });check('rendered crop and rotated coordinates match geometry',geometry.every(g=>g.pass),{geometry});
    const bounded=await page.evaluate(()=>{for(let i=0;i<60;i++)Editor.change(()=>{Editor.state.profiles.color.brightness=i%2?1:0;},true);return Editor.history.length;});
    await ready();check('undo history is bounded to 50 steps',bounded===50);
    const migration=await page.evaluate(()=>{
      const a=localStorage.getItem(Editor.key),b=localStorage.getItem('upload_img_opts_panel_34'),cases=[];
      try{localStorage.removeItem(Editor.key);for(const preset of ['photo','colorphoto','anime','soft','gray','text','custom']){localStorage.setItem('upload_img_opts_panel_34',JSON.stringify({preset,palette:'mono',brightness:17,contrast:123}));const s=Editor.load();cases.push({preset,mode:s.mode,brightness:s.profiles[s.mode].brightness,rawPreserved:JSON.parse(localStorage.getItem('upload_img_opts_panel_34')).preset===preset});}}finally{if(a===null)localStorage.removeItem(Editor.key);else localStorage.setItem(Editor.key,a);if(b===null)localStorage.removeItem('upload_img_opts_panel_34');else localStorage.setItem('upload_img_opts_panel_34',b);}return cases;
    });check('legacy migration retains raw data and imports custom only',migration.every(c=>c.rawPreserved&&c.mode===(c.preset==='text'?'text':['gray','custom'].includes(c.preset)?'mono':'color')&&c.brightness===(c.preset==='custom'?17:0)),{migration});
    panel=37;await page.evaluate(async()=>{await refresh();});check('ID37 retains legacy interface',await page.evaluate(()=>!Editor.active&&Editor.root.hidden&&getComputedStyle(document.getElementById('previewWrap')).display!=='none'&&document.querySelector('[data-preset="photo"]').hidden===false));
    panel=34;
    const missing=async route=>route.fulfill({status:503,body:'injected unavailable script'});
    await page.route('**/image_processor.js',missing);await page.reload();await page.evaluate(async()=>{await refresh();});
    const unavailable=await page.evaluate(()=>({missing:Editor.missing,disabled:document.getElementById('up').disabled,log:document.getElementById('log').textContent}));
    check('missing processor disables ID34 safely',unavailable.missing&&unavailable.disabled&&unavailable.log.includes('模块加载失败'),unavailable);
    await page.unroute('**/image_processor.js',missing);
    check('no browser exceptions',errors.length===0,{errors});
    const report={finishedAt:new Date().toISOString(),checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass)};
    await download('editor-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));
    if(report.failures.length)throw new Error(JSON.stringify(report.failures));return {passed:report.passed};
  }finally{
    await page.evaluate(saved=>{if(document.getElementById('previewDialog').open)document.getElementById('previewDialog').close();Editor.stop();Object.keys(localStorage).filter(k=>k.startsWith('upload_')||k==='epd_onboard_done').forEach(k=>localStorage.removeItem(k));Object.entries(saved).filter(([k])=>k.startsWith('upload_')||k==='epd_onboard_done').forEach(([k,v])=>localStorage.setItem(k,v));},saved);
    page.removeListener('pageerror',error);await page.unroute('**/*',intercept);
  }
}
