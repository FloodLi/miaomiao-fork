const http=require('node:http');
const fs=require('node:fs');
const path=require('node:path');
const root=path.resolve(__dirname,'../..');
const panels=JSON.parse(fs.readFileSync(path.join(__dirname,'panels.json'),'utf8')).map(p=>({
  ...p,profileId:p.id===37?'preview-37-color-v1':p.profileId,
  processor:p.id===34||p.id===37?'four-color':'legacy'
}));
const legacy=fs.readFileSync(path.join(root,'test/editor/fixtures/legacy_processor.js'),'utf8');

const icons=new Set(['arrow-left','folder-open','chevron-down','undo-2','redo-2','rotate-ccw','upload','panel-right-close','panel-right-open','sliders-horizontal','crop','hand','scan','zoom-in','zoom-out','eye','download','info','rotate-cw','flip-horizontal','flip-vertical','check','x','image']);
const files=new Map([
  ['/',[path.join(__dirname,'index.html'),'text/html; charset=utf-8']],
  ['/app.css',[path.join(__dirname,'app.css'),'text/css; charset=utf-8']],
  ['/app.js',[path.join(__dirname,'app.js'),'application/javascript; charset=utf-8']],
  ['/worker.js',[path.join(__dirname,'worker.js'),'application/javascript; charset=utf-8']],
  ['/processor.js',[path.join(root,'web/image_processor.js'),'application/javascript; charset=utf-8']],
  ['/assets/brand.png',[path.join(root,'web/miaooaim-mark.png'),'image/png']],
  ['/assets/photo.jpg',[path.join(root,'test/web/fixtures/reference.png'),'image/png']],
  ['/assets/chart.png',[path.join(root,'test/web/fixtures/reference.png'),'image/png']],
  ['/assets/chart-37.png',[path.join(root,'test/web/fixtures/reference.png'),'image/png']],
  ['/assets/qr.png',[path.join(root,'test/web/fixtures/qr.png'),'image/png']],
]);
let port=Number(process.argv[process.argv.indexOf('--port')+1])||8787;
const send=(res,status,body,type='application/json; charset=utf-8')=>{res.writeHead(status,{'Content-Type':type});res.end(typeof body==='string'||Buffer.isBuffer(body)?body:JSON.stringify(body));};
const server=http.createServer((req,res)=>{
  res.setHeader('Cache-Control','no-store');res.setHeader('X-Content-Type-Options','nosniff');
  res.setHeader('Cross-Origin-Resource-Policy','same-origin');
  res.setHeader('Content-Security-Policy',"default-src 'self'; script-src 'self'; worker-src 'self'; connect-src 'self'; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
  if(!['127.0.0.1:'+port,'localhost:'+port].includes(req.headers.host))return send(res,403,{error:'Loopback host required'});
  if(req.headers.origin&&!['http://127.0.0.1:'+port,'http://localhost:'+port].includes(req.headers.origin))return send(res,403,{error:'Local origin required'});
  const url=new URL(req.url,'http://127.0.0.1:'+port);
  if(req.method==='POST'){
    if(url.pathname!=='/api/mock-upload')return send(res,405,{error:'Device operations are not available in this prototype'});
    const profile=panels.find(p=>p.id===Number(url.searchParams.get('panel')));
    if(!profile||!url.searchParams.has('panel'))return send(res,400,{error:'Unknown mock panel'});
    let chunks=[],size=0,done=false;
    const deadline=setTimeout(()=>{done=true;req.destroy();},30000);
    req.on('data',chunk=>{if(done)return;size+=chunk.length;if(size>4*1024*1024){done=true;clearTimeout(deadline);send(res,413,{error:'Mock upload too large'});req.destroy();}else chunks.push(chunk);});
    req.on('end',()=>{clearTimeout(deadline);if(done)return;const b=Buffer.concat(chunks);chunks=[];
      if(b.length<24||!b.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10]))||b.readUInt32BE(16)!==profile.width||b.readUInt32BE(20)!==profile.height)return send(res,400,{error:'PNG dimensions do not match mock panel'});
      send(res,200,{ok:true,simulated:true,deviceAccess:false,panel:profile.id,width:profile.width,height:profile.height,bytes:b.length});
    });req.on('close',()=>clearTimeout(deadline));return;
  }
  if(req.method!=='GET'&&req.method!=='HEAD')return send(res,405,{error:'Read-only prototype'});
  if(url.pathname==='/api/session')return send(res,200,{app:'msp-editor-workspace-prototype',prototype:true,deviceAccess:false,port,pid:process.pid,panels:panels.length});
  if(url.pathname==='/api/panels')return send(res,200,panels);
  if(url.pathname==='/panels.js')return send(res,200,'self.PANEL_PROFILES='+JSON.stringify(panels)+';','application/javascript; charset=utf-8');
  if(url.pathname==='/legacy.js')return send(res,200,legacy,'application/javascript; charset=utf-8');
  let file=files.get(url.pathname);
  const icon=/^\/icons\/([a-z0-9-]+)\.svg$/.exec(url.pathname);
  if(icon&&icons.has(icon[1]))file=[path.join(root,'output/workspace-prototype-deps/node_modules/lucide-static/icons',icon[1]+'.svg'),'image/svg+xml'];
  if(!file)return send(res,404,{error:'Not a prototype resource'});
  fs.readFile(file[0],(error,data)=>{if(error)return send(res,404,{error:'Local sample resource unavailable'});send(res,200,data,file[1]);});
});
server.on('error',error=>{console.error(error.message);process.exit(1);});
server.listen(port,'127.0.0.1',()=>console.log(JSON.stringify({url:'http://127.0.0.1:'+port,prototype:true,deviceAccess:false,panels:panels.length})));
