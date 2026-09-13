/* Shared window/Worker image processor. No DOM or device writes. */
(function(scope){
'use strict';
const limits={brightness:[-80,80,0],contrast:[40,260,100],gamma:[55,180,100],shadows:[0,100,0],saturation:[0,160,100],sharp:[0,100,0],dither:[0,160,100],threshold:[40,210,128],neutralProtection:[0,100,0]};
function defaults(mode='color'){return {...Object.fromEntries(Object.entries(limits).map(([k,v])=>[k,v[2]])),dotPattern:'soft',...(mode==='text'?{dither:0,sharp:0}:{})};}
function tuning(value={},mode='color'){
  const out=defaults(mode);
  for(const [k,[lo,hi,fallback]] of Object.entries(limits)){const n=value[k]==null?NaN:Number(value[k]);out[k]=Number.isFinite(n)?Math.min(hi,Math.max(lo,n)):fallback;}
  out.dotPattern=value.dotPattern==='regular'?'regular':'soft';
  if(mode==='text'){out.dither=0;out.sharp=0;out.shadows=0;}
  if(mode!=='color')out.saturation=0;
  return out;
}
function composition(value={}){
  const number=(v,d)=>Number.isFinite(Number(v))?Number(v):d;
  return {fit:['contain','cover','stretch'].includes(value.fit)?value.fit:'contain',rot:[0,90,180,270].includes(+value.rot)?+value.rot:0,flipX:!!value.flipX,flipY:!!value.flipY,
    cx:Math.max(0,Math.min(1,number(value.cx,.5))),cy:Math.max(0,Math.min(1,number(value.cy,.5))),zoom:Math.max(1,Math.min(4,number(value.zoom,1)))};
}
function crop(iw,ih,w,h,value){
  const c=composition(value),sw=c.rot%180?ih:iw,sh=c.rot%180?iw:ih;
  let cw=sw,ch=sh,dx=0,dy=0,dw=w,dh=h;
  if(c.fit==='cover'){const scale=Math.max(w/sw,h/sh)*c.zoom;cw=w/scale;ch=h/scale;}
  else if(c.fit==='contain'){const scale=Math.min(w/sw,h/sh);dw=sw*scale;dh=sh*scale;dx=(w-dw)/2;dy=(h-dh)/2;}
  c.cx=c.fit==='cover'?Math.max(cw/sw/2,Math.min(1-cw/sw/2,c.cx)):.5;
  c.cy=c.fit==='cover'?Math.max(ch/sh/2,Math.min(1-ch/sh/2,c.cy)):.5;
  return {...c,sw,sh,cw,ch,dx,dy,dw,dh};
}
function clamp255(v){return Math.max(0,Math.min(255,v));}
function correctedChannel(v,opt){return Math.pow(clamp255(v+opt.brightness)/255,1/Math.max(.35,Math.min(2.4,opt.gamma||1)))*255;}
const JD75_COLOR_PALETTE=[[0,0,0],[255,255,255],[192,57,43],[210,165,24]];
function jd75Linear(v){v/=255;return v<=0.04045?v/12.92:Math.pow((v+0.055)/1.055,2.4);}
const JD75_LINEAR_PALETTE=JD75_COLOR_PALETTE.map(c=>c.map(jd75Linear));
const JD75_INK_LUMA=JD75_LINEAR_PALETTE.map(c=>.2126*c[0]+.7152*c[1]+.0722*c[2]);

function smoothUnit(value){const t=Math.max(0,Math.min(1,value));return t*t*(3-2*t);}
function neutralConfidence(r,g,b){
  const max=Math.max(r,g,b),chroma=max-Math.min(r,g,b),saturation=max>0?chroma/max:0;
  // Smooth transitions retain pale colors; relative saturation protects dark inks.
  return (1-smoothUnit((chroma-16)/80))*(1-smoothUnit((saturation-.25)/.5));
}

function protectWarmCoverage(coverage,amount){
  const red=coverage[2]*amount,yellow=coverage[3]*amount;
  const white=red*JD75_INK_LUMA[2]+yellow*JD75_INK_LUMA[3];
  coverage[2]-=red;coverage[3]-=yellow;
  // Replace only excess warm coverage with a B/W mixture of equal model luminance.
  coverage[1]+=white;coverage[0]+=red+yellow-white;
}

// Affine projections onto the palette tetrahedron, its faces, edges and vertices.
// The tiny Gram matrices are inverted once, not for each image pixel.
const JD75_COLOR_HULL=Array.from({length:15},(_,index)=>{
  const ids=[0,1,2,3].filter(k=>(index+1)&(1<<k)),origin=JD75_LINEAR_PALETTE[ids[0]];
  const basis=ids.slice(1).map(k=>JD75_LINEAR_PALETTE[k].map((v,c)=>v-origin[c]));
  const n=basis.length,metric=[.2126,.7152,.0722];
  const matrix=basis.map((b,i)=>[...basis.map(a=>a.reduce((sum,v,c)=>sum+v*b[c]*metric[c],0)),...basis.map((_,j)=>i===j?1:0)]);
  for(let col=0;col<n;col++){
    let pivot=col;
    for(let row=col+1;row<n;row++)if(Math.abs(matrix[row][col])>Math.abs(matrix[pivot][col]))pivot=row;
    [matrix[col],matrix[pivot]]=[matrix[pivot],matrix[col]];
    const scale=matrix[col][col];
    for(let j=0;j<2*n;j++)matrix[col][j]/=scale;
    for(let row=0;row<n;row++)if(row!==col){
      const factor=matrix[row][col];for(let j=0;j<2*n;j++)matrix[row][j]-=factor*matrix[col][j];
    }
  }
  const project=basis.map((_,i)=>[0,1,2].map(c=>basis.reduce((sum,b,j)=>sum+matrix[i][n+j]*b[c]*metric[c],0)));
  return {ids,origin,project};
}).reverse();

function jd75InkCoverage(r,g,b,out,scratch,hasRed=true,hasYellow=true){
  let bestError=Infinity;
  for(const face of JD75_COLOR_HULL){
    if(!hasRed&&face.ids.includes(2)||!hasYellow&&face.ids.includes(3))continue;
    scratch.fill(0);
    let remaining=1,valid=true;
    for(let j=0;j<face.project.length;j++){
      const p=face.project[j],v=p[0]*(r-face.origin[0])+p[1]*(g-face.origin[1])+p[2]*(b-face.origin[2]);
      if(v<-.000001){valid=false;break;}
      scratch[face.ids[j+1]]=Math.max(0,v);remaining-=Math.max(0,v);
    }
    if(!valid||remaining<-.000001)continue;
    scratch[face.ids[0]]=Math.max(0,remaining);
    let rr=0,gg=0,bb=0;
    for(let k=0;k<4;k++){const p=JD75_LINEAR_PALETTE[k];rr+=scratch[k]*p[0];gg+=scratch[k]*p[1];bb+=scratch[k]*p[2];}
    const error=.2126*(r-rr)**2+.7152*(g-gg)**2+.0722*(b-bb)**2;
    if(error<bestError){bestError=error;out.set(scratch);}
    if(error<1e-12)break;
  }
}

// Spatially stable threshold modulation; never alter the propagated color error.
function jd75DotJitter(x,y,ink){
  let h=Math.imul(x+1,0x1f123bb5)^Math.imul(y+1,0x5f356495)^Math.imul(ink+1,0x6c8e9cf5);
  h=Math.imul(h^(h>>>16),0x7feb352d);h=Math.imul(h^(h>>>15),0x846ca68b);
  return ((h^(h>>>16))>>>0)/4294967296-.5;
}

function process(im,input={}){
  if(!im||!Number.isInteger(im.width)||!Number.isInteger(im.height)||im.width<1||im.height<1||im.width*im.height>2000000||!im.data||im.data.length!==im.width*im.height*4)throw new Error('Invalid image buffer');
  const mode=['color','mono','text'].includes(input.mode)?input.mode:'color',p=tuning(input,mode);
  const opt={...p,invert:!!input.invert,contrast:p.contrast/100,gamma:p.gamma/100,saturation:mode==='color'?p.saturation/100:0,sharpness:mode==='text'?0:p.sharp/100,dither:mode==='text'?0:p.dither/100};
  const d=im.data,w=im.width,h=im.height,n=w*h;
  const rgb=new Float32Array(n*3),neutral=new Uint8Array(n),lut=new Float32Array(256);
  const contrast=Math.max(0.2,Math.min(3,opt.contrast));
  const saturation=Math.max(0,Math.min(1.6,opt.saturation==null?1:opt.saturation));
  const strength=Math.max(0,Math.min(1.6,opt.dither));
  const modulation=opt.dotPattern==='soft'?0.65*Math.min(1,strength):0;
  const protection=mode==='color'?p.neutralProtection/100:0;
  const neutralWeight=protection>0?new Uint8Array(n):null;
  for(let v=0;v<256;v++)lut[v]=jd75Linear(clamp255((correctedChannel(v,opt)-128)*contrast+128));
  for(let i=0;i<n;i++){
    const p=i*4,q=i*3,a=d[p+3]/255;
    let r=d[p]*a+255*(1-a),g=d[p+1]*a+255*(1-a),b=d[p+2]*a+255*(1-a);
    if(opt.invert){r=255-r;g=255-g;b=255-b;}
    if(neutralWeight)neutralWeight[i]=Math.round(neutralConfidence(r,g,b)*255);
    // Original neutral text/gray cannot acquire colored dots from its neighbours.
    neutral[i]=Math.max(r,g,b)-Math.min(r,g,b)<=2||saturation===0?1:0;
    r=lut[Math.round(r)];g=lut[Math.round(g)];b=lut[Math.round(b)];
    const y=.2126*r+.7152*g+.0722*b;
    rgb[q]=neutral[i]?y:Math.max(0,Math.min(1,y+(r-y)*saturation));
    rgb[q+1]=neutral[i]?y:Math.max(0,Math.min(1,y+(g-y)*saturation));
    rgb[q+2]=neutral[i]?y:Math.max(0,Math.min(1,y+(b-y)*saturation));
    const lift=mode==='text'?0:opt.shadows/100*y*(1-y)*(1-y);
    for(let c=0;c<3;c++)rgb[q+c]=Math.min(1,rgb[q+c]+lift);
  }
  if(mode==='text'){
    const threshold=jd75Linear(p.threshold);
    for(let i=0;i<n;i++){const value=rgb[i*3]<threshold?0:255;d[i*4]=d[i*4+1]=d[i*4+2]=value;d[i*4+3]=255;}
    return im;
  }
  if(opt.sharpness>0){
    const luma=new Float32Array(n),amount=Math.min(1.2,opt.sharpness);
    for(let i=0;i<n;i++)luma[i]=.2126*rgb[i*3]+.7152*rgb[i*3+1]+.0722*rgb[i*3+2];
    for(let y=1;y<h-1;y++)for(let x=1;x<w-1;x++){
      const i=y*w+x;
      if(luma[i]===0||luma[i]===1)continue;
      if(neutral[i]&&(!neutral[i-1]||!neutral[i+1]||!neutral[i-w]||!neutral[i+w]))continue;
      const delta=amount*(4*luma[i]-luma[i-1]-luma[i+1]-luma[i-w]-luma[i+w]);
      for(let c=0;c<3;c++)rgb[i*3+c]=Math.max(0,Math.min(1,rgb[i*3+c]+delta));
    }
  }
  // Project into the reproducible gamut first, then diffuse ink-coverage error
  // using the existing Floyd-Steinberg 7/3/5/1 rule on alternating rows.
  let current=new Float32Array((w+2)*4),next=new Float32Array((w+2)*4);
  const coverage=new Float32Array(4),scratch=new Float32Array(4),value=new Float32Array(4);
  for(let y=0;y<h;y++){
    const step=(y&1)?-1:1;
    for(let x=step===1?0:w-1;x>=0&&x<w;x+=step){
      const i=y*w+x,q=i*3,p=i*4,e=(x+1)*4;
      if(neutral[i]){
        coverage[0]=1-rgb[q];coverage[1]=rgb[q];coverage[2]=coverage[3]=0;
      }else jd75InkCoverage(rgb[q],rgb[q+1],rgb[q+2],coverage,scratch,
        !input.inks||input.inks.includes('red'),!input.inks||input.inks.includes('yellow'));
      if(neutralWeight&&neutralWeight[i])protectWarmCoverage(coverage,protection*neutralWeight[i]/255);
      let best=0,bestValue=-Infinity;
      for(let k=0;k<4;k++){
        value[k]=Math.max(-1,Math.min(2,coverage[k]+current[e+k]));
        const decision=value[k]+(modulation?modulation*jd75DotJitter(x,y,k):0);
        if(coverage[k]>0&&decision>bestValue){bestValue=decision;best=k;}
      }
      const ink=JD75_COLOR_PALETTE[best];
      d[p]=ink[0];d[p+1]=ink[1];d[p+2]=ink[2];d[p+3]=255;
      for(let k=0;k<4;k++){
        // Drop ink errors at source boundaries that cannot use that ink.
        const error=coverage[k]===0||coverage[k]===1?0:Math.max(-1,Math.min(1,value[k]-(k===best?1:0)))*strength;
        current[e+step*4+k]+=error*7/16;
        next[e-step*4+k]+=error*3/16;next[e+k]+=error*5/16;next[e+step*4+k]+=error/16;
      }
    }
    const swap=current;current=next;next=swap;next.fill(0);
  }
  return im;
}


const E6_FS_NEAR_PALETTE = [[2,2,2],[190,200,200],[205,202,0],[135,19,0],[5,64,158],[39,102,60]];
const E6_FS_OUT_PALETTE = [[0,0,0],[255,255,255],[255,232,0],[196,36,28],[20,80,185],[40,150,90]];
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

const INKS={bw:['black','white'],bwr:['black','white','red'],bwy:['black','white','yellow'],bwry:['black','white','red','yellow'],e6:['black','white','red','yellow','blue','green']};
const INK_RGB={black:[0,0,0],white:[255,255,255],red:[192,57,43],yellow:[210,165,24]};
function profileFromStatus(status){
  const s=status||{},p=s.image_profile,types=INKS[p?.color_type];
  if(!Number.isInteger(s.panel)||s.panel<0||!Number.isInteger(s.panel_width)||!Number.isInteger(s.panel_height)||s.panel_width<1||s.panel_height<1||s.panel_width*s.panel_height>2000000||!types||JSON.stringify(p.inks)!==JSON.stringify(types))return null;
  const special={34:'jd79665-v1',37:'jd79668-v1',43:'e6-v1',44:'ssd2677-v1'};
  const expected=special[s.panel]||(p.color_type==='bw'?'mono-v1':p.color_type==='bwr'||p.color_type==='bwy'?'coverage-three-v1':'coverage-four-v1');
  if(p.id!==expected||p.color_type==='e6'&&s.panel!==43||s.panel===43&&p.color_type!=='e6'||[34,37,44].includes(s.panel)&&p.color_type!=='bwry')return null;
  return {id:s.panel,width:s.panel_width,height:s.panel_height,family:p.color_type,profileId:p.id,inks:[...p.inks],name:String(s.panel_name||'')};
}
function profileDefaults(profile,mode){
  const p=defaults(mode);
  if(mode==='color'&&profile.id===34)p.neutralProtection=35;
  if(mode==='color'&&![34,37].includes(profile.id))p.dotPattern='regular';
  return p;
}
function profilePalette(profile){
  if(profile.family==='e6')return [[0,0,0],[255,255,255],[196,36,28],[255,232,0],[20,80,185],[40,150,90]];
  return profile.inks.map(k=>INK_RGB[k]);
}
function processProfile(im,profile,params={}){
  const valid=profile&&profileFromStatus({panel:profile.id,panel_width:profile.width,panel_height:profile.height,image_profile:{id:profile.profileId,color_type:profile.family,inks:profile.inks}});
  if(!valid||im?.width!==profile.width||im?.height!==profile.height||im?.data?.length!==profile.width*profile.height*4)throw Error('Invalid panel image contract');
  const mode=['color','mono','text'].includes(params.mode)?params.mode:profile.family==='bw'?'mono':'color';
  if(mode==='color'&&profile.family==='bw')throw Error('Color mode unavailable');
  const p={...profileDefaults(profile,mode),...params,mode,inks:profile.inks};
  if(mode==='color'&&profile.family==='e6'){
    const clean=tuning(p,mode),opt={...clean,contrast:clean.contrast/100,gamma:clean.gamma/100,dither:clean.dither/100};
    applySharpen(im.data,im.width,im.height,clean.sharp/100);
    ditherPreviewE6SixColor({putImageData:()=>{}},im,opt);
    return im;
  }
  return process(im,p);
}
const api={process,processProfile,profileFromStatus,profileDefaults,profilePalette,defaults,tuning,composition,crop,linear:jd75Linear,palette:JD75_COLOR_PALETTE};
scope.ImageProcessor=api;
if(typeof module!=='undefined'&&module.exports)module.exports=api;
if(typeof document==='undefined'&&typeof scope.postMessage==='function'){
  scope.onmessage=event=>{
    const job=event.data;
    try{
      const input={width:job.width,height:job.height,data:new Uint8ClampedArray(job.buffer)};
      const im=job.profile?processProfile(input,job.profile,{...job.params,mode:job.mode}):process(input,job.options);
      scope.postMessage({epoch:job.epoch,revision:job.revision,panel:job.panel,profileId:job.profileId,width:im.width,height:im.height,buffer:im.data.buffer},[im.data.buffer]);
    }catch(error){scope.postMessage({epoch:job.epoch,revision:job.revision,panel:job.panel,profileId:job.profileId,error:String(error.message||error)});}
  };
}
})(typeof self!=='undefined'?self:globalThis);
