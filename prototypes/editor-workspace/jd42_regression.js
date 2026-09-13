// Local ID37 test, writes native 400x300 originals and old/new output comparisons.
async(page)=>{
  const origin=await page.evaluate(()=>location.origin),checks=[];
  if(!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin))throw Error('Local prototype only');
  const check=(name,pass,metrics={})=>checks.push({name,pass,...metrics});
  const save=async(name,data)=>{const wait=page.waitForEvent('download');await page.evaluate(({name,data})=>{const a=document.createElement('a');a.href=data;a.download=name;a.click();},{name,data});await(await wait).saveAs('output/jd42-color-20260913/'+name);};
  await page.goto(origin+'/?panel=37');await page.waitForFunction(()=>Workspace.image&&!Workspace.loading&&Workspace.ready===Workspace.revision&&!Workspace.error);
  const saved=await page.evaluate(()=>localStorage.getItem(Workspace.key));
  const ready=()=>page.waitForFunction(()=>Workspace.image&&!Workspace.loading&&Workspace.ready===Workspace.revision&&!Workspace.error);
  try{
    const migration=await page.evaluate(()=>{
      Workspace.states['preview-37']={mode:'color',params:{color:{brightness:28,contrast:175}}};delete Workspace.states['preview-37-color-v1'];Workspace.selectPanel(37);
      return {old:Workspace.states['preview-37'].params.color.brightness,current:Workspace.state.params.color,profile:Workspace.profile};
    });await ready();
    check('ID37 uses separate candidate profile and neutral defaults',migration.old===28&&migration.profile.profileId==='preview-37-color-v1'&&migration.profile.processor==='four-color'&&migration.current.brightness===0&&migration.current.contrast===100&&migration.current.sharp===0&&migration.current.neutralProtection===0,migration);
    const source=await page.evaluate(()=>{
      const c=document.createElement('canvas');c.width=400;c.height=300;const x=c.getContext('2d');
      x.fillStyle='#fff';x.fillRect(0,0,400,300);x.fillStyle='#000';x.font='bold 13px sans-serif';x.fillText('JD79668 4.2  |  COLOR STEPS  |  400 x 300',7,17);
      const rows=[['RED LIGHT',[255,255,255],[192,57,43]],['RED DARK',[0,0,0],[192,57,43]],['YELLOW LIGHT',[255,255,255],[210,165,24]],['YELLOW DARK',[0,0,0],[210,165,24]],['RED / YELLOW',[192,57,43],[210,165,24]],['GRAY',[0,0,0],[255,255,255]],['SKIN',[80,30,20],[255,224,189]],['BLUE / GREEN',[0,0,255],[0,255,0]]];
      const linear=v=>{v/=255;return v<=.04045?v/12.92:((v+.055)/1.055)**2.4;};
      const encode=v=>Math.round(255*(v<=.0031308?12.92*v:1.055*v**(1/2.4)-.055));
      const patches=[];
      rows.forEach(([label,a,b],row)=>{
        const y=36+row*29;x.fillStyle='#000';x.font='bold 8px sans-serif';x.fillText(label,5,y+16);
        for(let step=0;step<12;step++){
          const left=86+Math.floor(307*step/12),right=86+Math.floor(307*(step+1)/12),rgb=a.map((v,k)=>encode(linear(v)*(1-step/11)+linear(b[k])*step/11));
          x.fillStyle=`rgb(${rgb.join(',')})`;x.fillRect(left,y,right-left-1,26);patches.push({row,step,x:left,y,w:right-left-1,h:26,rgb});
          if(!row){x.fillStyle='#000';x.font='9px monospace';x.fillText(String(step).padStart(2,'0'),left+6,30);}
        }
      });
      x.fillStyle='#000';x.font='9px sans-serif';x.fillText('12 steps / Four inks / Last two rows: gamut limits',7,285);
      x.fillRect(0,0,400,1);x.fillRect(0,299,400,1);x.fillRect(0,0,1,300);x.fillRect(399,0,1,300);
      window.jd42Patches=patches;return c.toDataURL();
    });await save('source-chart.png',source);
    await page.locator('#file').setInputFiles('output/jd42-color-20260913/source-chart.png');await ready();
    const result=await page.evaluate(()=>{
      const input=Workspace.source(),p=Workspace.profile,legacy={...ImageProcessor.defaults('color'),contrast:104,sharp:4,threshold:128,red:34,yellow:36};
      const old=LegacyProcessor.process({width:400,height:300,data:input.data.slice()},p,legacy),next=Workspace.frame.getContext('2d').getImageData(0,0,400,300);
      const png=im=>{const c=document.createElement('canvas');c.width=400;c.height=300;c.getContext('2d').putImageData(im instanceof ImageData?im:new ImageData(im.data,400,300),0,0);return c.toDataURL();};
      const measure=im=>{
        const colors=new Set(),patches=[];let tinted=0,endpoints=0;
        for(let i=0;i<im.data.length;i+=4){colors.add([...im.data.slice(i,i+4)].join(','));if(input.data[i]===input.data[i+1]&&input.data[i]===input.data[i+2]){if(im.data[i]!==im.data[i+1]||im.data[i]!==im.data[i+2])tinted++;if((input.data[i]===0||input.data[i]===255)&&im.data[i]!==input.data[i])endpoints++;}}
        for(const p of jd42Patches){const sum=[0,0,0];let n=0;for(let y=p.y+2;y<p.y+p.h-2;y++)for(let x=p.x+2;x<p.x+p.w-2;x++){const q=(y*400+x)*4;for(let k=0;k<3;k++)sum[k]+=ImageProcessor.linear(im.data[q+k]);n++;}const mean=sum.map(v=>v/n);const error=Math.sqrt(mean.reduce((s,v,k)=>s+[.2126,.7152,.0722][k]*(v-ImageProcessor.linear(p.rgb[k]))**2,0));patches.push({...p,mean,error});}
        return {colors:[...colors],tinted,endpoints,patches,meanError:patches.filter(p=>p.row<6).reduce((s,p)=>s+p.error,0)/72};
      };
      const twice=LegacyProcessor.process({width:400,height:300,data:next.data.slice()},p,legacy);let differences=0;for(let i=0;i<next.data.length;i++)if(next.data[i]!==twice.data[i])differences++;
      return {old:measure(old),next:measure(next),oldPng:png(old),newPng:png(next),differences};
    });
    await save('chart-legacy.png',result.oldPng);await save('chart-new.png',result.newPng);
    check('native 400x300 output uses only four ink codes',result.next.colors.every(c=>['0,0,0,255','255,255,255,255','192,57,43,255','210,165,24,255'].includes(c)));
    check('gray and black/white endpoints remain clean',result.next.tinted===0&&result.next.endpoints===0,{tinted:result.next.tinted,endpoints:result.next.endpoints});
    check('reference color steps improve over legacy blocks',result.next.meanError<result.old.meanError*.5,{old:result.old.meanError,next:result.next.meanError});
    check('prepared chart passes old ID37 photo preset unchanged',result.differences===0,{differentBytes:result.differences});
    for(let row=0;row<6;row++){
      const means=result.next.patches.filter(p=>p.row===row).map(p=>p.mean[row===4?1:0]),ascending=row!==0&&row!==2;
      check('row '+row+' keeps tonal ordering',means.every((v,i)=>!i||(ascending?v>=means[i-1]-.015:v<=means[i-1]+.015)),{means});
    }
    const worker=result.newPng;await page.evaluate(()=>{Workspace.stop();Workspace.failedWorker=true;Workspace.request();});await ready();check('worker and fallback match',await page.evaluate(png=>Workspace.frame.toDataURL()===png,worker));
    await page.evaluate(()=>{Workspace.stop();Workspace.failedWorker=false;});
    let releasePhoto;const photoGate=new Promise(resolve=>{releasePhoto=resolve;});
    const delayPhoto=async route=>{await photoGate;await route.continue();};
    await page.route('**/assets/photo.jpg',delayPhoto);
    try{
      await page.selectOption('#sampleSelect','photo');
      check('pending sample cannot export previous chart',await page.evaluate(async()=>{let blocked=false;try{await Workspace.blob();}catch(e){blocked=true;}return Workspace.loading&&Workspace.ready===-1&&document.getElementById('upload').disabled&&document.getElementById('download').disabled&&blocked;}));
    }finally{releasePhoto();}
    await ready();await page.unroute('**/assets/photo.jpg',delayPhoto);
    check('photo asset is decoded before comparison',await page.evaluate(()=>Workspace.fileName==='示例图片.png'&&Workspace.image.width===1280&&Workspace.image.height===960));
    const photo=await page.evaluate(()=>{
      const input=Workspace.source(),legacy={...ImageProcessor.defaults('color'),contrast:104,sharp:4,red:34,yellow:36},old=LegacyProcessor.process({width:400,height:300,data:input.data.slice()},Workspace.profile,legacy);
      const c=document.createElement('canvas');c.width=400;c.height=300;c.getContext('2d').putImageData(new ImageData(old.data,400,300),0,0);
      const next=Workspace.frame.getContext('2d').getImageData(0,0,400,300),twice=LegacyProcessor.process({width:400,height:300,data:next.data.slice()},Workspace.profile,legacy);
      return {old:c.toDataURL(),next:Workspace.frame.toDataURL(),unchanged:next.data.every((v,i)=>v===twice.data[i])};
    });await save('photo-legacy.png',photo.old);await save('photo-new.png',photo.next);
    check('prepared photo is distinct and survives old photo defaults',photo.next!==result.newPng&&photo.unchanged);
    await page.evaluate(()=>Workspace.change(()=>{Workspace.state.params.color.neutralProtection=20;},true));await ready();
    await save('photo-neutral-20.png',await page.evaluate(()=>Workspace.frame.toDataURL()));
    await page.evaluate(()=>Workspace.change(()=>{Workspace.state.params.color.neutralProtection=0;},true));await ready();
    await page.setViewportSize({width:1440,height:900});await page.screenshot({path:'output/jd42-color-20260913/preview.png'});
    let posted;
    const capture=async route=>{posted=route.request().postDataBuffer().toString('base64');await route.continue();};
    await page.route('**/api/mock-upload?panel=37',capture);
    try{await page.locator('#upload').click();await page.waitForFunction(()=>!Workspace.busy&&Workspace.notice.includes('模拟完成'));}finally{await page.unroute('**/api/mock-upload?panel=37',capture);}
    check('ID37 mock POST is the complete current photo',!!posted&&await page.evaluate(async data=>{const im=new Image();im.src='data:image/png;base64,'+data;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;c.getContext('2d').drawImage(im,0,0);return c.width===400&&c.height===300&&c.toDataURL()===Workspace.frame.toDataURL();},posted));
    await page.locator('[data-mode="mono"]').click();await ready();
    check('ID37 mono has only BW and hides color controls',await page.evaluate(()=>{const d=Workspace.frame.getContext('2d').getImageData(0,0,400,300).data;for(let i=0;i<d.length;i+=4)if(![0,255].includes(d[i])||d[i]!==d[i+1]||d[i]!==d[i+2])return false;return !Workspace.supports('saturation')&&!Workspace.supports('neutralProtection');}));
    await save('photo-mono.png',await page.evaluate(()=>Workspace.frame.toDataURL()));
    await page.locator('[data-mode="color"]').click();await ready();check('mode switch restores color frame',await page.evaluate(png=>Workspace.frame.toDataURL()===png,photo.next));
    for(const [width,height] of [[1280,720],[390,844],[844,390]]){
      await page.setViewportSize({width,height});await page.evaluate(()=>Workspace.paint());
      check('ID37 '+width+'x'+height+' keeps canvas and controls visible',await page.evaluate(png=>{const v=document.getElementById('viewport').getBoundingClientRect(),b=document.getElementById('param-brightness').getBoundingClientRect();return document.documentElement.scrollWidth<=innerWidth&&document.documentElement.scrollHeight<=innerHeight&&v.width>130&&v.height>100&&b.top>=0&&b.bottom<=innerHeight&&Workspace.frame.toDataURL()===png;},photo.next));
      await page.screenshot({path:'output/jd42-color-20260913/view-'+width+'x'+height+'.png'});
    }
    await page.setViewportSize({width:1440,height:900});
    await page.locator('#sampleSelect').selectOption('qr');await ready();await page.locator('[data-mode="text"]').click();await ready();
    check('ID37 QR text retains BW endpoints without dither controls',await page.evaluate(()=>{const src=Workspace.source().data,d=Workspace.frame.getContext('2d').getImageData(0,0,400,300).data;let black=0,white=0;for(let i=0;i<d.length;i+=4){if(![0,255].includes(d[i])||d[i]!==d[i+1]||d[i]!==d[i+2])return false;if(src[i]===src[i+1]&&src[i]===src[i+2]&&[0,255].includes(src[i])&&src[i]!==d[i])return false;if(d[i]===0)black++;else white++;}return black>100&&white>100&&!Workspace.supports('dither')&&!Workspace.supports('sharp');}));
    await save('qr-text.png',await page.evaluate(()=>Workspace.frame.toDataURL()));
    await page.locator('[data-mode="color"]').click();await page.locator('#sampleSelect').selectOption('photo');await ready();
    for(const id of [0,26,35,36,38,40,41,44,43]){const p=await page.evaluate(id=>Workspace.catalog.find(p=>p.id===id),id);check(id+' is not routed by 400x300 geometry',p.processor==='legacy');}
    await page.locator('#panelSelect').selectOption('34');await ready();await page.evaluate(()=>{Workspace.state.params.color=defaults(Workspace.profile,'color');Workspace.request();});await ready();
    const current75=await page.evaluate(()=>({png:Workspace.frame.toDataURL(),protection:Workspace.state.params.color.neutralProtection}));
    await page.locator('#file').setInputFiles('output/workspace-prototype/color-bias-35.png');await ready();
    check('7.5 accepted output and protection remain unchanged',current75.protection===35&&await page.evaluate(png=>Workspace.original.toDataURL()===png,current75.png));
    const report={date:new Date().toISOString(),panel:37,checks,passed:checks.filter(c=>c.pass).length,failures:checks.filter(c=>!c.pass),oldError:result.old.meanError,newError:result.next.meanError};
    await save('test-report.json','data:application/json;charset=utf-8,'+encodeURIComponent(JSON.stringify(report,null,2)));
    if(report.failures.length)throw Error(JSON.stringify(report.failures));return {passed:report.passed,oldError:report.oldError,newError:report.newError};
  }finally{await page.evaluate(saved=>{Workspace.stop();delete window.jd42Patches;if(saved===null)localStorage.removeItem(Workspace.key);else localStorage.setItem(Workspace.key,saved);},saved);}
}
