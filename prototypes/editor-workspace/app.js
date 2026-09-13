'use strict';
const $=id=>document.getElementById(id);
const FAMILY={bw:'黑白',bwr:'黑白红',bwry:'黑白红黄',e6:'六色'};
const PALETTES={bw:['#000000','#ffffff'],bwr:['#000000','#ffffff','#c0392b'],bwry:['#000000','#ffffff','#c0392b','#d2a518'],e6:['#000000','#ffffff','#c4241c','#ffe800','#1450b9','#28965a']};
const clone=value=>JSON.parse(JSON.stringify(value));
const FIELDS=[['brightness','亮度',-80,80,''],['contrast','对比度',40,260,'%'],['shadows','暗部',0,100,'%'],['saturation','饱和度',0,160,'%'],['neutralProtection','偏色抑制',0,100,'%'],['gamma','伽马',55,180,''],['sharp','锐化',0,100,'%'],['dither','抖动强度',0,160,'%'],['threshold','文字阈值',40,210,''],['red','红色保留',0,100,'%'],['yellow','黄色保留',0,100,'%']];
const linear=Float32Array.from({length:256},(_,i)=>ImageProcessor.linear(i));
function average(source,width,height){
  const sw=source.width,sh=source.height,src=source.data,mid=new Float32Array(width*sh*3),out=new ImageData(width,height);
  for(let y=0;y<sh;y++)for(let x=0;x<width;x++){
    const a=x*sw/width,b=(x+1)*sw/width,q=(y*width+x)*3;
    for(let sx=Math.floor(a);sx<Math.ceil(b)&&sx<sw;sx++){
      const weight=(Math.min(b,sx+1)-Math.max(a,sx))/(b-a),p=(y*sw+sx)*4;
      for(let k=0;k<3;k++)mid[q+k]+=linear[src[p+k]]*weight;
    }
  }
  for(let y=0;y<height;y++)for(let x=0;x<width;x++){
    const a=y*sh/height,b=(y+1)*sh/height,p=(y*width+x)*4;
    for(let k=0;k<3;k++){
      let v=0;for(let sy=Math.floor(a);sy<Math.ceil(b)&&sy<sh;sy++)v+=mid[(sy*width+x)*3+k]*(Math.min(b,sy+1)-Math.max(a,sy))/(b-a);
      v=Math.max(0,Math.min(1,v));out.data[p+k]=Math.round(255*(v<=.0031308?12.92*v:1.055*v**(1/2.4)-.055));
    }out.data[p+3]=255;
  }return out;
}
function usesFourColor(profile){return profile.id===34||profile.processor==='four-color';}
function defaults(profile,mode){
  const p={...ImageProcessor.defaults(mode),red:32,yellow:32};
  if(mode==='color'&&profile.id===34)p.neutralProtection=35;
  if(mode==='color'&&!usesFourColor(profile)){
    if(profile.id===37)Object.assign(p,{contrast:104,sharp:4,threshold:128,red:34,yellow:36});
    else Object.assign(p,{brightness:4,contrast:130,gamma:92,sharp:12,threshold:108,dither:profile.id===43?100:70});
  }
  return p;
}
const App={
  catalog:[],profile:null,state:null,states:{},epoch:0,revision:0,rendered:-1,ready:-1,
  image:null,fileName:'',url:null,frame:document.createElement('canvas'),original:document.createElement('canvas'),
  worker:null,failedWorker:false,job:null,pending:null,timer:0,sourceRequest:0,fileEpoch:0,loading:false,busy:false,error:'',notice:'',
  history:[],future:[],gesture:null,drag:null,tab:'adjust',tool:'view',compare:false,
  view:{fit:true,zoom:100,kind:'effect'},sidebar:300,paintFrame:0,
  key:'msp.prototype.editor-workspace.v1',
  async init(){
    this.makeControls();this.bind();
    try{
      const saved=JSON.parse(localStorage.getItem(this.key)||'{}');this.states=saved.states||{};this.sidebar=Math.min(380,Math.max(260,Number(saved.sidebar)||300));
    }catch(e){}
    document.documentElement.style.setProperty('--sidebar',this.sidebar+'px');$('splitter').setAttribute('aria-valuenow',this.sidebar);
    try{
      const response=await fetch('/api/panels');if(!response.ok)throw Error('无法读取样稿屏幕列表');
      const list=await response.json();this.catalog=list.filter(p=>Number.isInteger(p.id)&&p.width>0&&p.height>0&&p.width*p.height<=2000000&&PALETTES[p.family]);
      if(!this.catalog.length)throw Error('没有有效屏幕描述');
      $('panelSelect').replaceChildren();
      for(const family of ['bwry','bw','bwr','e6']){
        const group=document.createElement('optgroup');group.label=FAMILY[family];
        for(const p of this.catalog.filter(p=>p.family===family)){const option=document.createElement('option');option.value=p.id;option.textContent=p.name+' · '+p.width+'×'+p.height;group.append(option);} $('panelSelect').append(group);
      }
      $('panelSelect').disabled=false;$('catalogInfo').textContent=this.catalog.length+' 个有效面板描述来自当前驱动表，未修改RAM尺寸、偏移或驱动。';
      const query=new URL(location.href).searchParams.get('panel');this.selectPanel(query!==null?Number(query):34);
      if(!this.image&&!this.loading)await this.sample('photo');
    }catch(e){this.error=e.message;this.controls();}
  },
  makeControls(){
    for(const [i,[key,label,min,max]] of FIELDS.entries()){
      const row=document.createElement('div');row.className='slider-row';row.dataset.field=key;
      row.innerHTML=`<label for="param-${key}">${label}<output id="value-${key}"></output></label><div class="slider-input"><input id="param-${key}" data-param="${key}" type="range" min="${min}" max="${max}"><button class="icon-button reset-control" data-reset="${key}" title="重置${label}" aria-label="重置${label}"><img src="/icons/rotate-ccw.svg" alt=""></button></div>`;
      $(i<4?'basicControls':'advancedControls').append(row);
    }
  },
  bind(){
    const on=(id,event,fn,options)=>$(id).addEventListener(event,fn,options);
    on('openFile','click',()=>$('file').click());on('file','change',e=>{this.sourceRequest++;this.load(e.target.files[0]);});
    on('sampleSelect','change',e=>this.sample(e.target.value));on('panelSelect','change',e=>this.selectPanel(+e.target.value));
    document.querySelectorAll('[data-param]').forEach(el=>{
      el.addEventListener('input',()=>this.change(()=>{this.state.params[this.state.mode][el.dataset.param]=+el.value;}));
      for(const event of ['change','blur'])el.addEventListener(event,()=>this.commit());
    });
    document.querySelectorAll('[data-reset]').forEach(el=>el.addEventListener('click',()=>this.change(()=>{const key=el.dataset.reset;this.state.params[this.state.mode][key]=defaults(this.profile,this.state.mode)[key];},true)));
    document.querySelectorAll('[data-mode]').forEach(el=>el.addEventListener('click',()=>this.change(()=>{this.state.mode=el.dataset.mode;},true)));
    document.querySelectorAll('[data-tab]').forEach(el=>el.addEventListener('click',()=>this.setTab(el.dataset.tab)));
    document.querySelector('.inspector-tabs').addEventListener('keydown',e=>{if(!['ArrowLeft','ArrowRight','Home','End'].includes(e.key))return;e.preventDefault();const tabs=[...document.querySelectorAll('[data-tab]')],i=e.key==='Home'?0:e.key==='End'?1:this.tab==='adjust'?1:0;this.setTab(tabs[i].dataset.tab);tabs[i].focus();});
    for(const [id,label] of [['adjustTab','tabAdjust'],['composeTab','tabCompose']]){$(id).setAttribute('role','tabpanel');$(id).setAttribute('aria-labelledby',label);}
    document.querySelectorAll('[data-view]').forEach(el=>el.addEventListener('click',()=>{this.view.kind=el.dataset.view;this.view.fit=el.dataset.view==='effect';if(!this.view.fit)this.view.zoom=100;this.controls();this.paint();}));
    document.querySelectorAll('[data-rot]').forEach(el=>el.addEventListener('click',()=>this.change(()=>{this.state.composition.rot=+el.dataset.rot;},true)));
    on('fitMode','change',e=>this.change(()=>{this.state.composition.fit=e.target.value;this.tool=e.target.value==='cover'?'crop':'view';},true));
    on('cropZoom','input',e=>this.change(()=>{this.state.composition.zoom=+e.target.value;}));for(const event of ['change','blur'])on('cropZoom',event,()=>this.commit());
    on('resetCropZoom','click',()=>this.change(()=>{this.state.composition.zoom=1;},true));
    on('centerCrop','click',()=>this.change(()=>{this.state.composition.cx=this.state.composition.cy=.5;},true));
    on('flipX','change',e=>this.change(()=>{this.state.composition.flipX=e.target.checked;},true));on('flipY','change',e=>this.change(()=>{this.state.composition.flipY=e.target.checked;},true));
    on('pattern','change',e=>this.change(()=>{this.state.params[this.state.mode].dotPattern=e.target.value;},true));
    on('panTool','click',()=>{this.tool='view';this.controls();});on('cropTool','click',()=>{this.tool='crop';this.setTab('compose');this.controls();});
    on('fitView','click',()=>{this.view.fit=true;this.paint();});on('actualView','click',()=>this.zoom(100));
    on('zoomSelect','change',e=>{if(e.target.value==='fit'){this.view.fit=true;this.paint();}else this.zoom(+e.target.value);});
    on('zoomIn','click',()=>this.zoom((this.scale||1)*125));on('zoomOut','click',()=>this.zoom((this.scale||1)*80));
    on('undo','click',()=>this.undo());on('redo','click',()=>this.redo());
    on('reset','click',()=>this.change(()=>{this.state.params[this.state.mode]=defaults(this.profile,this.state.mode);this.state.composition=ImageProcessor.composition();this.view.fit=true;},true));
    on('resetTab','click',()=>this.change(()=>{if(this.tab==='compose')this.state.composition=ImageProcessor.composition();else this.state.params[this.state.mode]=defaults(this.profile,this.state.mode);},true));
    on('toggleSidebar','click',()=>{const closed=document.body.classList.toggle('sidebar-closed');$('toggleSidebar').setAttribute('aria-expanded',String(!closed));$('toggleSidebar').title=closed?'展开参数栏':'收起参数栏';$('toggleSidebar').setAttribute('aria-label',$('toggleSidebar').title);$('toggleSidebar').querySelector('img').src='/icons/panel-right-'+(closed?'open':'close')+'.svg';this.schedulePaint();});
    on('splitter','pointerdown',e=>{if(e.button!==0)return;$('splitter').setPointerCapture(e.pointerId);this.splitDrag={x:e.clientX,width:$('inspector').getBoundingClientRect().width,id:e.pointerId};e.preventDefault();});
    on('splitter','pointermove',e=>{if(this.splitDrag?.id===e.pointerId)this.resizeSidebar(this.splitDrag.width+this.splitDrag.x-e.clientX);});
    for(const event of ['pointerup','pointercancel'])on('splitter',event,()=>{this.splitDrag=null;this.save();});
    on('splitter','keydown',e=>{if(['ArrowLeft','ArrowRight'].includes(e.key)){e.preventDefault();this.resizeSidebar(this.sidebar+(e.key==='ArrowLeft'?10:-10));this.save();}});
    on('viewport','pointerdown',e=>this.pointerDown(e));on('viewport','pointermove',e=>this.pointerMove(e));
    for(const event of ['pointerup','pointercancel'])on('viewport',event,e=>this.pointerEnd(e));
    on('viewport','wheel',e=>{if(e.ctrlKey){e.preventDefault();this.zoom((this.scale||1)*100*(e.deltaY>0?.9:1.1));}},{passive:false});
    on('compare','pointerdown',e=>{if($('compare').disabled)return;$('compare').setPointerCapture(e.pointerId);this.setCompare(true);e.preventDefault();});
    for(const event of ['pointerup','pointercancel','blur'])on('compare',event,()=>this.setCompare(false));
    on('compare','keydown',e=>{if([' ','Enter'].includes(e.key)){e.preventDefault();this.setCompare(true);}});on('compare','keyup',()=>this.setCompare(false));
    window.addEventListener('blur',()=>this.setCompare(false));
    window.addEventListener('keydown',e=>{
      if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='z'&&!['INPUT','TEXTAREA'].includes(e.target.tagName)){e.preventDefault();e.shiftKey?this.redo():this.undo();}
      if(e.code==='Space'&&!['INPUT','SELECT','TEXTAREA','BUTTON'].includes(e.target.tagName)){e.preventDefault();this.setCompare(true);}
    });window.addEventListener('keyup',e=>{if(e.code==='Space')this.setCompare(false);});
    on('upload','click',()=>this.upload());on('download','click',()=>this.download());
    on('about','click',()=>$('aboutDialog').showModal());on('closeAbout','click',()=>$('aboutDialog').close());
    new ResizeObserver(()=>this.schedulePaint()).observe($('viewport'));window.addEventListener('resize',()=>this.schedulePaint());
  },
  selectPanel(id){
    if(this.busy)return;this.commit();this.stop();this.epoch++;this.revision++;this.ready=-1;this.rendered=-1;this.history=[];this.future=[];this.gesture=null;this.compare=false;this.tool='view';this.view={fit:true,zoom:100,kind:'effect'};
    this.profile=this.catalog.find(p=>p.id===id)||null;this.error='';this.notice='';
    if(!this.profile){this.state=null;this.error='无效的模拟屏幕';this.controls();this.paint();return;}
    const p=this.profile,stored=this.states[p.profileId],mode=p.family==='bw'?'mono':'color';
    this.state={mode,params:Object.fromEntries(['color','mono','text'].map(m=>[m,defaults(p,m)])),composition:ImageProcessor.composition()};
    if(stored){
      if(['color','mono','text'].includes(stored.mode)&&!(p.family==='bw'&&stored.mode==='color'))this.state.mode=stored.mode;
      for(const m of ['color','mono','text'])if(stored.params?.[m])this.state.params[m]={...defaults(p,m),...ImageProcessor.tuning({...defaults(p,m),...stored.params[m]},m),red:Math.max(0,Math.min(100,Number(stored.params[m].red)||0)),yellow:Math.max(0,Math.min(100,Number(stored.params[m].yellow)||0))};
    }
    $('panelSelect').value=id;this.request();this.controls();this.paint();
  },
  save(){
    if(this.profile&&this.state)this.states[this.profile.profileId]={mode:this.state.mode,params:clone(this.state.params)};
    try{localStorage.setItem(this.key,JSON.stringify({states:this.states,sidebar:this.sidebar}));}catch(e){}
  },
  normalize(){
    const mode=this.state.mode,p=this.state.params[mode],clean=ImageProcessor.tuning(p,mode);
    this.state.params[mode]={...clean,red:Math.max(0,Math.min(100,Number(p.red)||0)),yellow:Math.max(0,Math.min(100,Number(p.yellow)||0))};
    this.state.composition=ImageProcessor.composition(this.state.composition);
    if(this.image){const c=ImageProcessor.crop(this.image.width,this.image.height,this.profile.width,this.profile.height,this.state.composition);this.state.composition.cx=c.cx;this.state.composition.cy=c.cy;}
  },
  change(fn,commit=false){if(this.busy||!this.state)return;if(!this.gesture)this.gesture=clone(this.state);fn();this.normalize();this.request();this.controls();if(commit)this.commit();},
  commit(){if(this.gesture&&JSON.stringify(this.gesture)!==JSON.stringify(this.state)){this.history.push(this.gesture);if(this.history.length>50)this.history.shift();this.future=[];this.save();}this.gesture=null;this.controls();},
  undo(){if(this.busy||!this.state)return;this.commit();if(!this.history.length)return;this.future.push(clone(this.state));this.state=this.history.pop();this.request();this.save();this.controls();},
  redo(){if(this.busy||!this.future.length)return;this.history.push(clone(this.state));this.state=this.future.pop();this.request();this.save();this.controls();},
  setTab(tab){this.tab=tab;$('adjustTab').hidden=tab!=='adjust';$('composeTab').hidden=tab!=='compose';document.querySelectorAll('[data-tab]').forEach(b=>{b.classList.toggle('active',b.dataset.tab===tab);b.setAttribute('aria-selected',String(b.dataset.tab===tab));b.tabIndex=b.dataset.tab===tab?0:-1;});$('resetTab').title=tab==='adjust'?'重置调整':'重置构图';$('resetTab').setAttribute('aria-label',$('resetTab').title);$('parameterScroll').scrollTop=0;},
  supports(key){
    if(!this.state||!this.profile)return false;const m=this.state.mode,p=this.profile;
    if(key==='threshold')return m==='text';if(key==='sharp'||key==='dither')return m!=='text';
    if(key==='shadows')return m!=='text'&&(m!=='color'||usesFourColor(p));
    if(key==='saturation')return m==='color'&&usesFourColor(p);
    if(key==='neutralProtection')return m==='color'&&usesFourColor(p);
    if(key==='red')return m==='color'&&p.family!=='e6'&&!usesFourColor(p);
    if(key==='yellow')return m==='color'&&p.family==='bwry'&&!usesFourColor(p);
    return true;
  },
  controls(){
    const p=this.profile,s=this.state;
    document.querySelectorAll('[data-param],[data-reset],[data-mode],#fitMode,#cropZoom,#flipX,#flipY,#pattern,#reset,#resetTab,#openFile,#sampleSelect,#file').forEach(e=>e.disabled=this.busy||!p);
    $('panelSelect').disabled=this.busy||!this.catalog.length;
    $('undo').disabled=this.busy||!this.history.length;$('redo').disabled=this.busy||!this.future.length;
    const ready=!!this.image&&this.ready===this.revision&&!this.error&&!this.busy&&!!p;
    $('upload').disabled=!ready;$('download').disabled=!ready;$('compare').disabled=this.rendered<0;
    $('status').classList.toggle('error',!!this.error);$('status').textContent=this.error||this.notice||(this.loading?'正在读取图片…':this.busy?'本地验证中…':!this.image?'请选择图片':this.ready!==this.revision?'正在生成预览…':'预览就绪');
    $('fileName').textContent=this.fileName||'未选择图片';$('fileName').title=this.fileName;
    $('empty').hidden=!!this.image||this.loading;
    if(!p||!s)return;
    const values=s.params[s.mode],c=s.composition;
    document.querySelectorAll('[data-mode]').forEach(b=>{b.hidden=b.dataset.mode==='color'&&p.family==='bw';b.classList.toggle('active',b.dataset.mode===s.mode);b.setAttribute('aria-pressed',String(b.dataset.mode===s.mode));});
    for(const [key,,, ,suffix] of FIELDS){const row=document.querySelector('[data-field="'+key+'"]');row.hidden=!this.supports(key);$('param-'+key).value=values[key];$('value-'+key).textContent=key==='gamma'?(values[key]/100).toFixed(2):values[key]+suffix;}
    $('patternRow').hidden=s.mode==='text'||(s.mode==='color'&&!usesFourColor(p));$('pattern').value=values.dotPattern;
    $('fitMode').value=c.fit;$('cropZoom').value=c.zoom;$('cropZoomValue').textContent=c.zoom.toFixed(2)+'×';$('cropZoomRow').hidden=c.fit!=='cover';$('centerCrop').hidden=c.fit!=='cover';
    $('flipX').checked=c.flipX;$('flipY').checked=c.flipY;
    document.querySelectorAll('[data-rot]').forEach(b=>{b.classList.toggle('active',+b.dataset.rot===c.rot);b.disabled=this.busy;});
    if(c.fit!=='cover')this.tool='view';$('cropTool').disabled=c.fit!=='cover'||this.busy||!this.image;
    for(const [id,value] of [['panTool','view'],['cropTool','crop']]){$(id).classList.toggle('active',this.tool===value);$(id).setAttribute('aria-pressed',String(this.tool===value));}
    $('viewport').classList.toggle('crop',this.tool==='crop');
    document.querySelectorAll('[data-view]').forEach(b=>{b.classList.toggle('active',this.view.kind===b.dataset.view);b.setAttribute('aria-pressed',String(this.view.kind===b.dataset.view));});
    $('canvasTitle').textContent=FAMILY[p.family]+' · '+p.width+' × '+p.height;$('geometry').textContent=(this.image?this.image.width+'×'+this.image.height+' → ':'')+p.width+'×'+p.height;
    $('profileLabel').textContent=(p.id===37&&usesFourColor(p)?'4.2四色测试':p.id===34?'当前四色处理':p.family==='bw'?'现有黑白处理':p.id===43?'原有E6六色路径':'原有面板处理路径')+' · ID '+p.id;
    $('modeStatus').textContent='模拟设备 · 不会执行真实刷屏';
    $('inkSwatches').replaceChildren();for(const color of PALETTES[p.family]){const dot=document.createElement('i');dot.style.background=color;dot.title=color;$('inkSwatches').append(dot);}
  },
  async sample(kind){
    if(this.busy)return;this.load(null);const request=++this.sourceRequest;this.loading=true;this.controls();
    const path=kind==='chart'&&this.profile?.id===37?'/assets/chart-37.png':{photo:'/assets/photo.jpg',chart:'/assets/chart.png',qr:'/assets/qr.png'}[kind];
    try{const r=await fetch(path);if(!r.ok)throw Error('素材不可用');const blob=await r.blob();if(request!==this.sourceRequest)return;this.load(blob,{photo:'示例图片.png',chart:'色阶测试.png',qr:'二维码.png'}[kind]);}
    catch(e){if(request!==this.sourceRequest)return;const c=document.createElement('canvas');c.width=800;c.height=480;const x=c.getContext('2d');x.fillStyle='#fff';x.fillRect(0,0,800,480);for(let i=0;i<16;i++){const v=Math.round(i*255/15);x.fillStyle=`rgb(${v},${v},${v})`;x.fillRect(i*50,0,50,240);x.fillStyle=`rgb(192,${Math.round(57+i*8)},43)`;x.fillRect(i*50,240,50,240);}c.toBlob(blob=>{if(request===this.sourceRequest)this.load(blob,'本地色阶.png');});}
  },
  load(file,name){
    if(this.busy)return;this.stop();this.sourceRequest++;this.fileEpoch++;this.epoch++;this.revision++;this.ready=-1;this.rendered=-1;this.image=null;this.error='';this.notice='';this.loading=!!file;this.compare=false;this.history=[];this.future=[];this.gesture=null;
    if(this.state)this.state.composition=ImageProcessor.composition();this.view={fit:true,zoom:100,kind:'effect'};this.tool='view';this.fileName=name||file?.name||'';
    if(this.url)URL.revokeObjectURL(this.url);this.url=null;this.controls();this.paint();if(!file)return;
    const epoch=this.fileEpoch,img=new Image();this.url=URL.createObjectURL(file);
    img.onload=()=>{if(epoch!==this.fileEpoch)return;this.loading=false;if(!img.width||!img.height||img.width*img.height>24000000){this.error='图片过大或尺寸无效';this.controls();return;}this.image=img;this.request();};
    img.onerror=()=>{if(epoch===this.fileEpoch){this.loading=false;this.error='图片读取失败，请重新选择图片';this.controls();}};img.src=this.url;
  },
  source(){
    const p=this.profile,c=ImageProcessor.crop(this.image.width,this.image.height,p.width,p.height,this.state.composition),canvas=document.createElement('canvas');canvas.width=p.width;canvas.height=p.height;
    const x=canvas.getContext('2d',{willReadFrequently:true});x.fillStyle='#fff';x.fillRect(0,0,p.width,p.height);x.save();x.beginPath();x.rect(c.dx,c.dy,c.dw,c.dh);x.clip();x.translate(c.dx+c.dw/2,c.dy+c.dh/2);x.scale(c.dw/c.cw,c.dh/c.ch);x.translate(c.sw/2-c.cx*c.sw,c.sh/2-c.cy*c.sh);x.scale(c.flipX?-1:1,c.flipY?-1:1);x.rotate(c.rot*Math.PI/180);x.drawImage(this.image,-this.image.width/2,-this.image.height/2);x.restore();return x.getImageData(0,0,p.width,p.height);
  },
  request(){
    this.revision++;this.ready=-1;this.notice='';this.error='';if(!this.profile){this.error='请选择有效的模拟屏幕';this.controls();return;}if(!this.image){this.controls();return;}
    try{this.pending={epoch:this.epoch,revision:this.revision,panel:this.profile.id,profileId:this.profile.profileId,width:this.profile.width,height:this.profile.height,mode:this.state.mode,params:clone(this.state.params[this.state.mode]),pixels:this.source().data};}catch(e){this.error=e.message;this.pending=null;}
    this.controls();this.pump();
  },
  stop(){if(this.timer)clearTimeout(this.timer);this.timer=0;if(this.worker)this.worker.terminate();this.worker=null;this.job=null;this.pending=null;},
  pump(){
    if(this.job||!this.pending)return;const job=this.pending;this.pending=null;this.job=job;
    if(!this.worker&&!this.failedWorker){try{const w=new Worker('/worker.js');this.worker=w;w.onmessage=e=>{if(this.worker===w)this.complete(e.data);};w.onerror=e=>{e.preventDefault();if(this.worker===w)this.fallback();};w.onmessageerror=()=>{if(this.worker===w)this.fallback();};}catch(e){this.failedWorker=true;}}
    if(this.worker){try{const pixels=job.pixels.slice();this.timer=setTimeout(()=>this.fallback(),10000);this.worker.postMessage({...job,pixels:undefined,buffer:pixels.buffer},[pixels.buffer]);}catch(e){this.fallback();}}
    else this.runFallback(job);
  },
  fallback(){if(this.timer)clearTimeout(this.timer);this.timer=0;if(this.worker)this.worker.terminate();this.worker=null;this.failedWorker=true;if(this.job)this.runFallback(this.job);},
  runFallback(job){setTimeout(()=>{if(this.job!==job||job.epoch!==this.epoch)return;try{const im={width:job.width,height:job.height,data:job.pixels.slice()},profile=this.catalog.find(p=>p.id===job.panel),out=job.mode!=='color'||usesFourColor(profile)?ImageProcessor.process(im,{...job.params,mode:job.mode}):LegacyProcessor.process(im,profile,job.params);this.complete({...job,buffer:out.data.buffer});}catch(e){this.complete({...job,error:e.message});}},0);},
  complete(result){
    const job=this.job;if(!job||result.epoch!==job.epoch||result.revision!==job.revision||result.profileId!==job.profileId||result.panel!==job.panel)return;
    if(this.timer)clearTimeout(this.timer);this.timer=0;this.job=null;
    if(job.epoch===this.epoch&&job.profileId===this.profile?.profileId){
      if(result.error){if(job.revision===this.revision)this.error='处理失败：'+result.error;}
      else if(job.revision>this.rendered){try{
        if(result.buffer?.byteLength!==job.width*job.height*4)throw Error('结果尺寸不匹配');
        this.frame.width=this.original.width=job.width;this.frame.height=this.original.height=job.height;
        this.frame.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(result.buffer),job.width,job.height),0,0);
        this.original.getContext('2d').putImageData(new ImageData(job.pixels.slice(),job.width,job.height),0,0);
        this.rendered=job.revision;if(job.revision===this.revision)this.ready=job.revision;this.paint();
      }catch(e){this.error=e.message;}}
    }this.controls();this.pump();
  },
  schedulePaint(){if(this.paintFrame)cancelAnimationFrame(this.paintFrame);this.paintFrame=requestAnimationFrame(()=>{this.paintFrame=0;this.paint();});},
  paint(){
    const vp=$('viewport'),canvas=$('display'),p=this.profile;if(!vp.clientWidth||!vp.clientHeight)return;
    if(!p){canvas.width=canvas.height=1;canvas.style.width=canvas.style.height='0px';return;}
    const fit=Math.min(vp.clientWidth/p.width,vp.clientHeight/p.height,Math.max(1,400/Math.min(p.width,p.height)));
    this.scale=this.view.fit?fit:this.view.zoom/100;const w=p.width*this.scale,h=p.height*this.scale;
    $('surface').style.width=Math.max(w,vp.clientWidth)+'px';$('surface').style.height=Math.max(h,vp.clientHeight)+'px';canvas.style.width=w+'px';canvas.style.height=h+'px';canvas.style.left=Math.max(0,(vp.clientWidth-w)/2)+'px';canvas.style.top=Math.max(0,(vp.clientHeight-h)/2)+'px';canvas.style.imageRendering=this.view.kind==='pixels'?'pixelated':'auto';
    const dpr=window.devicePixelRatio||1;canvas.width=Math.max(1,Math.min(p.width,Math.round(w*dpr)));canvas.height=Math.max(1,Math.min(p.height,Math.round(h*dpr)));
    const x=canvas.getContext('2d');x.fillStyle='#fff';x.fillRect(0,0,canvas.width,canvas.height);
    if(this.rendered>=0){const source=this.compare?this.original:this.frame;if(this.view.kind==='pixels'||(canvas.width===p.width&&canvas.height===p.height)){x.imageSmoothingEnabled=false;x.drawImage(source,0,0,canvas.width,canvas.height);}else x.putImageData(average(source.getContext('2d').getImageData(0,0,p.width,p.height),canvas.width,canvas.height),0,0);}
    if(this.view.fit){vp.scrollLeft=vp.scrollTop=0;$('zoomSelect').value='fit';}
    else{let option=$('zoomSelect').querySelector('[data-custom]');const z=String(Math.round(this.view.zoom));if(![...$('zoomSelect').options].some(o=>o.value===z)){if(!option){option=document.createElement('option');option.dataset.custom='true';$('zoomSelect').append(option);}option.value=z;option.textContent=z+'%';}$('zoomSelect').value=z;}
    $('compare').classList.toggle('active',this.compare);$('compare').setAttribute('aria-pressed',String(this.compare));$('compareLabel').hidden=!this.compare;
  },
  zoom(value){this.view.fit=false;this.view.zoom=Math.min(400,Math.max(25,value));this.paint();},
  setCompare(value){this.compare=!!value&&this.rendered>=0;this.paint();},
  resizeSidebar(width){this.sidebar=Math.max(260,Math.min(380,Math.round(width)));document.documentElement.style.setProperty('--sidebar',this.sidebar+'px');$('splitter').setAttribute('aria-valuenow',this.sidebar);this.schedulePaint();},
  pointerDown(e){
    if(e.button!==0||this.busy||!this.image||!this.profile||this.compare)return;
    this.drag={id:e.pointerId,x:e.clientX,y:e.clientY,left:$('viewport').scrollLeft,top:$('viewport').scrollTop,rect:$('display').getBoundingClientRect(),crop:ImageProcessor.crop(this.image.width,this.image.height,this.profile.width,this.profile.height,this.state.composition),tool:this.tool};
    $('viewport').setPointerCapture(e.pointerId);if(this.tool==='crop')this.gesture=clone(this.state);e.preventDefault();
  },
  pointerMove(e){const d=this.drag;if(!d||d.id!==e.pointerId)return;const dx=e.clientX-d.x,dy=e.clientY-d.y;if(d.tool==='view'){$('viewport').scrollLeft=d.left-dx;$('viewport').scrollTop=d.top-dy;}else this.change(()=>{this.state.composition.cx=d.crop.cx-dx/d.rect.width*d.crop.cw/d.crop.sw;this.state.composition.cy=d.crop.cy-dy/d.rect.height*d.crop.ch/d.crop.sh;});},
  pointerEnd(e){if(this.drag?.id!==e.pointerId)return;const crop=this.drag.tool==='crop';this.drag=null;if(crop)this.commit();},
  async blob(){const epoch=this.epoch,revision=this.revision,profileId=this.profile?.profileId;if(!this.profile||this.ready!==revision||!this.image||this.error)throw Error('请等待当前预览完成');const blob=await new Promise((resolve,reject)=>this.frame.toBlob(b=>b?resolve(b):reject(Error('PNG编码失败')),'image/png'));if(epoch!==this.epoch||revision!==this.revision||profileId!==this.profile?.profileId||this.ready!==revision)throw Error('图像或屏幕已改变，请重新操作');return blob;},
  async upload(){
    if(this.busy)return;const target={epoch:this.epoch,revision:this.revision,id:this.profile?.id};try{const blob=await this.blob();if(target.epoch!==this.epoch||target.revision!==this.revision||target.id!==this.profile?.id)throw Error('屏幕或参数已改变');this.busy=true;this.notice='';this.controls();const id=target.id;
      const r=await fetch('/api/mock-upload?panel='+id,{method:'POST',headers:{'Content-Type':'image/png'},body:blob});const result=await r.json();if(!r.ok||!result.simulated)throw Error(result.error||'本地验证失败');
      this.notice='模拟完成 · '+result.width+'×'+result.height+' · '+Math.round(result.bytes/1024)+' KB · 未发送设备';
    }catch(e){this.notice='模拟未完成：'+e.message;}finally{this.busy=false;this.controls();}
  },
  async download(){try{const blob=await this.blob(),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='preview-id'+this.profile.id+'-'+this.state.mode+'.png';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}catch(e){this.notice='导出未完成：'+e.message;this.controls();}}
};
window.Workspace=App;
App.init();
