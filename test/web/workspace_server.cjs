// Local fixture server for the embedded page. Never forwards device operations.
const fs=require('node:fs'),http=require('node:http'),path=require('node:path');
const root=path.resolve(__dirname,'../..'),port=Number(process.argv[2])||8765;
const profiles=JSON.parse(fs.readFileSync(process.argv[3]||path.join(root,'output/host-tests/image-profiles.json'),'utf8'));
const server=http.createServer((req,res)=>{
  res.setHeader('Cache-Control','no-store');const url=new URL(req.url,'http://127.0.0.1:'+port);
  if(req.method!=='GET'){res.writeHead(405);return res.end('Local preview is read only');}
  const json=value=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify(value));};
  if(url.pathname==='/status')return json(profiles.find(p=>p.panel===34));
  if(url.pathname==='/profiles.json')return json(profiles);
  if(url.pathname==='/images')return json({items:[]});
  if(url.pathname==='/wifi_status')return json({sta_connected:false});
  if(url.pathname==='/session')return json({app:'msp-embedded-workspace-preview',pid:process.pid,deviceAccess:false});
  const name=url.pathname==='/'?'index.html':url.pathname.slice(1);
  if(!['index.html','config.html','message.html','feature_help.js','image_processor.js','miaooaim-mark.png'].includes(name)){res.writeHead(404);return res.end('Not found');}
  const type=name.endsWith('.js')?'application/javascript':name.endsWith('.png')?'image/png':'text/html; charset=utf-8';
  fs.readFile(path.join(root,'web',name),(err,data)=>{if(err){res.writeHead(404);res.end();}else{res.setHeader('Content-Type',type);res.end(data);}});
});
server.listen(port,'127.0.0.1',()=>console.log('http://127.0.0.1:'+port));
