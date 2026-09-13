const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const crypto=require('node:crypto');
const P=require('../../web/image_processor.js');
let count=0;
const test=(name,fn)=>{fn();count++;console.log('PASS '+name);};
const image=(w,h,fn)=>{const data=new Uint8ClampedArray(w*h*4);for(let y=0;y<h;y++)for(let x=0;x<w;x++)data.set(fn(x,y),(y*w+x)*4);return {width:w,height:h,data};};
const process=(im,opts)=>P.process({...im,data:im.data.slice()},opts).data;
const hash=data=>crypto.createHash('sha256').update(data).digest('hex');
const colors=data=>new Set(Array.from({length:data.length/4},(_,i)=>Array.from(data.slice(i*4,i*4+4)).join(',')));
const mean=data=>{let s=0;for(let i=0;i<data.length;i+=4)s+=data[i]/255;return s/(data.length/4);};
test('defaults and bounded numeric input',()=>{
  assert.equal(P.defaults().contrast,100);assert.equal(P.defaults().shadows,0);assert.equal(P.defaults('text').dither,0);
  assert.equal(P.tuning({contrast:null}).contrast,100);assert.equal(P.tuning({brightness:999}).brightness,80);
  assert.equal(P.tuning({sharp:99,dither:150,shadows:60},'text').sharp,0);
});
test('legacy color golden and repeatability',()=>{
  const im=image(64,64,(x,y)=>[x*4,y*4,(x*3+y*5)%256,255]);
  assert.equal(hash(process(im,{mode:'color'})),'07cdc38bfd32edfaa316d96ce73a70ffd7a1917786b4549aa070f99acec5813a');
  assert.equal(hash(process(im,{mode:'color'})),hash(process(im,{mode:'color'})));
});
for(const mode of ['color','mono','text']){
  test(mode+' black/white endpoints and transparent white',()=>{
    const im=image(20,10,(x)=>x<5?[0,0,0,255]:x<10?[255,255,255,255]:[255,0,0,0]);
    const out=process(im,{mode,shadows:100});
    for(let y=0;y<10;y++)for(let x=0;x<20;x++)assert.deepEqual([...out.slice((y*20+x)*4,(y*20+x+1)*4)],x<5?[0,0,0,255]:[255,255,255,255]);
  });
  test(mode+' palette and no gray tint',()=>{
    const im=image(96,80,(x,y)=>[(x*11)%256,(y*17)%256,(x+y)*3%256,255]);
    const out=process(im,{mode});
    const allowed=mode==='color'?['0,0,0,255','255,255,255,255','192,57,43,255','210,165,24,255']:['0,0,0,255','255,255,255,255'];
    assert.ok([...colors(out)].every(c=>allowed.includes(c)));
    const gray=process(image(96,80,(x)=>[x*2,x*2,x*2,255]),{mode});
    assert.ok([...colors(gray)].every(c=>c==='0,0,0,255'||c==='255,255,255,255'));
  });
}
test('mono shadows lift dark tones without changing endpoints',()=>{
  let previous=-1;
  for(const v of [0,16,32,64,96,128,192,224,255]){
    const im=image(96,96,()=>[v,v,v,255]);
    const base=mean(process(im,{mode:'mono'})),lift=mean(process(im,{mode:'mono',shadows:100}));
    assert.ok(base>=previous);assert.ok(lift>=base-.003);previous=base;
    if(v===0||v===255)assert.equal(base,lift);
  }
});
test('text has no diffusion, sharpening or shadow effects',()=>{
  const im=image(256,12,x=>[x,x,x,255]);
  const out=process(im,{mode:'text',dither:160,sharp:100,shadows:100});
  for(let y=0;y<12;y++)for(let x=0;x<256;x++)assert.equal(out[(y*256+x)*4],x<128?0:255);
});
test('invalid images are bounded',()=>{
  assert.throws(()=>P.process({width:0,height:1,data:[]}));
  assert.throws(()=>P.process({width:800,height:480,data:[]}));
  assert.throws(()=>P.process({width:99999,height:99999,data:[]}));
});
test('crop covers panel, clamps center, and follows rotation',()=>{
  for(const rot of [0,90,180,270])for(const zoom of [1,2,4])for(const [cx,cy] of [[0,0],[1,1],[.5,.5]]){
    const c=P.crop(1280,960,800,480,{fit:'cover',rot,zoom,cx,cy});
    assert.ok(c.cx*c.sw-c.cw/2>=-.0001&&c.cx*c.sw+c.cw/2<=c.sw+.0001);
    assert.ok(c.cy*c.sh-c.ch/2>=-.0001&&c.cy*c.sh+c.ch/2<=c.sh+.0001);
    assert.ok(Math.abs(c.cw/c.ch-800/480)<1e-9);assert.equal(c.sw,rot%180?960:1280);
  }
  const c=P.crop(1280,960,800,480,{fit:'contain'});assert.equal(c.dx,80);assert.equal(c.dw,640);assert.equal(c.dh,480);
});
test('worker protocol matches synchronous processing and reports errors',()=>{
  let result;
  const context={Uint8ClampedArray,Float32Array,Uint8Array,Math,console};context.self={postMessage:r=>result=r};
  vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../../web/image_processor.js'),'utf8'),context);
  const im=image(32,32,(x,y)=>[x*7,y*7,50,255]);
  context.self.onmessage({data:{epoch:2,revision:7,width:32,height:32,buffer:im.data.slice().buffer,options:{mode:'color'}}});
  assert.equal(result.epoch,2);assert.equal(result.revision,7);assert.equal(hash(new Uint8ClampedArray(result.buffer)),hash(process(im,{mode:'color'})));
  context.self.onmessage({data:{epoch:2,revision:8,width:0,height:0,buffer:new ArrayBuffer(0)}});assert.ok(result.error);assert.equal(result.revision,8);
});
console.log(`${count} processor tests passed`);
