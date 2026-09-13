/* Frozen pre-workspace color algorithms; regression only, not embedded. */
(function(){let epdPanelId,epdPanelHasRed,epdPanelHasYellow,epdPanelIsE6,epdPanelW,epdPanelH;
const E6_FS_NEAR_PALETTE = [[2,2,2],[190,200,200],[205,202,0],[135,19,0],[5,64,158],[39,102,60]];
const E6_FS_OUT_PALETTE = [[0,0,0],[255,255,255],[255,232,0],[196,36,28],[20,80,185],[40,150,90]];
function isE6PreviewPanel(){return epdPanelId===43;}
function isJdFourColorPreviewPanel(){return epdPanelId===34||epdPanelId===37;}
function clamp255(v){return Math.max(0,Math.min(255,v));}
function correctedChannel(v,opt){
  v=Math.max(0,Math.min(255,v+opt.brightness));
  const gamma=Math.max(0.35,Math.min(2.4,opt.gamma||1));
  return Math.pow(v/255,1/gamma)*255;
}
function applySharpen(data,w,h,amount){
  if(amount<=0) return;
  const src=new Uint8ClampedArray(data);
  const a=Math.min(1.2,amount);
  for(let y=1;y<h-1;y++){
    for(let x=1;x<w-1;x++){
      const p=(y*w+x)*4;
      for(let c=0;c<3;c++){
        const center=src[p+c]*5;
        const around=src[p-4+c]+src[p+4+c]+src[p-w*4+c]+src[p+w*4+c];
        const sharp=center-around;
        data[p+c]=Math.max(0,Math.min(255,src[p+c]*(1-a)+sharp*a));
      }
    }
  }
}
function nearestE6FsColor(r,g,b){
  let best=0,bestErr=Infinity;
  for(let i=0;i<E6_FS_NEAR_PALETTE.length;i++){
    const p=E6_FS_NEAR_PALETTE[i];
    const dr=r-p[0],dg=g-p[1],db=b-p[2];
    const err=dr*dr+dg*dg+db*db;
    if(err<bestErr){bestErr=err;best=i;}
  }
  return best;
}
function ditherPreviewE6SixColor(ctx,im,opt){
  const d=im.data,w=im.width,h=im.height;
  const contrast=Math.max(0.2,Math.min(3,opt.contrast));
  const strength=Math.max(0,Math.min(2,opt.dither));
  const rb=new Float32Array(w*h);
  const gb=new Float32Array(w*h);
  const bb=new Float32Array(w*h);
  for(let i=0;i<w*h;i++){
    let r=d[i*4],g=d[i*4+1],b=d[i*4+2],a=d[i*4+3];
    if(a<128){r=255;g=255;b=255;}
    if(opt.invert){r=255-r;g=255-g;b=255-b;}
    rb[i]=clamp255((correctedChannel(r,opt)-128)*contrast+128);
    gb[i]=clamp255((correctedChannel(g,opt)-128)*contrast+128);
    bb[i]=clamp255((correctedChannel(b,opt)-128)*contrast+128);
  }
  for(let y=0;y<h;y++){
    for(let x=0;x<w;x++){
      const idx=y*w+x,p=idx*4;
      const r=clamp255(rb[idx]),g=clamp255(gb[idx]),b=clamp255(bb[idx]);
      const c=nearestE6FsColor(r,g,b);
      const out=E6_FS_OUT_PALETTE[c];
      d[p]=out[0];d[p+1]=out[1];d[p+2]=out[2];d[p+3]=255;
      if(strength<=0.001) continue;
      const near=E6_FS_NEAR_PALETTE[c];
      const er=(r-near[0])*strength,eg=(g-near[1])*strength,eb=(b-near[2])*strength;
      const add=(ni,wgt)=>{rb[ni]+=er*wgt/16;gb[ni]+=eg*wgt/16;bb[ni]+=eb*wgt/16;};
      if(x+1<w)add(idx+1,7);
      if(y+1<h){
        if(x>0)add(idx+w-1,3);
        add(idx+w,5);
        if(x+1<w)add(idx+w+1,1);
      }
    }
  }
  ctx.putImageData(im,0,0);
}
function ditherPreviewJd(ctx,im,opt){
  const d=im.data,w=im.width,h=im.height;
  const inkColors=[[0,0,0],[192,57,43],[210,165,24]];
  const tone=new Float32Array(w*h),ink=new Uint8Array(w*h);
  const threshold=clamp255(opt.threshold);
  const contrast=Math.max(0.2,Math.min(3,opt.contrast));
  const strength=Math.max(0,Math.min(1.6,opt.dither));
  const rs=Math.max(0,Math.min(1,opt.redSensitivity));
  const ys=Math.max(0,Math.min(1,opt.yellowSensitivity));
  const mode=opt.redMode||'normal';
  const bias=mode==='strong'?-18:(mode==='safe'?20:0);
  const mono=opt.palette==='mono';
  // Classify original pixels before diffusion; neutral edges cannot inherit ink.
  for(let i=0;i<w*h;i++){
    const p=i*4,a=d[p+3]/255;
    let r=d[p]*a+255*(1-a),g=d[p+1]*a+255*(1-a),b=d[p+2]*a+255*(1-a);
    if(opt.invert){r=255-r;g=255-g;b=255-b;}
    const chroma=Math.max(r,g,b)-Math.min(r,g,b);
    r=correctedChannel(r,opt);g=correctedChannel(g,opt);b=correctedChannel(b,opt);
    const lum=clamp255(((77*r+150*g+29*b)/256-128)*contrast+128);
    tone[i]=lum;
    if(mono||chroma<=20)continue;
    const redScore=r-Math.max(g,b)*(0.72+rs*0.18)+(r-Math.min(g,b))*0.35-bias;
    const yellowScore=Math.min(r,g)-b*(0.72+ys*0.18)+(Math.min(r,g)-Math.min(r,g,b))*0.30;
    if(epdPanelHasYellow&&yellowScore>86-ys*58&&(lum<238-ys*72||chroma>=64)&&yellowScore>=redScore-8)
      ink[i]=2;
    else if(epdPanelHasRed&&mode!=='off'&&redScore>80-rs*55&&(lum<230-rs*80||chroma>=64))
      ink[i]=1;
  }
  // Sharpen luminance only; colored neighbours must not tint neutral pixels.
  if(opt.sharpness>0){
    const source=new Float32Array(tone),a=Math.min(1.2,opt.sharpness);
    for(let y=1;y<h-1;y++)for(let x=1;x<w-1;x++){
      const i=y*w+x;
      if(ink[i]||ink[i-1]||ink[i+1]||ink[i-w]||ink[i+w]||source[i]===0||source[i]===255)continue;
      tone[i]=clamp255(source[i]+a*(4*source[i]-source[i-1]-source[i+1]-source[i-w]-source[i+w]));
    }
  }
  let current=new Float32Array(w+2),next=new Float32Array(w+2);
  for(let y=0;y<h;y++){
    const step=(y&1)?-1:1;
    for(let x=step===1?0:w-1;x>=0&&x<w;x+=step){
      const i=y*w+x,p=i*4,e=x+1;
      if(ink[i]){
        const color=inkColors[ink[i]];
        d[p]=color[0];d[p+1]=color[1];d[p+2]=color[2];d[p+3]=255;
        continue;
      }
      const lum=tone[i];
      const old=(lum===0||lum===255)?lum:clamp255(lum+current[e]);
      const out=old<threshold?0:255;
      d[p]=d[p+1]=d[p+2]=out;d[p+3]=255;
      const error=(old-out)*strength;
      current[e+step]+=error*7/16;
      next[e-step]+=error*3/16;next[e]+=error*5/16;next[e+step]+=error/16;
    }
    const swap=current;current=next;next=swap;next.fill(0);
  }
  ctx.putImageData(im,0,0);
}
function ditherPreview(canvas,opt){
  const ctx=canvas.getContext('2d');
  const im=ctx.getImageData(0,0,canvas.width,canvas.height);
  const d=im.data,w=canvas.width,h=canvas.height;
  if(epdPanelId===34&&opt.colorMethod==='colorfs'&&opt.palette!=='mono'){
    ditherPreviewJd75Color(ctx,im,opt);
    return;
  }
  if(isJdFourColorPreviewPanel()){
    ditherPreviewJd(ctx,im,opt);
    return;
  }
  applySharpen(d,w,h,opt.sharpness);
  if(isE6PreviewPanel() && opt.e6DitherMode!=='tri'){
    ditherPreviewE6SixColor(ctx,im,opt);
    return;
  }
  const threshold=Math.max(0,Math.min(255,opt.threshold));
  const contrast=Math.max(0.2,Math.min(3,opt.contrast));
  const redSensitivity=Math.max(0,Math.min(1,opt.redSensitivity));
  const yellowSensitivity=Math.max(0,Math.min(1,opt.yellowSensitivity));
  const strength=Math.max(0,Math.min(2,opt.dither));
  const pureBw = strength<=0.001 && (opt.redMode||'normal')==='off';
  const gray=new Float32Array(w*h);
  const redScore=new Float32Array(w*h);
  const yellowScore=new Float32Array(w*h);
  for(let i=0;i<w*h;i++){
    let r=d[i*4],g=d[i*4+1],b=d[i*4+2],a=d[i*4+3];
    if(a<128){r=255;g=255;b=255;}
    if(opt.invert){r=255-r;g=255-g;b=255-b;}
    r=correctedChannel(r,opt);
    g=correctedChannel(g,opt);
    b=correctedChannel(b,opt);
    let y=0.299*r+0.587*g+0.114*b;
    y=Math.max(0,Math.min(255,(y-128)*contrast+128));
    gray[i]=y;
    if(pureBw) continue;
    const mode=opt.redMode||'normal';
    const modeBias=mode==='strong'?-18:(mode==='safe'?20:(mode==='off'?999:0));
    const warm=r-Math.max(g,b)*(0.72+redSensitivity*0.18);
    const saturation=r-Math.min(g,b);
    redScore[i]=warm+saturation*0.35-modeBias;
    const yellowBase=Math.min(r,g)-b*(0.72+yellowSensitivity*0.18);
    const yellowSat=Math.min(r,g)-Math.min(r,g,b);
    yellowScore[i]=yellowBase+yellowSat*0.30;
  }
  if(pureBw){
    for(let i=0;i<w*h;i++){
      const p=i*4;
      const out=gray[i]<threshold?0:255;
      d[p]=d[p+1]=d[p+2]=out;d[p+3]=255;
    }
    ctx.putImageData(im,0,0);
    return;
  }
  const redGate=80-redSensitivity*55;
  const redLightGate=230-redSensitivity*80;
  const yellowGate=86-yellowSensitivity*58;
  const yellowLightGate=238-yellowSensitivity*72;
  const useYellow=epdPanelHasYellow && !(isE6PreviewPanel() && opt.e6DitherMode==='tri');
  for(let y=0;y<h;y++){
    for(let x=0;x<w;x++){
      const idx=y*w+x;
      const p=idx*4;
      const old=Math.max(0,Math.min(255,gray[idx]));
      const isYellow=useYellow&&yellowScore[idx]>yellowGate&&old<yellowLightGate&&yellowScore[idx]>=redScore[idx]-8;
      if(isYellow){
        d[p]=210;d[p+1]=165;d[p+2]=24;d[p+3]=255;
        gray[idx]=255;
        continue;
      }
      const isRed=epdPanelHasRed&&opt.redMode!=='off'&&redScore[idx]>redGate&&old<redLightGate;
      if(isRed){
        d[p]=192;d[p+1]=57;d[p+2]=43;d[p+3]=255;
        gray[idx]=255;
        continue;
      }
      const out=old<threshold?0:255;
      d[p]=d[p+1]=d[p+2]=out;d[p+3]=255;
      const e=(old-out)*strength;
      if(x+1<w) gray[idx+1]+=e*7/16;
      if(y+1<h){
        if(x>0) gray[idx+w-1]+=e*3/16;
        gray[idx+w]+=e*5/16;
        if(x+1<w) gray[idx+w+1]+=e/16;
      }
    }
  }
  ctx.putImageData(im,0,0);
}
self.LegacyProcessor={process(im,profile,p){
epdPanelId=profile.id;epdPanelW=profile.width;epdPanelH=profile.height;epdPanelHasRed=profile.family!=='bw';epdPanelHasYellow=['bwry','e6'].includes(profile.family);epdPanelIsE6=profile.id===43;
const opt={threshold:p.threshold,brightness:p.brightness,contrast:p.contrast/100,gamma:p.gamma/100,sharpness:p.sharp/100,dither:p.dither/100,redSensitivity:p.red/100,yellowSensitivity:p.yellow/100,redMode:'normal',palette:'color',colorMethod:'classic',e6DitherMode:profile.id===43?'six':'tri'};
const ctx={getImageData:()=>im,putImageData:()=>{}};ditherPreview({width:im.width,height:im.height,getContext:()=>ctx},opt);return im;
}};})();