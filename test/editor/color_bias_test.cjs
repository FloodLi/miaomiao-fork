const assert=require('node:assert/strict');
const P=require('../../web/image_processor.js');
const palette=[[0,0,0],[255,255,255],[192,57,43],[210,165,24]];
const luma=palette.map(c=>.2126*P.linear(c[0])+.7152*P.linear(c[1])+.0722*P.linear(c[2]));
let passed=0;
const check=(name,fn)=>{fn();passed++;console.log('PASS '+name);};
function render(color,strength=0,mode='color'){
  const width=128,height=128,data=new Uint8ClampedArray(width*height*4);
  for(let i=0;i<data.length;i+=4)data.set([...color,255],i);
  return P.process({width,height,data},{mode,neutralProtection:strength}).data;
}
function stats(data){
  const counts=[0,0,0,0];
  for(let i=0;i<data.length;i+=4){
    const k=palette.findIndex(c=>c.every((v,j)=>v===data[i+j]));assert.ok(k>=0);assert.equal(data[i+3],255);counts[k]++;
  }
  const n=data.length/4;return {warm:(counts[2]+counts[3])/n,luma:counts.reduce((s,c,i)=>s+c*luma[i],0)/n,counts};
}
check('off is the default and strength clamps',()=>{assert.equal(P.defaults().neutralProtection,0);assert.equal(P.tuning({neutralProtection:200}).neutralProtection,100);assert.equal(P.tuning({neutralProtection:-20}).neutralProtection,0);});
for(const color of [[0,0,0],[255,255,255],[192,57,43],[210,165,24],[255,0,0],[255,255,0],[60,2,0],[42,40,0]]){
  check('pure or high-saturation color stays identical '+color,()=>assert.deepEqual(render(color,35),render(color,0)));
}
for(const color of [[150,145,136],[190,181,164],[220,205,185],[125,123,120]]){
  check('low-chroma warmth reduces while model luminance holds '+color,()=>{
    const before=stats(render(color,0)),after=stats(render(color,35));
    assert.ok(before.warm>0,JSON.stringify(before));assert.ok(after.warm<before.warm*.85,JSON.stringify({before,after}));
    assert.ok(Math.abs(after.luma-before.luma)<.006,JSON.stringify({before,after}));
    console.log(JSON.stringify({color,before,after}));
  });
}
for(const color of [[245,220,215],[245,239,205]]){
  check('pale true colors are softened, not removed '+color,()=>{
    const before=stats(render(color,0)),after=stats(render(color,35));assert.ok(after.warm>before.warm*.55);assert.ok(after.warm<before.warm);
  });
}
check('grays stay neutral and non-color modes are unchanged',()=>{
  for(const v of [0,32,64,128,192,255]){const d=render([v,v,v],35);assert.equal(stats(d).warm,0);}
  for(const mode of ['mono','text'])assert.deepEqual(render([190,181,164],35,mode),render([190,181,164],0,mode));
});
check('strength is deterministic and monotonic on neutral patches',()=>{
  let previous=1;
  for(const n of [0,20,35,60,100]){const a=render([190,181,164],n);assert.deepEqual(a,render([190,181,164],n));const now=stats(a).warm;assert.ok(now<=previous+.002);previous=now;}
});
console.log(`${passed} color bias tests passed`);
