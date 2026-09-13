// Optional fixtures: preview-photo-input.jpg, editor-qr.png, jd75-color-source.png in output/playwright.
async(page)=>{
  const origin='http://127.0.0.1:8765',checks=[];
  const check=(name,pass,metrics={})=>checks.push({name,pass,...metrics});
  const route=async r=>{
    const req=r.request(),path=req.url().slice(origin.length).split('?')[0];
    if(req.method()!=='GET'||!req.url().startsWith(origin+'/'))return r.abort();
    if(path==='/status')return r.fulfill({json:{panel:34,panel_width:800,panel_height:480,panel_has_red:true,panel_has_yellow:true,panel_name:'JD79665',spiffs_ok:true,spiffs_total:1048576,spiffs_used:0}});
    if(path==='/images')return r.fulfill({json:{items:[]}});
    if(path==='/wifi_status')return r.fulfill({json:{mode:3,sta_connected:false}});
    if(path==='/'||/\.(html|js|png|ico)$/.test(path))return r.continue();return r.fulfill({json:{}});
  };
  await page.route('**/*',route);await page.goto(origin+'/index.html');
  const saved=await page.evaluate(()=>({editor:localStorage.getItem(Editor.key),onboard:localStorage.getItem('epd_onboard_done')}));
  const ready=()=>page.waitForFunction(()=>Editor.img&&Editor.ready===Editor.revision&&!Editor.error,{},{timeout:20000});
  const exportPng=async name=>{const wait=page.waitForEvent('download');await page.evaluate(name=>{const a=document.createElement('a');a.download=name;a.href=Editor.frame.toDataURL();a.click();},name);await(await wait).saveAs('output/playwright/'+name);};
  try{
    await page.evaluate(()=>localStorage.setItem('epd_onboard_done','1'));await page.reload();
    await page.evaluate(async()=>{await refresh();Editor.state=Editor.initial();Editor.controls();});
    await page.locator('#f').setInputFiles('output/playwright/preview-photo-input.jpg');await ready();
    for(const mode of ['color','mono','text']){
      await page.locator('#edMode [data-mode="'+mode+'"]').click();await ready();
      check('original photo '+mode+' ready',await page.evaluate(()=>Editor.img.width===1280&&Editor.img.height===960&&Editor.frame.width===800&&Editor.frame.height===480));
      await exportPng('editor-photo-'+mode+'.png');
    }
    await page.locator('#edMode [data-mode="color"]').click();await ready();
    for(const [width,height] of [[1440,900],[1280,720],[390,844],[844,390]]){
      await page.setViewportSize({width,height});await page.locator('#jdEditor').scrollIntoViewIfNeeded();
      await page.screenshot({path:'output/playwright/editor-photo-ui-'+width+'.png',fullPage:true});
      await page.locator('#edExpand').click();await page.screenshot({path:'output/playwright/editor-photo-expanded-'+width+'.png'});await page.keyboard.press('Escape');
      await page.waitForFunction(()=>!document.getElementById('previewDialog').open&&Editor.home.contains(Editor.root));
    }
    // Two asynchronous file selections must finish on the last one, not the slow photo.
    await page.locator('#f').setInputFiles('output/playwright/preview-photo-input.jpg');
    await page.locator('#f').setInputFiles('output/playwright/editor-qr.png');await ready();
    check('rapid file selection keeps last file',await page.evaluate(()=>Editor.file.name==='editor-qr.png'&&Editor.img.width===480));
    await page.locator('#edMode [data-mode="text"]').click();await ready();
    const qr=await page.evaluate(()=>{const a=Editor.source().data,b=Editor.frame.getContext('2d').getImageData(0,0,800,480).data;let diff=0;for(let i=0;i<a.length;i++)if(a[i]!==b[i])diff++;return diff;});
    check('text mode preserves generated QR pixels exactly',qr===0,{differentBytes:qr});await exportPng('editor-qr-panel.png');
    await page.locator('#f').setInputFiles('output/playwright/jd75-color-source.png');await ready();
    await page.locator('#edMode [data-mode="color"]').click();await ready();await exportPng('editor-color-chart.png');
    check('native color chart keeps dimensions',await page.evaluate(()=>Editor.img.width===800&&Editor.img.height===480&&Editor.state.composition.zoom===1));
    const report={checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass)};
    const wait=page.waitForEvent('download');await page.evaluate(report=>{const a=document.createElement('a');a.download='editor-assets-report.json';a.href='data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2));a.click();},report);await(await wait).saveAs('output/playwright/editor-assets-report.json');
    if(report.failures.length)throw new Error(JSON.stringify(report.failures));return report;
  }finally{
    await page.evaluate(saved=>{if(document.getElementById('previewDialog').open)document.getElementById('previewDialog').close();Editor.stop();if(saved.editor===null)localStorage.removeItem(Editor.key);else localStorage.setItem(Editor.key,saved.editor);if(saved.onboard===null)localStorage.removeItem('epd_onboard_done');else localStorage.setItem('epd_onboard_done',saved.onboard);},saved);
    await page.unroute('**/*',route);
  }
}
