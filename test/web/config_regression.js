async (page) => {
  const origin=await page.evaluate(()=>location.origin);
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local test server required');
  await page.unrouteAll({behavior:'ignoreErrors'});
  page.removeAllListeners('pageerror');
  const hostile = '<img src=x onerror=window.x=1>';
  const saved = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const responses = {
    '/wifi_status': {mode:3,sta_connected:true,sta_ssid:hostile,sta_ip:'192.0.2.1',saved_wifi:[hostile],mdns_host:'epdtest'},
    '/status': {battery_valid:false},
    '/panel_config': {panel:30,active_panel:30,pending_panel:30,width:600,height:448,busy_idle:0,pending_busy_idle:0},
    '/auth_config': {enabled:true,user:hostile},
    '/welcome_config': {enabled:true,title:'Test',message:''},
    '/version': {version:'test',date:'test',time:'test',idf:'test'},
    '/power_config': {enabled:false,interval_min:60,idle_timeout_s:120},
    '/button_modes': {mask:255},
    '/scan': {aps:[{ssid:hostile,auth:3,rssi:-55}]}
  };
  await page.route(origin+'/**', async route => {
    const request = route.request();
    const path = request.url().slice(origin.length).split('?')[0];
    if (request.method() === 'POST') {
      saved.push({path,body:request.postDataJSON()});
      return route.fulfill({json:{ok:true}});
    }
    if (responses[path]) return route.fulfill({json:responses[path]});
    return route.continue();
  });
  await page.goto(origin+'/config.html');
  await page.evaluate(async () => { await initConfigPage(); await scanWifi(); });
  await page.waitForTimeout(100);
  const injected = await page.evaluate(() => ({
    ran:window.x===1,
    nodes:document.querySelectorAll('#authLog img,#apList img,#savedWifiList img').length,
    auth:document.getElementById('authLog').textContent,
    ssid:document.querySelector('.ap-ssid').textContent
  }));
  if (injected.ran || injected.nodes || !injected.auth.includes(hostile) || injected.ssid!==hostile)
    throw new Error('Remote strings were not rendered as plain text: '+JSON.stringify(injected));
  await page.locator('.ap-item').click();
  if (await page.locator('#inSSID').inputValue() !== hostile)
    throw new Error('SSID selection must preserve the literal value');
  await page.evaluate(async () => {
    window.confirm=()=>true;
    document.getElementById('pnPanel').value='30';
    document.getElementById('pnBusy').value='0';
    await savePanel();
  });
  if (!saved.some(x=>x.path==='/panel_config' && x.body.panel===30 && x.body.busy_idle===0))
    throw new Error('Explicit BUSY=0 was overwritten');
  const layouts=[];
  for (const [width,height] of [[1440,900],[390,844],[360,800]]) {
    await page.setViewportSize({width,height});
    const metrics = await page.evaluate(() => ({
      overflow:document.documentElement.scrollWidth>innerWidth,
      small:[...document.querySelectorAll('.btn')].filter(e=>{
        const r=e.getBoundingClientRect();return r.width>0 && r.height>0 && r.height<44;
      }).map(e=>e.textContent.trim())
    }));
    if (metrics.overflow || (width<760 && metrics.small.length))
      throw new Error('Layout regression at '+width+': '+JSON.stringify(metrics));
    await page.screenshot({path:'output/web-tests/config/config-'+width+'.png',fullPage:true});
    layouts.push({width,height});
  }
  if(errors.length) throw new Error(errors.join('\n'));
  return {plainText:true,ssidSelection:true,busyOverride:true,layouts,errors};
}
