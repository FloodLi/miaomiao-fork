importScripts('/panels.js','/processor.js','/legacy.js');
self.onmessage=event=>{
  const job=event.data,profile=PANEL_PROFILES.find(p=>p.id===job.panel);
  try{
    if(!profile||profile.profileId!==job.profileId||job.width!==profile.width||job.height!==profile.height)throw new Error('Unknown prototype profile');
    if(!job.buffer||job.buffer.byteLength!==profile.width*profile.height*4)throw new Error('Invalid frame length');
    if(!['color','mono','text'].includes(job.mode)||(profile.family==='bw'&&job.mode==='color'))throw new Error('Unsupported ink mode');
    const im={width:profile.width,height:profile.height,data:new Uint8ClampedArray(job.buffer)};
    const result=job.mode!=='color'||profile.id===34||profile.processor==='four-color'?ImageProcessor.process(im,{...job.params,mode:job.mode}):LegacyProcessor.process(im,profile,job.params);
    self.postMessage({epoch:job.epoch,revision:job.revision,panel:profile.id,profileId:profile.profileId,buffer:result.data.buffer},[result.data.buffer]);
  }catch(error){self.postMessage({epoch:job.epoch,revision:job.revision,panel:job.panel,profileId:job.profileId,error:String(error.message||error)});}
};
