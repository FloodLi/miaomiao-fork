// Exercise the production message page with isolated local status and upload responses.
async(page)=>{
  const origin=await page.evaluate(()=>location.origin),checks=[],posts=[],errors=[];
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local test server required');
  let status={panel:37,panel_width:400,panel_height:300,panel_has_red:true,panel_has_yellow:true},failUpload=false;
  const check=(name,pass,extra={})=>checks.push({name,pass,...extra});
  const intercept=async route=>{
    const req=route.request(),path=req.url().slice(origin.length).split('?')[0];
    if(!req.url().startsWith(origin+'/'))return route.abort();
    if(req.method()==='POST'){
      if(path!=='/msg_image_show')throw Error('Unexpected mutation: '+path);
      posts.push({png:req.postDataBuffer().toString('base64'),headers:req.headers()});return route.fulfill({status:failUpload?500:200,body:failUpload?'injected refresh failure':'OK'});
    }
    if(path==='/message.html')return route.fulfill({path:'web/message.html',contentType:'text/html'});
    if(path==='/status')return route.fulfill({json:status});
    if(path==='/msg_config')return route.fulfill({json:{text:'黄色留言测试\nHello ABC 123',color:0,align:1,font_size:0,x_offset:0,y_offset:0}});
    if(path==='/favicon.ico')return route.fulfill({status:204});
    return route.continue();
  };
  const save=async(name,data)=>{const wait=page.waitForEvent('download');await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});await(await wait).saveAs('output/web-tests/message/'+name);};
  const onError=e=>errors.push(String(e));page.on('pageerror',onError);await page.route('**/*',intercept);
  await page.setViewportSize({width:1440,height:900});await page.goto(origin+'/message.html');
  await page.waitForFunction(()=>document.getElementById('msgLog').textContent.includes('设置已加载'));
  const saved=await page.evaluate(()=>Object.fromEntries(Object.entries(localStorage)));
  const sample=async(color,expected,baseline=false)=>{
    await page.locator('#webColor').selectOption(color);
    return page.evaluate(({expected,baseline})=>{
      const c=document.getElementById('webMsgCanvas'),d=c.getContext('2d').getImageData(0,0,c.width,c.height).data,mask=new Uint8Array(c.width*c.height);let count=0,illegal=0,different=0;
      for(let i=0;i<mask.length;i++){const p=i*4,white=d[p]===255&&d[p+1]===255&&d[p+2]===255;mask[i]=white?0:1;if(!white){count++;if(d[p]!==expected[0]||d[p+1]!==expected[1]||d[p+2]!==expected[2])illegal++;}if(d[p+3]!==255)illegal++;if(!baseline&&mask[i]!==window.messageMask[i])different++;}
      if(baseline)window.messageMask=mask;return {width:c.width,height:c.height,count,illegal,different};
    },{expected,baseline});
  };
  try{
    const matrices=[{id:37,w:400,h:300,red:true,yellow:true,thresholds:[80,170,220]},{id:34,w:800,h:480,red:true,yellow:true,thresholds:[170]},{id:43,w:792,h:528,red:true,yellow:true,thresholds:[80,170,220]},{id:0,w:400,h:300,red:true,yellow:false,thresholds:[170]},{id:47,w:128,h:250,red:false,yellow:false,thresholds:[170]},{id:44,w:792,h:528,red:true,yellow:true,thresholds:[170]}];
    for(const p of matrices){
      status={panel:p.id,panel_width:p.w,panel_height:p.h,panel_has_red:p.red,panel_has_yellow:p.yellow};await page.evaluate(()=>loadStatusForPreview());
      await page.locator('#webFontPx').fill(p.id===47?'24':'48');let previous=0;
      for(const threshold of p.thresholds){
        await page.locator('#webThreshold').evaluate((el,v)=>{el.value=v;el.dispatchEvent(new Event('input'));},threshold);
        const black=await sample('black',[0,0,0],true);check('panel '+p.id+' threshold '+threshold+' baseline has strokes',black.count>50&&!black.illegal&&black.count>=previous,{count:black.count});previous=black.count;
        for(const color of ['red','yellow','blue','green']){
          const ink=color==='red'&&p.red?[196,36,28]:color==='yellow'&&p.yellow?[255,232,0]:color==='blue'&&p.id===43?[20,80,185]:color==='green'&&p.id===43?[40,150,90]:[0,0,0];
          const m=await sample(color,ink);check('panel '+p.id+' threshold '+threshold+' '+color+' preserves glyph mask',m.count===black.count&&m.different===0&&m.illegal===0&&m.width===p.w&&m.height===p.h,m);
        }
      }
    }
    check('same geometry does not enable E6',await page.evaluate(()=>epdPanelW===792&&epdPanelH===528&&!epdPanelIsE6));
    status={panel:37,panel_width:400,panel_height:300,panel_has_red:true,panel_has_yellow:true};await page.evaluate(()=>loadStatusForPreview());
    await page.locator('#webThreshold').evaluate(el=>{el.value=170;el.dispatchEvent(new Event('input'));});
    await page.locator('#webColor').selectOption('yellow');
    const native=await page.evaluate(()=>document.getElementById('webMsgCanvas').toDataURL());await save('yellow-message.png',native);
    await page.evaluate(()=>{localStorage.setItem('epd_auth_u','test');localStorage.setItem('epd_auth_p','local-only');});
    await page.locator('#webShowBtn').click();await page.waitForFunction(()=>!document.getElementById('webShowBtn').disabled);
    check('yellow PNG POST matches preview exactly',posts.length===1&&await page.evaluate(async({png,native})=>{const im=new Image();im.src='data:image/png;base64,'+png;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;c.getContext('2d').drawImage(im,0,0);return c.width===400&&c.height===300&&c.toDataURL()===native;},{png:posts[0].png,native}));
    check('message upload retains authentication',posts[0].headers.authorization==='Basic dGVzdDpsb2NhbC1vbmx5');
    failUpload=true;await page.locator('#webShowBtn').click();await page.waitForFunction(()=>!document.getElementById('webShowBtn').disabled);check('failed upload reports failure and unlocks retry',await page.locator('#msgLog').textContent().then(t=>t.includes('injected refresh failure')));failUpload=false;
    for(const theme of ['loopy','kitty','lotso']){await page.evaluate(theme=>{document.documentElement.dataset.theme=theme;renderWebMessage();},theme);check(theme+' UI theme cannot change yellow text',await page.evaluate(native=>document.getElementById('webMsgCanvas').toDataURL()===native,native));}
    for(const[width,height]of[[1440,900],[390,844],[360,800]]){
      await page.setViewportSize({width,height});check(width+' message layout remains usable',await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.screenshot({path:'output/web-tests/message/message-'+width+'.png',fullPage:true});
    }
    await page.locator('#msgText').fill('   ');const n=posts.length;await page.locator('#webShowBtn').click();check('empty text cannot upload placeholder',posts.length===n);
    check('no runtime errors',errors.length===0,{errors});
    const report={checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass)};await save('web-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed};
  }finally{await page.evaluate(saved=>{delete window.messageMask;localStorage.clear();for(const[k,v]of Object.entries(saved))localStorage.setItem(k,v);},saved);await page.unroute('**/*',intercept);page.removeListener('pageerror',onError);}
}
