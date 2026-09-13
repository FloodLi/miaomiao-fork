// Local optional photo fixture: output/playwright/preview-photo-input.jpg.
// playwright-cli -s=jd75-large run-code --filename test/web/preview_photo_regression.js
async(page)=>{
  const origin='http://127.0.0.1:8765',checks=[],errors=[];let uploaded=null;
  const check=(name,pass,metrics={})=>checks.push({name,pass,...metrics});
  const route=async r=>{
    const req=r.request(),path=req.url().slice(origin.length).split('?')[0];
    if(!req.url().startsWith(origin+'/'))return r.abort();
    if(req.method()==='POST'){
      if(path!=='/upload')return r.abort();
      uploaded=req.postDataBuffer().toString('base64');return r.fulfill({status:200,body:'mock only'});
    }
    if(path==='/status')return r.fulfill({json:{panel:34,panel_width:800,panel_height:480,panel_has_red:true,panel_has_yellow:true,panel_name:'JD79665',version:'preview-test',spiffs_ok:true,spiffs_total:1048576,spiffs_used:0}});
    if(path==='/images')return r.fulfill({json:{items:[]}});
    if(path==='/wifi_status')return r.fulfill({json:{mode:3,sta_connected:false}});
    if(path==='/'||/\.(html|js|png|ico)$/.test(path))return r.continue();
    return r.fulfill({json:{}});
  };
  const save=async(name,data)=>{
    const wait=page.waitForEvent('download');
    await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});
    await(await wait).saveAs('output/playwright/'+name);
  };
  await page.route('**/*',route);await page.goto(origin+'/index.html');
  const saved=await page.evaluate(()=>Object.fromEntries(Object.entries(localStorage)));
  const error=e=>errors.push(String(e));page.on('pageerror',error);
  try{
    await page.evaluate(()=>{localStorage.setItem('epd_onboard_done','1');localStorage.removeItem('upload_img_opts_panel_34');});
    await page.reload();await page.evaluate(async()=>{await refresh();});
    await page.locator('#f').setInputFiles('output/playwright/preview-photo-input.jpg');
    await page.waitForFunction(()=>previewImg&&previewImg.width>0);
    await page.locator('[data-preset="colorphoto"]').click();
    const native=await page.evaluate(()=>document.getElementById('epdPreview').toDataURL());
    await save('photo-panel-pixels.png',native);
    for(const [width,height] of [[1440,900],[390,844],[844,390]]){
      await page.setViewportSize({width,height});
      const fit=await page.evaluate(()=>{
        paintFittedPreview();const c=document.getElementById('epdPreviewFit'),stage=c.parentElement,controls=document.querySelector('.preview-controls');
        return {width:c.getBoundingClientRect().width,stageWidth:stage.clientWidth,aboveControls:stage.getBoundingClientRect().bottom<=controls.getBoundingClientRect().top,overflow:document.documentElement.scrollWidth>innerWidth,data:c.toDataURL(),native:document.getElementById('epdPreview').toDataURL()};
      });
      check(width+' wide preview above controls',fit.aboveControls&&!fit.overflow&&fit.width>=(width===1440?790:width===390?280:650),{width:fit.width});
      check(width+' resize leaves upload untouched',fit.native===native);
      await save('photo-fit-'+width+'.png',fit.data);
      await page.screenshot({path:'output/playwright/photo-page-'+width+'.png',fullPage:true});
      await page.locator('#previewExpand').click();
      await page.evaluate(()=>paintFittedPreview());
      await page.screenshot({path:'output/playwright/photo-expanded-'+width+'.png'});
      check(width+' modal retains native image',await page.evaluate(expected=>document.getElementById('epdPreview').toDataURL()===expected,native));
      await page.locator('#previewViewSeg [data-view="pixels"]').click();
      await page.keyboard.press('Escape');
      await page.locator('#previewViewSeg [data-view="fit"]').click();
    }
    await page.evaluate(async()=>{await upload();});
    check('upload captured only by local mock',!!uploaded);
    const sent=await page.evaluate(async({base64,native})=>{
      const load=async data=>{const im=new Image();im.src=data;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;c.getContext('2d').drawImage(im,0,0);return {w:c.width,h:c.height,p:c.getContext('2d').getImageData(0,0,c.width,c.height).data};};
      const a=await load(native),b=await load('data:image/png;base64,'+base64);let differences=0;
      for(let i=0;i<a.p.length;i++)if(a.p[i]!==b.p[i])differences++;
      return {width:b.w,height:b.h,differences};
    },{base64:uploaded,native});
    check('actual POST is native 800x480, not fitted preview',sent.width===800&&sent.height===480&&sent.differences===0,sent);
    check('no browser exceptions',errors.length===0,{errors});
    const report={finishedAt:new Date().toISOString(),checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass)};
    await save('preview-photo-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));
    if(report.failures.length)throw new Error(JSON.stringify(report.failures));
    return report;
  }finally{
    await page.evaluate(saved=>{
      if(document.getElementById('previewDialog').open)document.getElementById('previewDialog').close();
      Object.keys(localStorage).filter(k=>k.startsWith('upload_img_opts')||k==='epd_onboard_done').forEach(k=>localStorage.removeItem(k));
      Object.entries(saved).filter(([k])=>k.startsWith('upload_img_opts')||k==='epd_onboard_done').forEach(([k,v])=>localStorage.setItem(k,v));
    },saved);
    page.removeListener('pageerror',error);await page.unroute('**/*',route);
  }
}
