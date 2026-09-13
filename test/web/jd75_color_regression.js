// playwright-cli -s=jd75-color run-code --filename test/web/jd75_color_regression.js
// Local mocked browser only. Creates native test charts and a JSON report.
async (page) => {
  const origin='http://127.0.0.1:8765',checks=[],errors=[];
  let panel=34;
  const sizes={34:[800,480],37:[400,300],44:[960,672],43:[792,528]};
  const check=(name,pass,metrics={})=>checks.push({name,pass,...metrics});
  const intercept=async route=>{
    const req=route.request(),pathname=req.url().slice(origin.length).split('?')[0];
    if(!req.url().startsWith(origin+'/')||req.method()!=='GET')return route.abort();
    if(pathname==='/status')return route.fulfill({json:{panel,panel_width:sizes[panel][0],panel_height:sizes[panel][1],panel_has_red:true,panel_has_yellow:true,panel_name:'JD color test',version:'local-test',spiffs_ok:true,spiffs_total:1048576,spiffs_used:0}});
    if(pathname==='/images')return route.fulfill({json:{items:[]}});
    if(pathname==='/wifi_status')return route.fulfill({json:{mode:3,sta_connected:false}});
    if(pathname==='/'||/\.(html|js|css|png|ico)$/.test(pathname))return route.continue();
    return route.fulfill({json:{}});
  };
  const onError=e=>errors.push(String(e));
  await page.route('**/*',intercept);
  await page.goto(origin+'/index.html');
  const saved=await page.evaluate(()=>Object.fromEntries(Object.entries(localStorage)));
  page.on('pageerror',onError);
  const download=async(name,data,mime)=>{
    const wait=page.waitForEvent('download');
    await page.evaluate(async({name,data,mime})=>{
      const blob=mime==='image/png'?await(await fetch(data)).blob():new Blob([data],{type:mime});
      const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=name;a.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);
    },{name,data,mime});
    await(await wait).saveAs('output/playwright/'+name);
  };
  try{
    await page.evaluate(()=>{localStorage.removeItem('upload_img_opts_panel_34');localStorage.setItem('epd_onboard_done','1');});
    await page.reload();
    await page.evaluate(async()=>{await refresh();});
    const result=await page.evaluate(async()=>{
      const canvas=(w=800,h=480)=>{const c=document.createElement('canvas');c.width=w;c.height=h;return c;};
      const pixels=c=>c.getContext('2d').getImageData(0,0,c.width,c.height).data;
      const linear=v=>{v/=255;return v<=.04045?v/12.92:((v+.055)/1.055)**2.4;};
      const encode=v=>Math.round(255*(v<=.0031308?12.92*v:1.055*v**(1/2.4)-.055));
      const mix=(a,b,t)=>a.map((v,c)=>encode(linear(v)*(1-t)+linear(b[c])*t));
      const source=canvas(),ctx=source.getContext('2d'),patches=[];
      ctx.fillStyle='#fff';ctx.fillRect(0,0,800,480);
      ctx.fillStyle='#000';ctx.font='bold 18px sans-serif';ctx.fillText('JD79665 7.5  |  COLOR STEPS  |  800 x 480',12,23);
      const rows=[
        ['RED LIGHT',[255,255,255],[192,57,43]],['RED DARK',[0,0,0],[192,57,43]],
        ['YELLOW LIGHT',[255,255,255],[210,165,24]],['YELLOW DARK',[0,0,0],[210,165,24]],
        ['RED / YELLOW',[192,57,43],[210,165,24]],['GRAY',[0,0,0],[255,255,255]],
        ['SKIN TONES',[80,30,20],[255,224,189]],['BLUE / GREEN',[0,0,255],[0,255,0]]
      ];
      for(let row=0;row<rows.length;row++){
        const [label,a,b]=rows[row],y=49+row*49;
        ctx.fillStyle='#000';ctx.font='bold 12px sans-serif';ctx.fillText(label,10,y+25);
        for(let step=0;step<16;step++){
          const x=148+step*40,color=mix(a,b,step/15);
          ctx.fillStyle=`rgb(${color.join(',')})`;ctx.fillRect(x,y,39,44);
          patches.push({row,step,x,y,w:39,h:44,color});
          if(row===0){ctx.fillStyle='#000';ctx.font='11px monospace';ctx.fillText(String(step).padStart(2,'0'),x+9,43);}
        }
      }
      ctx.fillStyle='#000';ctx.font='12px sans-serif';ctx.fillText('Last two rows: source colors may exceed the four-ink gamut.  Software reference, not measured ink.',12,463);
      ctx.fillRect(0,0,800,1);ctx.fillRect(0,479,800,1);ctx.fillRect(0,0,1,480);ctx.fillRect(799,0,1,480);
      const quant=(input,opt)=>{const c=canvas(input.width,input.height);c.getContext('2d').drawImage(input,0,0);ditherPreview(c,opt);return c;};
      const measure=c=>{
        const d=pixels(c),s=pixels(source);let neutralColored=0,endpointsChanged=0;
        const palette=new Set();
        for(let p=0;p<d.length;p+=4){
          palette.add([d[p],d[p+1],d[p+2],d[p+3]].join(','));
          if(s[p]===s[p+1]&&s[p]===s[p+2]){
            if(d[p]!==d[p+1]||d[p]!==d[p+2])neutralColored++;
            if((s[p]===0||s[p]===255)&&(d[p]!==s[p]||d[p+1]!==s[p]||d[p+2]!==s[p]))endpointsChanged++;
          }
        }
        const measured=patches.map(p=>{
          const mean=[0,0,0],counts=[0,0,0,0];let n=0;
          for(let y=p.y+2;y<p.y+p.h-2;y++)for(let x=p.x+2;x<p.x+p.w-2;x++){
            const q=(y*800+x)*4;for(let c=0;c<3;c++)mean[c]+=linear(d[q+c]);
            const k=d[q]===0?0:d[q]===255?1:d[q]===192?2:3;counts[k]++;n++;
          }
          for(let c=0;c<3;c++)mean[c]/=n;
          const error=Math.sqrt(mean.reduce((sum,v,c)=>sum+[.2126,.7152,.0722][c]*(v-linear(p.color[c]))**2,0));
          return {row:p.row,step:p.step,mean,counts,error};
        });
        return {palette:[...palette],neutralColored,endpointsChanged,patches:measured,meanError:measured.filter(p=>p.row<6).reduce((s,p)=>s+p.error,0)/96};
      };
      applyPreset('photo',false);const old=quant(source,previewOpts());
      const classicOptions=previewOpts();
      applyPreset('colorphoto',false);const opt=previewOpts(),start=performance.now(),next=quant(source,opt),elapsedMs=performance.now()-start;
      const standard=quant(source,{...opt,dotPattern:'regular'});
      const repeat=pixels(quant(source,opt)),first=pixels(next);
      let repeatDifferences=0;for(let i=0;i<first.length;i++)if(first[i]!==repeat[i])repeatDifferences++;
      const texture=[];
      const palette=[[0,0,0],[255,255,255],[192,57,43],[210,165,24]];
      const periodicity=(c,ink)=>{
        const d=pixels(c),v=new Float32Array(64*64);let mean=0;
        for(let y=0;y<64;y++)for(let x=0;x<64;x++){
          const p=((y+16)*96+x+16)*4;
          const a=palette[ink].every((n,k)=>d[p+k]===n)?1:0;v[y*64+x]=a;mean+=a;
        }
        mean/=v.length;const variance=mean*(1-mean);let peak=0;
        for(let lag=2;lag<=12;lag++)for(const [dx,dy] of [[lag,0],[0,lag],[lag,lag],[-lag,lag]]){
          let sum=0,n=0;
          for(let y=0;y<64-dy;y++)for(let x=Math.max(0,-dx);x<64-Math.max(0,dx);x++){
            sum+=(v[y*64+x]-mean)*(v[(y+dy)*64+x+dx]-mean);n++;
          }
          peak=Math.max(peak,sum/n/variance);
        }
        return {mean,peak};
      };
      for(const [a,b] of [[0,1],[0,2],[2,3]])for(const ratio of [.2,1/3,.5,2/3,.8]){
        const patch=canvas(96,96),color=mix(palette[a],palette[b],ratio),pc=patch.getContext('2d');
        pc.fillStyle=`rgb(${color.join(',')})`;pc.fillRect(0,0,96,96);
        texture.push({a,b,ratio,standard:periodicity(quant(patch,{...opt,dotPattern:'regular'}),a),soft:periodicity(quant(patch,{...opt,dotPattern:'soft'}),a)});
      }
      const passthrough=quant(next,classicOptions),nextPixels=pixels(next),passPixels=pixels(passthrough);
      let classicPassDifferences=0;for(let i=0;i<nextPixels.length;i++)if(nextPixels[i]!==passPixels[i])classicPassDifferences++;
      const image=new Image();image.src=source.toDataURL();await image.decode();previewImg=image;
      document.getElementById('previewWrap').style.display='grid';renderPreview();
      const blob=await buildUploadBlobFromPreview(),bitmap=await createImageBitmap(blob),exported=canvas(bitmap.width,bitmap.height);
      exported.getContext('2d').drawImage(bitmap,0,0);bitmap.close();
      const a=pixels(document.getElementById('epdPreview')),b=pixels(exported);let differences=0;
      for(let i=0;i<a.length;i++)if(a[i]!==b[i])differences++;
      const zero=quant(source,{...opt,dither:0}),gray=quant(source,{...opt,saturation:0});
      let grayColored=0;const gd=pixels(gray);for(let p=0;p<gd.length;p+=4)if(gd[p]!==gd[p+1]||gd[p]!==gd[p+2])grayColored++;
      const tiny=canvas(3,2),td=tiny.getContext('2d').createImageData(3,2);
      td.data.set([255,0,0,0,0,0,0,255,255,255,255,255,255,255,0,0,255,255,255,255,0,0,0,255]);
      tiny.getContext('2d').putImageData(td,0,0);
      const transparent=pixels(quant(tiny,opt)),inverted=pixels(quant(tiny,{...opt,invert:true}));
      const tinyOk=transparent[0]===255&&transparent[4]===0&&transparent[8]===255&&inverted[0]===0&&inverted[4]===255&&inverted[8]===0;
      window.jd75Fixture={source,quant,pixels,opt};
      return {old:measure(old),next:measure(next),nearest:measure(zero),texture,repeatDifferences,grayColored,tinyOk,differences,classicPassDifferences,elapsedMs,options:opt,
        source:source.toDataURL(),oldPng:old.toDataURL(),newPng:next.toDataURL(),standardPng:standard.toDataURL(),nearestPng:zero.toDataURL()};
    });
    await download('jd75-color-source.png',result.source,'image/png');
    await download('jd75-color-classic.png',result.oldPng,'image/png');
    await download('jd75-color-fs.png',result.newPng,'image/png');
    await download('jd75-color-standard.png',result.standardPng,'image/png');
    await download('jd75-color-no-dither.png',result.nearestPng,'image/png');
    check('color photo preset selects full palette diffusion',result.options.colorMethod==='colorfs'&&result.options.palette==='color');
    check('only firmware passthrough palette',result.next.palette.every(p=>['0,0,0,255','255,255,255,255','192,57,43,255','210,165,24,255'].includes(p)));
    check('neutral gray has no colored ink',result.next.neutralColored===0,{count:result.next.neutralColored});
    check('solid black/white and border preserved',result.next.endpointsChanged===0,{count:result.next.endpointsChanged});
    check('preview and exported PNG identical',result.differences===0,{differentBytes:result.differences});
    check('preprocessed chart survives existing photo preset',result.classicPassDifferences===0,{differentBytes:result.classicPassDifferences});
    check('color mixture error improves over classic',result.next.meanError<result.old.meanError*.8,{old:result.old.meanError,next:result.next.meanError});
    check('diffusion improves over nearest color',result.next.meanError<result.nearest.meanError*.8,{nearest:result.nearest.meanError,next:result.next.meanError});
    check('zero saturation removes colored ink',result.grayColored===0);
    check('tiny buffers transparency and inversion',result.tinyOk);
    check('same image always produces identical dots',result.repeatDifferences===0);
    const standardPeak=result.texture.reduce((s,p)=>s+p.standard.peak,0)/result.texture.length;
    const softPeak=result.texture.reduce((s,p)=>s+p.soft.peak,0)/result.texture.length;
    check('periodic correlation reduced on uniform mixtures',softPeak<standardPeak*.75,{standardPeak,softPeak});
    check('ink coverage stable across dot patterns',result.texture.every(p=>Math.abs(p.soft.mean-p.standard.mean)<.015),{maxDelta:Math.max(...result.texture.map(p=>Math.abs(p.soft.mean-p.standard.mean)))});
    const redLight=result.next.patches.filter(p=>p.row===0&&p.step>0&&p.step<15);
    check('pale red does not turn yellow',redLight.every(p=>p.counts[3]/p.counts.reduce((a,b)=>a+b,0)<.02),{yellowFractions:redLight.map(p=>p.counts[3]/p.counts.reduce((a,b)=>a+b,0))});
    check('in-gamut patch color error bounded',result.next.patches.filter(p=>p.row<6).every(p=>p.error<.025),{maxError:Math.max(...result.next.patches.filter(p=>p.row<6).map(p=>p.error))});
    for(const row of [0,1,2,3,4,5]){
      const p=result.next.patches.filter(p=>p.row===row);
      const channel=row===4?1:0;
      const means=p.map(x=>x.mean[channel]);
      const increasing=row!==0&&row!==2;
      check('row '+row+' monotonic levels',means.every((v,i)=>i===0||(increasing?v>=means[i-1]-.012:v<=means[i-1]+.012)),{means});
    }
    const changed=await page.evaluate(()=>{
      document.getElementById('brightness').value='7';document.getElementById('brightness').dispatchEvent(new Event('input'));
      document.getElementById('jdSaturation').value='83';document.getElementById('jdSaturation').dispatchEvent(new Event('input'));
      const before=previewOpts();loadUploadPreviewOptions();const after=previewOpts();
      return {before,after};
    });
    check('custom parameters retain color method after reload',JSON.stringify(changed.before)===JSON.stringify(changed.after)&&changed.after.colorMethod==='colorfs'&&changed.after.saturation===.83,changed);
    const migration=await page.evaluate(()=>{
      const key=uploadOptionsStorageKey(),saved=localStorage.getItem(key),current=JSON.parse(saved);
      delete current.dotPattern;localStorage.setItem(key,JSON.stringify(current));loadUploadPreviewOptions();
      const legacy=previewOpts().dotPattern;
      localStorage.setItem(key,saved);loadUploadPreviewOptions();const restored=previewOpts().dotPattern;
      document.getElementById('jdDotPattern').value='regular';document.getElementById('jdDotPattern').dispatchEvent(new Event('change'));loadUploadPreviewOptions();
      const selected=previewOpts().dotPattern;
      applyPreset('colorphoto',false);return {legacy,restored,selected,fresh:previewOpts().dotPattern};
    });
    check('legacy standard and explicit dot pattern persist',migration.legacy==='regular'&&migration.restored==='soft'&&migration.selected==='regular'&&migration.fresh==='soft',migration);
    const merged=await page.evaluate(()=>{
      const key=uploadOptionsStorageKey(),saved=localStorage.getItem(key),cases=[];
      try{
        for(const version of [undefined,1,2]){
          localStorage.setItem(key,JSON.stringify({preset:'photo',quantVersion:version,contrast:160,dither:40,colorMethod:'classic'}));
          loadUploadPreviewOptions();cases.push(previewOpts());
        }
        localStorage.removeItem(key);loadUploadPreviewOptions();cases.push(previewOpts());
        const old=document.querySelector('[data-preset="photo"]'),current=document.querySelector('[data-preset="colorphoto"]');
        return {cases,hidden:old.hidden,label:current.textContent,visible:!current.hidden};
      }finally{localStorage.setItem(key,saved);loadUploadPreviewOptions();}
    });
    check('one color preset and retired photo migration',merged.hidden&&merged.visible&&merged.label==='色彩模式'&&merged.cases.every(o=>o.preset==='colorphoto'&&o.colorMethod==='colorfs'&&o.dotPattern==='soft'&&o.contrast===1&&o.dither===1),merged);
    for(const id of [37,44,43]){
      panel=id;await page.evaluate(async()=>{await refresh();});
      const isolation=await page.evaluate(()=>{
        const {source,quant,pixels,opt}=jd75Fixture;
        const a=pixels(quant(source,{...opt,colorMethod:'classic'})),b=pixels(quant(source,{...opt,colorMethod:'colorfs'}));
        let differences=0;for(let i=0;i<a.length;i++)if(a[i]!==b[i])differences++;
        return {differences,hidden:document.querySelector('[data-preset="colorphoto"]').hidden,method:previewOpts().colorMethod,dotHidden:document.getElementById('jdDotPatternCtrl').hidden};
      });
      check(id+' output isolation and hidden color mode',isolation.differences===0&&isolation.hidden&&isolation.dotHidden&&isolation.method==='classic',isolation);
    }
    panel=34;await page.evaluate(async()=>{await refresh();applyPreset('colorphoto',false);renderPreview();});
    for(const [width,height] of [[1440,900],[390,844],[360,800]]){
      await page.setViewportSize({width,height});
      const fit=await page.evaluate(()=>{
        setPreviewView('fit');const c=document.getElementById('epdPreview'),f=document.getElementById('epdPreviewFit'),style=getComputedStyle(f);
        return {rendering:style.imageRendering,objectFit:style.objectFit,visible:!f.hidden&&c.hidden,data:c.toDataURL(),options:JSON.stringify(previewOpts())};
      });
      await page.locator('#previewViewSeg [data-view="pixels"]').click();
      const native=await page.evaluate(async()=>{
        const c=document.getElementById('epdPreview'),stage=c.parentElement,blob=await buildUploadBlobFromPreview(),bitmap=await createImageBitmap(blob);
        const out=document.createElement('canvas');out.width=bitmap.width;out.height=bitmap.height;out.getContext('2d').drawImage(bitmap,0,0);bitmap.close();
        return {width:c.getBoundingClientRect().width,height:c.getBoundingClientRect().height,viewportWidth:stage.clientWidth,rendering:getComputedStyle(c).imageRendering,scrollable:stage.scrollWidth>stage.clientWidth,overflow:document.documentElement.scrollWidth>innerWidth,data:c.toDataURL(),exported:out.toDataURL(),options:JSON.stringify(previewOpts())};
      });
      check(width+' smooth fit and original pixel view',fit.visible&&fit.rendering==='auto'&&fit.objectFit==='contain'&&native.width===800&&native.height===480&&native.rendering==='pixelated'&&native.scrollable===(native.width>native.viewportWidth)&&!native.overflow);
      check(width+' view switch never changes upload pixels or settings',native.data===fit.data&&native.exported===fit.data&&native.options===fit.options);
      await page.screenshot({path:'output/playwright/jd75-native-ui-'+width+'.png',fullPage:true});
      await page.locator('#previewViewSeg [data-view="fit"]').click();
      await page.locator('#previewExpand').click();
      const expanded=await page.evaluate(()=>{
        paintFittedPreview();const dialog=document.getElementById('previewDialog'),fit=document.getElementById('epdPreviewFit'),stage=fit.parentElement;
        return {open:dialog.open,width:dialog.getBoundingClientRect().width,fitWidth:fit.getBoundingClientRect().width,stageWidth:stage.clientWidth,bodyLocked:document.body.classList.contains('preview-expanded'),count:document.querySelectorAll('#epdPreview').length};
      });
      check(width+' expanded preview is wide and modal',expanded.open&&expanded.width>=width-48&&expanded.fitWidth<=expanded.stageWidth+.1&&expanded.bodyLocked&&expanded.count===1,expanded);
      await page.screenshot({path:'output/playwright/jd75-expanded-ui-'+width+'.png'});
      await page.keyboard.press('Escape');
      check(width+' Escape restores preview and scrolling',await page.evaluate(()=>!document.getElementById('previewDialog').open&&!document.body.classList.contains('preview-expanded')&&document.getElementById('previewWrap').contains(document.getElementById('epdPreview'))));
      const layout=await page.evaluate(()=>{
        const buttons=[...document.querySelectorAll('#presetSeg button')].filter(b=>!b.hidden);
        return {overflow:document.documentElement.scrollWidth>innerWidth,buttons:buttons.map(b=>({text:b.textContent,h:b.getBoundingClientRect().height,overflow:b.scrollWidth>b.clientWidth}))};
      });
      check(width+' responsive controls',!layout.overflow&&layout.buttons.every(b=>b.h>=44&&!b.overflow),layout);
      await page.screenshot({path:'output/playwright/jd75-color-ui-'+width+'.png',fullPage:true});
    }
    const area=await page.evaluate(()=>{
      const im=new ImageData(8,8);
      for(let y=0;y<8;y++)for(let x=0;x<8;x++){const p=(y*8+x)*4;im.data[p]=im.data[p+1]=im.data[p+2]=(x+y)%2?255:0;im.data[p+3]=255;}
      const single=resamplePreviewArea(im,1,1),odd=resamplePreviewArea(im,3,3);
      const native=document.getElementById('epdPreview'),before=native.toDataURL();
      paintFittedPreview();return {single:[...single.data],oddOpaque:[...odd.data].filter((_,i)=>i%4===3).every(a=>a===255),nativeUnchanged:before===native.toDataURL()};
    });
    check('area average integrates all dots in linear light',area.single[0]===188&&area.single[1]===188&&area.single[2]===188&&area.single[3]===255&&area.oddOpaque&&area.nativeUnchanged,area);
    check('no browser exceptions',errors.length===0,{errors});
    delete result.source;delete result.oldPng;delete result.newPng;delete result.standardPng;delete result.nearestPng;
    const report={finishedAt:new Date().toISOString(),checks,passed:checks.filter(c=>c.pass).length,failed:checks.filter(c=>!c.pass),metrics:result};
    await download('jd75-color-report.json',JSON.stringify(report,null,2),'application/json');
    if(report.failed.length)throw new Error(JSON.stringify(report.failed));
    return {passed:report.passed,elapsedMs:result.elapsedMs,oldError:result.old.meanError,newError:result.next.meanError,report:'output/playwright/jd75-color-report.json'};
  }finally{
    await page.evaluate(saved=>{
      Object.keys(localStorage).filter(k=>k.startsWith('upload_img_opts')||k==='epd_onboard_done').forEach(k=>localStorage.removeItem(k));
      Object.entries(saved).filter(([k])=>k.startsWith('upload_img_opts')||k==='epd_onboard_done').forEach(([k,v])=>localStorage.setItem(k,v));
      delete window.jd75Fixture;
    },saved);
    page.removeListener('pageerror',onError);await page.unroute('**/*',intercept);
  }
}
