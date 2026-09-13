// Theme-only UI regression. The local server and route guard cannot write a device.
async(page)=>{
  const origin=await page.evaluate(()=>location.origin),checks=[],errors=[],posts=[];
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local test server required');
  const profiles=await(await page.request.get(origin+'/profiles.json')).json();
  const status=profiles.find(p=>p.panel===37);
  const check=(name,pass,extra={})=>checks.push({name,pass,...extra});
  const intercept=async route=>{const req=route.request();if(!req.url().startsWith(origin+'/'))return route.abort();if(req.method()==='POST'){posts.push(req.url());return route.abort();}const path=req.url().slice(origin.length).split('?')[0];if(path==='/status')return route.fulfill({json:status});if(path==='/favicon.ico')return route.fulfill({status:204});return route.continue();};
  const ready=()=>page.waitForFunction(()=>Workspace.image&&Workspace.ready===Workspace.revision&&!Workspace.error);
  const onError=e=>errors.push(String(e));page.on('pageerror',onError);await page.route('**/*',intercept);
  await page.setViewportSize({width:1440,height:900});await page.goto(origin);
  const saved=await page.evaluate(()=>Object.fromEntries(Object.entries(localStorage)));let sibling;
  try{
    await page.evaluate(()=>{localStorage.setItem('epd_onboard_done','1');localStorage.removeItem('upload_editor_v4_panel_37');});await page.reload();
    await page.locator('#f').setInputFiles('test/web/fixtures/reference.png');await ready();
    const base=await page.evaluate(()=>({frame:Workspace.frame.toDataURL(),state:JSON.stringify(Workspace.state),revision:Workspace.revision}));
    for(const [theme,accent,hover,light,text] of [['loopy','rgb(255, 155, 179)','rgb(255, 217, 229)','rgb(255, 230, 238)','rgb(90, 48, 80)'],['kitty','rgb(255, 77, 109)','rgb(255, 219, 228)','rgb(255, 237, 241)','rgb(74, 21, 37)'],['lotso','rgb(183, 70, 120)','rgb(240, 214, 226)','rgb(248, 232, 239)','rgb(74, 20, 40)']]){
      await page.mouse.move(0,0);await page.evaluate(theme=>setTheme(theme),theme);
      const metrics=await page.evaluate(()=>{
        const r=document.getElementById('imageWorkspace').shadowRoot,css=id=>getComputedStyle(r.getElementById(id)),button=css('upload');
        const luma=rgb=>{const a=rgb.match(/\d+/g).slice(0,3).map(Number).map(v=>{v/=255;return v<=.04045?v/12.92:((v+.055)/1.055)**2.4;});return .2126*a[0]+.7152*a[1]+.0722*a[2];};
        const a=luma(button.backgroundColor),b=luma(button.color);
        return {button:button.backgroundColor,text:button.color,slider:css('param-brightness').accentColor,rangeAppearance:css('param-brightness').appearance,progress:css('param-brightness').getPropertyValue('--range-progress'),selected:getComputedStyle(r.querySelector('[data-mode="color"]')).color,light:css('panTool').backgroundColor,canvas:getComputedStyle(r.querySelector('.canvas-area')).backgroundColor,contrast:(Math.max(a,b)+.05)/(Math.min(a,b)+.05),connected:getComputedStyle(r.querySelector('.status-dot')).backgroundColor};
      });
      check(theme+' uses readable matching pastel colors',metrics.button===light&&metrics.slider===accent&&metrics.selected===text&&metrics.light===light&&metrics.text===text&&metrics.contrast>=4.5,metrics);
      check(theme+' slider avoids native dark track',metrics.rangeAppearance==='none'&&metrics.progress==='50%');
      check(theme+' keeps canvas neutral and connection semantic',metrics.canvas==='rgb(233, 235, 237)'&&metrics.connected==='rgb(46, 158, 82)');
      await page.locator('#upload').hover();check(theme+' hover matches theme',await page.locator('#upload').evaluate((el,hover)=>getComputedStyle(el).backgroundColor===hover,hover));await page.mouse.move(0,0);
      await page.locator('#param-brightness').focus();check(theme+' focus ring matches theme',await page.locator('#param-brightness').evaluate((el,text)=>getComputedStyle(el).outlineColor===text,text));await page.locator('#param-brightness').evaluate(el=>el.blur());
      check(theme+' cannot change processing state or pixels',await page.evaluate(base=>Workspace.revision===base.revision&&JSON.stringify(Workspace.state)===base.state&&Workspace.frame.toDataURL()===base.frame,base));
      for(const [width,height] of [[1440,900],[360,800],[844,390]]){
        await page.setViewportSize({width,height});await page.evaluate(()=>Workspace.paint());await page.locator('#upload').click({trial:true});
        const layout=await page.evaluate(()=>{const r=document.getElementById('imageWorkspace').shadowRoot,s=r.getElementById('editorShell'),b=r.getElementById('upload').getBoundingClientRect(),m=r.getElementById('workspace').getBoundingClientRect(),v=r.getElementById('viewport').getBoundingClientRect();return {overflow:s.scrollWidth>innerWidth||s.scrollHeight>innerHeight,fit:b.bottom<=m.top,preview:v.width>120&&v.height>100};});
        check(theme+' '+width+'x'+height+' stays usable',!layout.overflow&&layout.fit&&layout.preview,layout);
        await page.mouse.move(0,0);await page.screenshot({path:'output/web-tests/theme/'+theme+'-'+width+'x'+height+'.png'});
      }
      await page.setViewportSize({width:1440,height:900});
    }
    sibling=await page.context().newPage();await sibling.goto(origin);await sibling.evaluate(()=>setTheme('kitty'));
    await page.waitForFunction(()=>document.documentElement.dataset.theme==='kitty');check('another tab updates open workspace theme',await page.locator('#upload').evaluate(el=>getComputedStyle(el).backgroundColor==='rgb(255, 237, 241)'));await sibling.close();sibling=null;
    check('cross-tab theme update preserves image and parameters',await page.evaluate(base=>Workspace.revision===base.revision&&Workspace.frame.toDataURL()===base.frame&&JSON.stringify(Workspace.state)===base.state,base));
    await page.evaluate(()=>Workspace.close());await page.reload();await page.locator('#up').click();await page.waitForFunction(()=>Workspace.profile?.id===37);
    check('theme persists on reload',await page.locator('#upload').evaluate(el=>document.documentElement.dataset.theme==='kitty'&&getComputedStyle(el).backgroundColor==='rgb(255, 237, 241)'));
    await page.evaluate(()=>setTheme('invalid-theme'));check('invalid theme falls back safely',await page.locator('#upload').evaluate(el=>document.documentElement.dataset.theme==='loopy'&&getComputedStyle(el).backgroundColor==='rgb(255, 230, 238)'));
    check('no image or device uploads from theme changes',posts.length===0);check('no runtime errors',errors.length===0,{errors});
    const report={checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass)};
    const wait=page.waitForEvent('download');await page.evaluate(report=>{const a=document.createElement('a');a.href='data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2));a.download='theme-report.json';a.click();},report);await(await wait).saveAs('output/web-tests/theme/theme-report.json');
    if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed};
  }finally{if(sibling)await sibling.close();await page.evaluate(saved=>{Workspace.stop?.();localStorage.clear();for(const[k,v]of Object.entries(saved))localStorage.setItem(k,v);},saved);await page.unroute('**/*',intercept);page.removeListener('pageerror',onError);}
}
