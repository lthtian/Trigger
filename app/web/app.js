'use strict';
const $=id=>document.getElementById(id);
const escapeHtml=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const identifier=()=>crypto.randomUUID().replaceAll('-','');
const requests=new Map();
let sequence=0,environment=null,settings=null,selected=null,page='scenes',dirty=false,editing=null,naming=null,captured=[],lastCodes=[],toastTimer;
function rpc(command,payload={}) {
  return new Promise((resolve,reject)=>{
    const id=++sequence;
    const timer=setTimeout(()=>{requests.delete(id);reject(new Error('操作超时，请重试。'));},60000);
    requests.set(id,{resolve,reject,timer});
    window.chrome.webview.postMessage({id,command,payload});
  });
}
function toast(message,error=false) {
  clearTimeout(toastTimer);$('toast').textContent=message;$('toast').className='toast'+(error?' error':'');
  toastTimer=setTimeout(()=>$('toast').classList.add('hidden'),error?8000:4500);
}
function action(handler) { return async event=>{try {await handler(event);} catch(error){toast(error.message,true);}}; }
function markDirty() {dirty=true;$('dirty-mark').textContent='●';}
function scene() {return settings?.scenes.find(item=>item.id===selected);}
function selectPage(value) {page=value;document.querySelectorAll('.page').forEach(element=>element.classList.add('hidden'));$(value+'-page').classList.remove('hidden');document.querySelectorAll('.nav').forEach(element=>element.classList.toggle('active',element.dataset.page===value));render();}
function applyState(data) {
  environment=data;
  if(!dirty) settings=structuredClone(data.settings);
  if(!settings.scenes.some(item=>item.id===selected)) selected=settings.scenes[0]?.id??null;
  $('device-status').textContent=`${data.monitors.length} 块显示器 · ${data.desktops.length||1} 个桌面`;
  $('hide-after-launch').checked=settings.hide_after_launch;
  $('hotkeys-enabled').checked=settings.hotkeys_enabled;
  $('otp-enabled').checked=settings.otp_enabled;
  $('server-url').value=data.connection.url||'';
  $('connection-info').textContent=data.connection.configured?'已保存当前账号的加密连接凭据。令牌留空可保留。':'尚未配置验证码服务。凭据将使用当前 Windows 账号加密。';
  $('capabilities').textContent=data.desktop_supported?'支持窗口捕获、多显示器与 Windows 虚拟桌面分配。':data.desktop_error;
  $('sms-status').textContent=data.sms_status||'等待连接';
  render();
  if(data.last_report&&!data.busy)report(data.last_report,true);
  if(data.open_page&&page!==data.open_page)selectPage(data.open_page);
  if(data.hotkey_errors?.length)toast(data.hotkey_errors.join('；'),true);
}
function render() {
  if(!settings||!environment) return;
  const active=scene();
  $('scene-list').innerHTML=settings.scenes.map(item=>`<button class="scene-item ${item.id===selected?'selected':''}" data-scene="${escapeHtml(item.id)}"><span>${escapeHtml(item.name)}</span><small>${item.entries.length}</small></button>`).join('');
  $('breadcrumb').textContent=page==='scenes'?`工作场景 / ${active?.name||'新建你的第一个场景'}`:page==='codes'?'验证码 / 最近消息':'设置 / 本机';
  $('scene-title').textContent=active?.name||'创建你的工作场景';
  $('scene-description').textContent=active?`${active.entries.length} 个窗口，按你的布局打开。`:'记录常用应用和网页，下一次一键回到工作状态。';
  $('entry-count').textContent=active?.entries.length||0;
  for(const id of ['launch','rename-scene','save-scene','shortcut','scene-hotkey','delete-scene','add-entry','capture']) $(id).disabled=!active;
  $('launch').disabled=!active?.entries.length||environment.busy;
  $('launch').querySelector('span').textContent=environment.busy?'正在启动…':'启动场景';
  $('scene-hotkey').innerHTML='<option value="0">未设置</option>'+Array.from({length:9},(_,i)=>`<option value="${i+1}">Ctrl + Alt + ${i+1}</option>`).join('');
  $('scene-hotkey').value=active?.hotkey||0;
  $('dirty-mark').textContent=dirty?'●':'';
  const desktopValue=$('preview-desktop').value;
  const desktopNumbers=new Set(environment.desktops.map(item=>item.number));
  active?.entries.forEach(item=>{if(item.desktop)desktopNumbers.add(item.desktop);});
  $('preview-desktop').innerHTML='<option value="all">全部桌面</option>'+[...desktopNumbers].sort((a,b)=>a-b).map(number=>`<option value="${number}">${escapeHtml(desktopName(number))}</option>`).join('');
  if([...$('preview-desktop').options].some(option=>option.value===desktopValue)) $('preview-desktop').value=desktopValue;
  renderPreview();
  $('entries').innerHTML=active?.entries.length?active.entries.map(entry=>`<article class="entry"><div class="app-icon ${entry.kind==='web'?'web':''}">${entry.kind==='web'?'◎':'▧'}</div><div class="entry-main"><div class="entry-name">${escapeHtml(entry.name)}<span class="tag">${entry.kind==='web'?'网页':'应用'}</span>${entry.kind==='web'&&!entry.url?'<span class="tag danger">需要填写网址</span>':''}</div><div class="entry-detail">${escapeHtml(entry.kind==='web'?(entry.url||'编辑此窗口，填写要恢复的网址'):entry.path)}</div></div><div class="entry-placement">${escapeHtml(monitorName(entry.monitor))}<br>${escapeHtml(desktopName(entry.desktop))} · ${Math.round(entry.layout.width*100)}% × ${Math.round(entry.layout.height*100)}%</div><div class="entry-actions"><button data-edit="${escapeHtml(entry.id)}">编辑</button><button class="remove-entry" data-remove="${escapeHtml(entry.id)}" aria-label="移除 ${escapeHtml(entry.name)}">×</button></div></article>`).join(''):'<div class="empty">添加应用或网页，也可以直接记录已经摆好的窗口。</div>';
}
function monitorName(id) {
  if(id==='primary')return '主显示器';
  const index=environment.monitors.findIndex(item=>item.id===id);
  return index<0?'未连接的显示器':`显示器 ${index+1}${environment.monitors[index].primary?' · 主屏':''}`;
}
function desktopName(number) {return number===0?'启动时的当前桌面':environment.desktops.find(item=>item.number===number)?.name||`桌面 ${number}（启动时创建）`;}
function renderPreview() {
  const active=scene(), filter=$('preview-desktop').value;
  const current=environment.desktops.find(item=>item.current)?.number||1;
  $('monitor-preview').innerHTML=environment.monitors.map((monitor,index)=>{
    const entries=(active?.entries||[]).filter(entry=>{
      const onMonitor=entry.monitor===monitor.id||(entry.monitor==='primary'&&monitor.primary)||(!environment.monitors.some(m=>m.id===entry.monitor)&&monitor.primary);
      return onMonitor&&(filter==='all'||Number(filter)===(entry.desktop||current));
    });
    return `<div class="monitor"><div class="monitor-name"><span>显示器 ${index+1}${monitor.primary?' · 主屏':''}</span><span>${monitor.bounds.width} × ${monitor.bounds.height}</span></div><div class="monitor-screen">${entries.map(entry=>{
      const layout=entry.state==='maximized'?{x:0,y:0,width:1,height:1}:entry.layout;
      const width=Math.min(1,layout.width),height=Math.min(1,layout.height),x=Math.max(0,Math.min(1-width,layout.x)),y=Math.max(0,Math.min(1-height,layout.y));
      return `<div class="preview-window" style="left:${x*100}%;top:${y*100}%;width:${width*100}%;height:${height*100}%"><button data-edit="${escapeHtml(entry.id)}" title="编辑 ${escapeHtml(entry.name)}"><small>${entry.kind==='web'?'◎ 网页':'▧ 应用'}</small><strong>${escapeHtml(entry.name)}</strong><small>${escapeHtml(desktopName(entry.desktop))}</small></button></div>`;
    }).join('')}</div></div>`;
  }).join('');
}
async function save() {
  if(!settings.scenes.every(item=>item.entries.length)) throw new Error('请先为每个场景添加至少一个窗口。');
  const state=await rpc('save',{settings});dirty=false;applyState(state);
}
function openEntry(id=null) {
  if(!scene())return;
  editing=id;
  const entry=scene().entries.find(item=>item.id===id)||{id:identifier(),name:'',kind:'web',path:environment.browsers[0]?.path||'',url:'',args:'',workdir:'',title:'',monitor:'primary',desktop:0,state:'normal',reuse:true,layout:{x:0,y:0,width:.5,height:1}};
  $('entry-form').dataset.id=entry.id;
  $('entry-dialog-title').textContent=id?'编辑窗口':'添加应用 / 网页';
  for(const field of ['name','path','url','args','workdir','title','state']) $('entry-'+field).value=entry[field]??'';
  document.querySelector(`input[name="kind"][value="${entry.kind}"]`).checked=true;
  $('entry-reuse').checked=entry.reuse;
  $('entry-browser').innerHTML=environment.browsers.map(browser=>`<option value="${escapeHtml(browser.path)}">${escapeHtml(browser.name)}</option>`).join('')+'<option value="custom">选择其他 Chrome / Edge 路径</option>';
  $('entry-browser').value=environment.browsers.some(browser=>browser.path===entry.path)?entry.path:'custom';
  $('entry-monitor').innerHTML='<option value="primary">始终使用主显示器</option>'+environment.monitors.map((monitor,index)=>`<option value="${escapeHtml(monitor.id)}">显示器 ${index+1} · ${escapeHtml(monitor.name)}${monitor.primary?'（主屏）':''}</option>`).join('');
  if(entry.monitor!=='primary'&&!environment.monitors.some(monitor=>monitor.id===entry.monitor)) $('entry-monitor').add(new Option('原显示器（未连接）',entry.monitor));
  $('entry-monitor').value=entry.monitor;
  const count=Math.min(32,Math.max(environment.desktops.length+1,entry.desktop,2));
  $('entry-desktop').innerHTML='<option value="0">启动时的当前桌面</option>'+Array.from({length:count},(_,i)=>`<option value="${i+1}">${escapeHtml(desktopName(i+1))}</option>`).join('');
  $('entry-desktop').value=entry.desktop;
  $('entry-desktop').disabled=!environment.desktop_supported;
  setLayout(entry.layout);updateKind();$('entry-dialog').showModal();
}
function setLayout(layout) {for(const key of ['x','y','width','height']) $('layout-'+key).value=Math.round(Math.max(0,Math.min(1,layout[key]))*1000)/10;}
function updateKind() {
  const web=document.querySelector('input[name="kind"]:checked').value==='web';
  $('web-fields').classList.toggle('hidden',!web);
  $('app-path-label').classList.toggle('hidden',web&&$('entry-browser').value!=='custom');
  $('entry-url').required=web;
  if(web&&$('entry-browser').value!=='custom') $('entry-path').value=$('entry-browser').value;
}
function renderCodes(codes,status) {
  lastCodes=codes;
  if(status)$('sms-status').textContent=status;
  $('codes-list').innerHTML=codes.length?codes.map(item=>`<div class="code-card"><div><strong>${escapeHtml(item.code)}</strong><small>${escapeHtml(item.sender||'验证码')} · ${new Date(item.receivedAt*1000).toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit'})}</small></div><button class="secondary" data-copy="${escapeHtml(item.id)}">复制验证码</button></div>`).join(''):'<div class="empty">暂无最近验证码。收到短信后会在这里出现。</div>';
  $('code-dot').className=codes.length?'tiny-dot':'';
}
function report(data,complete=false) {
  $('restore-report').classList.remove('hidden');
  if(!complete) {
    if(!$('restore-report').dataset.running){$('restore-report').innerHTML='<h2>启动进度</h2>';$('restore-report').dataset.running='1';}
    $('restore-report').insertAdjacentHTML('beforeend',`<div class="result ${data.ok?(data.warning?'warning':''):'error'}">${data.ok?'✓':'!'} ${escapeHtml(data.name)} · ${escapeHtml(data.ok?(data.warning||(data.reused?'已复用并恢复':'已启动并恢复')):data.error)}</div>`);
  } else {
    $('restore-report').innerHTML='<h2></h2>';
    for(const item of data.items||[])report(item);
    delete $('restore-report').dataset.running;
    $('restore-report').querySelector('h2')?.replaceChildren(document.createTextNode(data.ok?'场景已就绪':'部分窗口需要处理'));
    environment.busy=false;render();
  }
}
window.chrome.webview.addEventListener('message',event=>{
  const message=event.data;
  if(message.reply) {
    const request=requests.get(message.reply);if(!request)return;
    clearTimeout(request.timer);requests.delete(message.reply);
    message.error?request.reject(new Error(message.error)):request.resolve(message.data);return;
  }
  if(message.event==='state')applyState(message.data);
  if(message.event==='codes')renderCodes(message.data.codes,message.data.status);
  if(message.event==='progress')report(message.data);
  if(message.event==='complete')report(message.data,true);
  if(message.event==='notice')toast(message.data,true);
});
document.querySelectorAll('.nav').forEach(button=>button.onclick=()=>selectPage(button.dataset.page));
$('scene-list').onclick=event=>{const button=event.target.closest('[data-scene]');if(button){selected=button.dataset.scene;selectPage('scenes');}};
document.querySelectorAll('[data-close]').forEach(button=>button.onclick=()=>$(button.dataset.close).close());
$('preview-desktop').onchange=renderPreview;
document.querySelectorAll('input[name="kind"]').forEach(input=>input.onchange=updateKind);
$('entry-browser').onchange=updateKind;
$('scene-hotkey').onchange=()=>{scene().hotkey=Number($('scene-hotkey').value);markDirty();};
$('add-entry').onclick=()=>openEntry();
for(const id of ['entries','monitor-preview']) $(id).onclick=event=>{
  const edit=event.target.closest('[data-edit]'),remove=event.target.closest('[data-remove]');
  if(edit)openEntry(edit.dataset.edit);
  if(remove){scene().entries=scene().entries.filter(item=>item.id!==remove.dataset.remove);markDirty();render();}
};
$('entry-form').onsubmit=action(async event=>{
  event.preventDefault();
  const kind=document.querySelector('input[name="kind"]:checked').value;
  const entry={id:$('entry-form').dataset.id,kind,reuse:$('entry-reuse').checked,desktop:Number($('entry-desktop').value),monitor:$('entry-monitor').value,layout:{}};
  for(const field of ['name','path','url','args','workdir','title','state'])entry[field]=$('entry-'+field).value.trim();
  for(const key of ['x','y','width','height'])entry.layout[key]=Number($('layout-'+key).value)/100;
  if(!entry.path)throw new Error('请选择程序或浏览器路径。');
  if(kind==='web'&&!/^https?:\/\//.test(entry.url))throw new Error('请填写完整的 http:// 或 https:// 网址。');
  if(editing)scene().entries=scene().entries.map(item=>item.id===editing?entry:item);else scene().entries.push(entry);
  markDirty();render();$('entry-dialog').close();
});
document.querySelectorAll('[data-layout]').forEach(button=>button.onclick=()=>{const presets={left:{x:0,y:0,width:.5,height:1},right:{x:.5,y:0,width:.5,height:1},full:{x:0,y:0,width:1,height:1},center:{x:.1,y:.1,width:.8,height:.8}};setLayout(presets[button.dataset.layout]);$('entry-state').value='normal';});
$('browse-app').onclick=action(async()=>{const path=await rpc('pick_file');if(path)$('entry-path').value=path;});
$('browse-folder').onclick=action(async()=>{const path=await rpc('pick_folder');if(path)$('entry-workdir').value=path;});
function nameDialog(mode) {naming=mode;$('name-heading').textContent=mode==='add'?'新建场景':'重命名场景';$('name-input').value=mode==='add'?'':scene().name;$('name-dialog').showModal();$('name-input').focus();}
$('add-scene').onclick=()=>nameDialog('add');$('rename-scene').onclick=()=>nameDialog('rename');
$('name-form').onsubmit=event=>{event.preventDefault();const name=$('name-input').value.trim();if(!name)return;if(naming==='add'){const item={id:identifier(),name,hotkey:0,entries:[]};settings.scenes.push(item);selected=item.id;}else scene().name=name;markDirty();selectPage('scenes');$('name-dialog').close();};
$('delete-scene').onclick=()=>$('confirm-dialog').showModal();
$('confirm-delete').onclick=action(async()=>{settings.scenes=settings.scenes.filter(item=>item.id!==selected);selected=settings.scenes[0]?.id??null;markDirty();$('confirm-dialog').close();await save();toast('场景配置已删除');});
$('save-scene').onclick=action(async()=>{await save();toast('场景已保存');});
$('shortcut').onclick=action(async()=>{await save();await rpc('shortcut',{id:selected});toast('已创建桌面快捷方式');});
$('launch').onclick=action(async()=>{await save();$('restore-report').innerHTML='';delete $('restore-report').dataset.running;await rpc('launch',{id:selected});environment.busy=true;render();});
$('capture').onclick=action(async()=>{
  $('capture-list').innerHTML='<p class="muted">正在读取窗口…</p>';$('capture-dialog').showModal();
  captured=await rpc('capture');
  $('capture-list').innerHTML=captured.length?captured.map((item,index)=>`<label class="capture-item"><input type="checkbox" value="${index}"><div><strong>${escapeHtml(item.title)}</strong><small>${escapeHtml(item.path)} · ${escapeHtml(desktopName(item.desktop))}</small></div></label>`).join(''):'<p class="muted">没有可记录的应用窗口。</p>';
});
$('capture-confirm').onclick=()=>{
  const entries=[...$('capture-list').querySelectorAll('input:checked')].map(input=>captured[Number(input.value)].entry);
  if(!entries.length){toast('请先选择窗口');return;}
  scene().entries.push(...entries);markDirty();render();$('capture-dialog').close();
  const browser=entries.find(entry=>entry.kind==='web');if(browser){toast('已记录布局，请补充浏览器网址');openEntry(browser.id);}else toast(`已添加 ${entries.length} 个窗口，请保存配置`);
};
$('save-settings').onclick=action(async()=>{settings.hide_after_launch=$('hide-after-launch').checked;settings.hotkeys_enabled=$('hotkeys-enabled').checked;settings.otp_enabled=$('otp-enabled').checked;markDirty();await save();toast('运行设置已保存');});
$('save-connection').onclick=action(async()=>{const data=await rpc('connection',{url:$('server-url').value.trim(),token:$('server-token').value});$('server-token').value='';applyState(data);toast('连接已保存，正在检查服务');});
$('test-popup').onclick=action(async()=>{await rpc('test_popup');toast('已在主屏左下角显示测试弹窗');});
$('codes-list').onclick=action(async event=>{const button=event.target.closest('[data-copy]');if(button){await rpc('copy',{id:button.dataset.copy});toast('验证码已复制');}});
$('exit-app').onclick=action(async()=>{await rpc('exit');});
setInterval(()=>{if(lastCodes.length)renderCodes(lastCodes.filter(item=>Date.now()/1000-item.receivedAt<300));},10000);
rpc('ready').then(applyState).catch(error=>toast(error.message,true));
