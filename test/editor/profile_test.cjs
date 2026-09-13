const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto');
const P=require('../../web/image_processor.js');
const statuses=JSON.parse(fs.readFileSync(process.argv[2]||'output/host-tests/image-profiles.json','utf8'));
const scope={self:{}};vm.runInNewContext(fs.readFileSync('test/editor/fixtures/legacy_processor.js','utf8'),scope);
const hash=d=>crypto.createHash('sha256').update(d).digest('hex');
let count=0;const test=(name,fn)=>{fn();count++;console.log('PASS '+name);};
const image=(w,h,gray=false)=>{const d=new Uint8ClampedArray(w*h*4);for(let y=0;y<h;y++)for(let x=0;x<w;x++){const i=(y*w+x)*4,v=Math.round(x*255/(w-1));d[i]=v;d[i+1]=gray?v:y*29%256;d[i+2]=gray?v:(x*7+y*11)%256;d[i+3]=255;}return {width:w,height:h,data:d};};
const profile=id=>P.profileFromStatus(statuses.find(s=>s.panel===id));
test('all production descriptors validate including logical RAM sizes',()=>{assert.equal(statuses.length,39);for(const s of statuses)assert.ok(P.profileFromStatus(s));assert.equal(profile(47).width,128);assert.equal(profile(47).height,250);assert.notEqual(profile(34).profileId,profile(37).profileId);assert.notEqual(profile(44).profileId,profile(34).profileId);});
test('missing and contradictory capabilities are rejected',()=>{
  for(const s of [null,{}, {...statuses[0],image_profile:null},{...statuses[0],panel_width:0},{...statuses[0],panel_width:8000000},{...statuses.find(s=>s.panel===43),panel:0},{...statuses[0],image_profile:{id:'coverage-three-v1',color_type:'bwr',inks:['black','white','yellow']}}])assert.equal(P.profileFromStatus(s),null);
});
for(const id of [47,19,20,10,34,37,44,43])test('panel '+id+' native processing uses legal inks',()=>{
  const p=profile(id),mode=p.family==='bw'?'mono':'color',im=image(p.width,p.height);
  P.processProfile(im,p,{mode});const allowed=P.profilePalette(p).map(c=>c.join(','));for(let i=0;i<im.data.length;i+=4)assert.ok(allowed.includes([...im.data.slice(i,i+3)].join(','))&&im.data[i+3]===255);
});
for(const id of [19,34,37,44])test('panel '+id+' neutral ramp stays BW',()=>{const p=profile(id),im=image(p.width,p.height,true);P.processProfile(im,p,{mode:'color'});for(let i=0;i<im.data.length;i+=4){assert.ok([0,255].includes(im.data[i]));assert.equal(im.data[i],im.data[i+1]);assert.equal(im.data[i],im.data[i+2]);}});
test('accepted JD outputs use independent defaults',()=>{
  for(const id of [34,37]){const p=profile(id),im=image(p.width,p.height),a=P.processProfile({...im,data:im.data.slice()},p,{mode:'color'}),b=P.process({...im,data:im.data.slice()},{...P.defaults('color'),neutralProtection:id===34?35:0,mode:'color'});assert.equal(hash(a.data),hash(b.data));}
  assert.equal(P.profileDefaults(profile(44),'color').neutralProtection,0);assert.equal(P.profileDefaults(profile(44),'color').dotPattern,'regular');
});
test('E6 core matches frozen algorithm with neutral and tuned values',()=>{const p=profile(43),im=image(p.width,p.height);for(const tune of [{},{brightness:7,contrast:112,gamma:90,sharp:15,dither:70}]){const params={...P.defaults('color'),...tune};const a=P.processProfile({...im,data:im.data.slice()},p,{...params,mode:'color'}),b=scope.self.LegacyProcessor.process({...im,data:im.data.slice()},p,params);assert.equal(hash(a.data),hash(b.data));}});
test('profile worker preserves identity and matches fallback',()=>{
  let response;const context={self:{postMessage:r=>response=r}};vm.runInNewContext(fs.readFileSync('web/image_processor.js','utf8'),context);
  const p=profile(37),im=image(p.width,p.height),job={epoch:12,revision:7,panel:37,profileId:p.profileId,profile:p,width:p.width,height:p.height,params:P.profileDefaults(p,'color'),mode:'color',buffer:im.data.slice().buffer};
  context.self.onmessage({data:job});assert.equal(response.panel,37);assert.equal(response.profileId,p.profileId);assert.equal(hash(new Uint8ClampedArray(response.buffer)),hash(P.processProfile(im,p,{mode:'color'}).data));
  context.self.onmessage({data:{...job,buffer:new ArrayBuffer(1)}});assert.ok(response.error);assert.equal(response.revision,7);
});
test('text threshold and pure endpoints remain exact across capabilities',()=>{for(const id of [47,10,34,37,44,43]){const p=profile(id),im=image(p.width,p.height,true);P.processProfile(im,p,{mode:'text',sharp:100,dither:160,shadows:100});for(let y=0;y<p.height;y++)for(let x=0;x<p.width;x++)assert.equal(im.data[(y*p.width+x)*4],Math.round(x*255/(p.width-1))<128?0:255);}});
console.log(`${count} panel profile tests passed`);
