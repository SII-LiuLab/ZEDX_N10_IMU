// Exercise selection/render scheduling without a browser or external packages.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../scripts/live_preview.html'), 'utf8').split('<script>')[1].split('</script>')[0];

async function check(names) {
  const requests = [], drawings = [];
  function element(kind) {
    const children = new Map();
    return { classList: {add(){}, remove(){}}, append(){},
      querySelector(key) {if(!children.has(key)) children.set(key, element(key)); return children.get(key);},
      getContext(){return {drawImage(){drawings.push(kind);}}} };
  }
  const root = element('document');
  const cameras = Object.fromEntries(names.map(name=>[name,{state:'live',views:name==='zed'?['rgb','depth']:['rgb'],preview_fps:10}]));
  const context = vm.createContext({document:{hidden:false,querySelector:key=>root.querySelector(key),createElement:element},
    performance, AbortSignal, setTimeout(){},
    async createImageBitmap(){return {width:960,height:600,close(){}};},
    async fetch(url){requests.push(url);return {ok:true,status:200,json:async()=>({cameras,fps_limit:10}),
      headers:{get:key=>key==='X-RGB-Bytes'?'3':'1'},blob:async()=>new Blob(['RGBDEPTH'])};}
  });
  vm.runInContext(source, context);
  await new Promise(setImmediate);
  assert.equal(vm.runInContext('cards.size',context), names.reduce((n,c)=>n+(c==='zed'?2:1),0));
  for(const name of names)assert(requests.some(url=>url.startsWith(`/frame/${name}/${name==='zed'?'rgbd':'rgb'}?`)),JSON.stringify(requests));
  for(const name of ['zed','shw_v6','shw_v7'].filter(n=>!names.includes(n)))assert(!requests.some(url=>url.startsWith(`/frame/${name}/`)));
  assert.equal(drawings.length,names.reduce((n,c)=>n+(c==='zed'?2:1),0));
}
(async()=>{for(const names of [['zed'],['shw_v6'],['shw_v7','zed'],['shw_v6','shw_v7','zed']])await check(names);console.log('4 camera selection/render scheduling cases passed');})().catch(error=>{console.error(error);process.exitCode=1;});
