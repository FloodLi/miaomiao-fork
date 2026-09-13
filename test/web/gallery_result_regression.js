async(page)=>{
  const origin=await page.evaluate(()=>location.origin);
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local test server required');
  const checks=[],errors=[];let epoch=10,state='pending',legacy=false,showStatus=200,deleteStatus=200,deleteBody='deleted',pollStatus=200,wrongEpoch=false,gate=null,statusGate=null,polls=0;
  const check=(name,pass)=>checks.push({name,pass});
  const intercept=async route=>{
    const req=route.request(),url=req.url();if(!url.startsWith(origin+'/'))return route.abort();
    const path=url.slice(origin.length).split('?')[0];
    if(path==='/show')return route.fulfill({status:showStatus,json:legacy?{ok:true,queued:true}:{ok:showStatus===200,queued:true,epoch}});
    if(path==='/delete_image'||path==='/delete')return route.fulfill({status:deleteStatus,body:deleteBody});
    if(req.method()==='POST')throw Error('Unexpected mutation');
    if(path==='/status'&&url.includes('display_epoch=')){
      polls++;const requested=Number(url.split('display_epoch=')[1]),captured={epoch:wrongEpoch?requested+1:requested,state,error:state==='error'?'ESP_ERR_TIMEOUT':''},waiting=gate;
      if(waiting)await waiting;
      return route.fulfill({status:pollStatus,json:{display_result:captured}}).catch(()=>{});
    }
    if(path==='/status'){if(statusGate)await statusGate;return route.fulfill({json:{panel:34,panel_width:800,panel_height:480,panel_name:'test'}});}
    if(path==='/images')return route.fulfill({json:{items:[]}});
    if(path==='/favicon.ico')return route.fulfill({status:204});
    return route.continue();
  };
  const onError=e=>errors.push(String(e));page.on('pageerror',onError);await page.route('**/*',intercept);await page.goto(origin);
  const saved=await page.evaluate(()=>localStorage.getItem('epd_onboard_done'));
  const show=()=>page.evaluate(()=>showImg('fixture.png'));
  const log=()=>page.locator('#log').textContent();
  let release;
  try{
    await page.evaluate(()=>{localStorage.setItem('epd_onboard_done','1');document.querySelector('.onboard-overlay')?.remove();});
    check('bounded production polling defaults',await page.evaluate(()=>HomeOperation.intervalMs===1000&&HomeOperation.requestTimeoutMs===10000&&HomeOperation.totalTimeoutMs===300000));
    await page.evaluate(()=>{HomeOperation.intervalMs=5;HomeOperation.requestTimeoutMs=60;HomeOperation.totalTimeoutMs=300;});
    await page.evaluate(()=>{window.activeShow=showImg('fixture.png');});await page.waitForFunction(()=>document.getElementById('log').textContent.includes('等待屏幕刷新'));
    check('queued is not displayed',!(await log()).includes('显示成功'));
    state='ok';await page.evaluate(()=>window.activeShow);check('only final ok displays success',(await log())==='显示成功');
    for(const [value,message]of[['error','显示失败'],['canceled','被新请求替换'],['unavailable','结果未确认']]){epoch++;state=value;await show();check(value+' remains distinct',(await log()).includes(message)&&!(await log()).includes('显示成功'));}
    const before=polls;legacy=true;await show();check('old firmware ack never claims completion',(await log()).includes('已提交')&&(await log()).includes('未确认')&&polls===before);legacy=false;
    showStatus=500;await show();check('show admission failure is visible',(await log()).includes('请求失败'));showStatus=200;
    state='ok';wrongEpoch=true;await show();check('wrong epoch cannot confirm success',(await log()).includes('结果未确认'));wrongEpoch=false;
    state='pending';await show();check('pending timeout is unconfirmed',(await log()).includes('结果未确认')&&(await log()).includes('超时'));
    pollStatus=503;await show();check('network error ends unconfirmed',(await log()).includes('结果未确认'));pollStatus=200;
    gate=new Promise(resolve=>{release=resolve;});state='ok';const count=polls;await page.evaluate(()=>{window.activeShow=showImg('slow.png');});await page.waitForFunction(()=>!!HomeOperation.controller);
    await page.evaluate(()=>window.activeShow);check('single stalled fetch is bounded',(await log()).includes('结果未确认')&&polls>count);release();gate=null;
    gate=new Promise(resolve=>{release=resolve;});epoch=100;await page.evaluate(()=>{window.activeShow=showImg('old.png');});await page.waitForFunction(()=>!!HomeOperation.controller);
    gate=null;epoch=101;state='error';await show();const latest=await log();release();await page.evaluate(()=>window.activeShow);
    check('late old success cannot overwrite new failure',(await log())===latest&&latest.includes('显示失败'));
    let releaseStatus;statusGate=new Promise(resolve=>{releaseStatus=resolve;});
    const refreshStarted=page.waitForRequest(r=>r.url()===origin+'/status');
    await page.evaluate(()=>{window.oldDelete=delFiles();});await refreshStarted;
    epoch++;state='error';await show();const newer=await log();releaseStatus();statusGate=null;await page.evaluate(()=>window.oldDelete);
    check('old operation status refresh cannot overwrite new result',(await log())===newer&&newer.includes('显示失败'));
    state='pending';await page.evaluate(()=>{window.activeShow=showImg('leaving.png');});await page.waitForFunction(()=>!!HomeOperation.controller);
    await page.evaluate(()=>window.dispatchEvent(new Event('pagehide')));const stopped=polls;await page.evaluate(()=>window.activeShow);await page.waitForTimeout(25);check('page exit stops polling',polls===stopped);
    for(const status of [404,500]){
      deleteStatus=status;deleteBody='injected delete failure';await page.evaluate(async()=>{const confirm=window.confirm;window.confirm=()=>true;try{await delImg('fixture.png');}finally{window.confirm=confirm;}});
      check('delete '+status+' cannot claim success',(await log()).startsWith('删除失败')&&!(await log()).includes('已删除 fixture'));
    }
    deleteStatus=200;deleteBody='delete fixture.png: ok';await page.evaluate(async()=>{const confirm=window.confirm;window.confirm=()=>true;try{await delImg('fixture.png');}finally{window.confirm=confirm;}});check('confirmed deletion succeeds',(await log()).startsWith('已删除 fixture'));
    for(const [status,body]of[[500,'upload.jpg=已删除，image.bin=删除失败；清屏=完成'],[500,'upload.jpg=已删除；清屏=失败'],[409,'清屏=已被新请求替换']]){
      deleteStatus=status;deleteBody=body;await page.evaluate(()=>delFiles());check('clear '+status+' reports exact partial result',(await log())==='操作未完成：'+body);
    }
    deleteStatus=200;deleteBody='upload.jpg=不存在，image.bin=不存在；清屏=完成';await page.evaluate(()=>delFiles());check('absent files and confirmed clear succeed',(await log())==='操作完成：'+deleteBody);
    check('no runtime errors',errors.length===0);
    const report={checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass),errors};
    const wait=page.waitForEvent('download');await page.evaluate(report=>{const a=document.createElement('a');a.href='data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2));a.download='report.json';a.click();},report);await(await wait).saveAs('output/web-tests/gallery/report.json');
    if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed};
  }finally{release?.();await page.evaluate(saved=>{HomeOperation.begin();if(saved===null)localStorage.removeItem('epd_onboard_done');else localStorage.setItem('epd_onboard_done',saved);delete window.activeShow;},saved);await page.unroute('**/*',intercept);page.removeListener('pageerror',onError);}
}
