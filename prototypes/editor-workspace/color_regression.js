async(page)=>{
  const origin=await page.evaluate(()=>location.origin),checks=[];
  const check=(name,pass,metrics={})=>checks.push({name,pass,...metrics});
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local prototype only');
  const save=async(name,data)=>{const wait=page.waitForEvent('download');await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});await(await wait).saveAs('output/workspace-prototype/'+name);};
  await page.goto(origin);await page.waitForFunction(()=>Workspace.image&&Workspace.ready===Workspace.revision&&!Workspace.error);
  const saved=await page.evaluate(()=>localStorage.getItem(Workspace.key));
  const ready=()=>page.waitForFunction(()=>Workspace.image&&Workspace.ready===Workspace.revision&&!Workspace.error);
  try{
    await page.evaluate(()=>{Workspace.selectPanel(34);Workspace.state.params.color=defaults(Workspace.profile,'color');Workspace.state.mode='color';Workspace.request();Workspace.controls();});await ready();
    check('ID34 candidate defaults to 35',await page.evaluate(()=>Workspace.state.params.color.neutralProtection===35));
    const out={};
    for(const strength of [0,35]){
      await page.evaluate(n=>Workspace.change(()=>{Workspace.state.params.color.neutralProtection=n;},true),strength);await ready();
      out[strength]=await page.evaluate(()=>{
        const source=Workspace.source().data,frame=Workspace.frame.getContext('2d').getImageData(0,0,Workspace.profile.width,Workspace.profile.height),data=frame.data;
        const palette=ImageProcessor.palette,ys=palette.map(c=>.2126*ImageProcessor.linear(c[0])+.7152*ImageProcessor.linear(c[1])+.0722*ImageProcessor.linear(c[2]));
        let lowN=0,lowWarm=0,red=0,yellow=0,y=0;
        for(let i=0;i<data.length;i+=4){const k=palette.findIndex(c=>c.every((v,j)=>v===data[i+j]));if(k<0)throw Error('Illegal color');y+=ys[k];if(k===2)red++;if(k===3)yellow++;
          const max=Math.max(source[i],source[i+1],source[i+2]),min=Math.min(source[i],source[i+1],source[i+2]);
          if(max-min<=40&&max>=48&&max<=235){lowN++;if(k>=2)lowWarm++;}
        }
        const second=ImageProcessor.process({width:frame.width,height:frame.height,data:data.slice()},{mode:'color'}).data;let changes=0;for(let i=0;i<data.length;i++)if(data[i]!==second[i])changes++;
        return {png:Workspace.frame.toDataURL(),red,yellow,lowN,lowWarm,meanLuma:y/(data.length/4),secondPassChanges:changes};
      });
      await save('color-bias-'+strength+'.png',out[strength].png);
    }
    check('warm dots decrease on photo low-chroma regions',out[35].lowWarm<out[0].lowWarm*.9,{before:out[0].lowWarm,after:out[35].lowWarm,samples:out[35].lowN});
    check('photo model luminance is stable',Math.abs(out[35].meanLuma-out[0].meanLuma)<.004,{before:out[0].meanLuma,after:out[35].meanLuma});
    check('prepared result survives neutral default processing',out[35].secondPassChanges===0,{changedBytes:out[35].secondPassChanges});
    const worker=out[35].png;await page.evaluate(()=>{Workspace.stop();Workspace.failedWorker=true;Workspace.request();});await ready();
    check('fallback and worker agree',await page.evaluate(png=>Workspace.frame.toDataURL()===png,worker));
    await page.evaluate(()=>{Workspace.save();});await page.reload();await ready();
    check('correction setting survives reload',await page.evaluate(()=>Workspace.profile.id===34&&Workspace.state.params.color.neutralProtection===35));
    for(const id of [37,43,44,47]){await page.locator('#panelSelect').selectOption(String(id));await ready();check(id+' correction is not enabled by default',await page.evaluate(id=>(id===37||!Workspace.supports('neutralProtection'))&&Workspace.state.params[Workspace.state.mode].neutralProtection===0,id));}
    await page.locator('#panelSelect').selectOption('34');await ready();await page.setViewportSize({width:1440,height:900});
    await page.locator('#advanced summary').click();await page.screenshot({path:'output/workspace-prototype/color-bias-preview.png'});
    const report={checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass),metrics:{before:{...out[0],png:undefined},after:{...out[35],png:undefined}}};
    await save('color-bias-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));
    if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed,metrics:report.metrics};
  }finally{await page.evaluate(saved=>{Workspace.stop();if(saved===null)localStorage.removeItem(Workspace.key);else localStorage.setItem(Workspace.key,saved);},saved);}
}
