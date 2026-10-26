(async () => {
  'use strict';
  const NS = 'http://www.w3.org/2000/svg';
  const TRAVEL = 8;
  const $ = id => document.getElementById(id);
  const STD = ['south','east','west','north','l1','r1','l2','r2','view','menu','l3','r3','up','down','left','right','home','extra'];
  const catalogResponse=await fetch('../controllers/catalog.json',{cache:'no-cache'});
  if(!catalogResponse.ok)throw Error('Controllerkatalog: HTTP '+catalogResponse.status);
  const catalog=await catalogResponse.json();
  const pads=catalog.controllers.map(entry=>({id:entry.id,name:entry.name,nintendo:entry.family==='nintendo',sony:entry.family==='playstation',file:`../controllers/${entry.id}/source.svg`,manifest:'../controllers/'+entry.mapping,kind:'AL2009man · SVG + JSON',map:{}}));
  const labels = {south:'A',east:'B',west:'X',north:'Y',l1:'L1 / LB',r1:'R1 / RB',l2:'L2 / LT',r2:'R2 / RT',view:'View / Select',menu:'Menu / Start',l3:'L3',r3:'R3',up:'D-Pad ↑',down:'D-Pad ↓',left:'D-Pad ←',right:'D-Pad →',home:'Home / PS',extra:'Zusatz',mute:'Mikrofontaste'};
  const bits = {menu:1,view:2,south:4,east:8,west:16,north:32,up:64,down:128,left:256,right:512,l1:1024,r1:2048,l3:4096,r3:8192};
  const svg = $('pad'), host = $('host'), stage = $('stage');
  const empty = () => ({on:new Set(),lx:0,ly:0,rx:0,ry:0,lt:0,rt:0,touches:{l:null,r:null}});
  let manual = empty(), view = null, selected = pads[0], request = 0, drag = null;
  let automaticDevice = null, automaticEngaged = false, deviceSignature = '', lastReadout = 0, fileURL = '';
  const cache = new Map();
  let prefs = {};
  try { prefs = JSON.parse(localStorage.getItem('exosnap-pad-review-v2') || '{}'); } catch {}
  if(!prefs.appearanceVersion&&Number(prefs.detailWidth)===75&&Number(prefs.detailOpacity)===65){prefs.detailWidth=50;prefs.detailOpacity=40;}
  const make = (tag, attrs = {}, parent = host) => {
    const n = document.createElementNS(NS,tag);
    Object.entries(attrs).forEach(([k,v]) => n.setAttribute(k,v));
    parent.append(n); return n;
  };
  function save() {
    const state = {pad:selected.id,look:$('look').value,bg:$('background').value,color:$('background-color').value,size:$('size').value,opacity:$('opacity').value,glow:$('glow').checked,shadow:$('shadow').checked,detailWidth:$('detail-width').value,detailOpacity:$('detail-opacity').value,appearanceVersion:3};
    if (state.bg === 'file') state.bg = 'scene';
    try { localStorage.setItem('exosnap-pad-review-v2',JSON.stringify(state)); } catch {}
  }
  function localCenter(node) {
    const b = node.getBBox();
    return new DOMPoint(b.x+b.width/2,b.y+b.height/2).matrixTransform(svg.getScreenCTM().inverse().multiply(node.getScreenCTM()));
  }
  function label(key) {
    if(selected.labels?.[key])return selected.labels[key];
    const auxiliary={'touch-l':'Touchpad links · Kontakt','touch-r':'Touchpad rechts · Kontakt','aux-l':'L · Zusatz','aux-r':'R · Zusatz','aux-zl':'ZL · Zusatz','aux-zr':'ZR · Zusatz','mute':'Mikrofontaste','quick-access':'Schnellzugriff'};
    if(auxiliary[key])return auxiliary[key];
    if (selected.sony) return ({south:'Kreuz ×',east:'Kreis ○',west:'Quadrat □',north:'Dreieck △',view:selected.id==='dualshock4'?'Share':'Create',menu:'Options',extra:'Touchpad-Klick'})[key] || labels[key];
    if (selected.nintendo) return ({south:'B',east:'A',west:'Y',north:'X',view:'Minus −',menu:'Plus +',l2:'ZL',r2:'ZR',extra:'Capture'})[key] || labels[key];
    return key==='extra'?'Share':labels[key];
  }
  function rememberDetails(mapped) {
    view.details=[...host.querySelectorAll('.detail')].map(node=>({node,width:parseFloat(getComputedStyle(node).strokeWidth)/(mapped?(view.mapping.appearance?.detailWidth||50)/100:1)}));
    for(const id of ['detail-width','detail-opacity'])$(id).disabled=!view.details.length;
    applyDetails();
  }
  function applyDetails() {
    const width=Number($('detail-width').value)/100,opacity=Number($('detail-opacity').value)/100;
    for(const detail of view?.details||[]){detail.node.style.strokeWidth=String(detail.width*width);detail.node.style.strokeOpacity=String(opacity);detail.node.style.fillOpacity=String(opacity);}
    $('detail-width-value').textContent=Math.round(width*100)+' %';$('detail-opacity-value').textContent=Math.round(opacity*100)+' %';
  }
  function buildMapped(text,pad,mapping) {
    const doc=new DOMParser().parseFromString(text,'image/svg+xml');
    if(doc.querySelector('parsererror'))throw Error('Ungültiges SVG');
    const [vx,vy,vw,vh]=mapping.viewBox,scale=Math.min(480/vw,320/vh)*0.96;
    const drawing=make('g',{transform:`translate(${(480-vw*scale)/2-vx*scale} ${(320-vh*scale)/2-vy*scale}) scale(${scale})`});
    drawing.append(...[...doc.documentElement.children].map(node=>document.importNode(node,true)));
    host.setAttribute('class','mapped');
    const entries={},fills={},caps={},centers={},travel={},touches={},axisOutputs={},defs=make('defs');
    pad.labels=mapping.controlLabels||{};
    const maskKeys=Object.fromEntries(Object.entries(bits).map(([key,value])=>[value,key]));
    const matrixFor=node=>host.getScreenCTM().inverse().multiply(node.getScreenCTM());
    const matrixText=matrix=>`matrix(${[matrix.a,matrix.b,matrix.c,matrix.d,matrix.e,matrix.f].join(' ')})`;
    const visible=node=>{for(let parent=node;parent&&parent!==drawing;parent=parent.parentElement)if(getComputedStyle(parent).display==='none')return false;return true;};
    function entry(key,idle,sourceId,clipPoints=null,pressId=null,pressurePoints=null,tint=false) {
      const matrix=matrixFor(idle),wrapper=make('g',{'data-btn':key,tabindex:0,role:'button','aria-label':label(key)});
      const candidates=[...(idle.matches('path,rect,circle,ellipse,polygon')?[idle]:[]),...idle.querySelectorAll('path,rect,circle,ellipse,polygon')].filter(visible);
      let shape=null,largest=-1;
      for(const candidate of candidates){const box=candidate.getBBox(),m=matrixFor(candidate),area=box.width*box.height*Math.abs(m.a*m.d-m.b*m.c);if(area>largest){shape=candidate;largest=area;}}
      if(pressId)shape=idle.querySelector('#'+pressId)||shape;
      let highlight=null;
      if(shape){
        const transform=matrixFor(shape),copy=shape.cloneNode(false);copy.removeAttribute('id');
        copy.setAttribute('transform',matrixText(transform));copy.setAttribute('style','fill:#4cb8ff;fill-opacity:0.36;stroke:none;pointer-events:none');
        if(copy.localName==='path'&&!tint){const d=copy.getAttribute('d'),next=d.slice(1).search(/[Mm]/);if(next>=0)copy.setAttribute('d',d.slice(0,next+1));}
        const hit=copy.cloneNode(true);hit.setAttribute('style','fill:transparent;stroke:none;pointer-events:all');wrapper.append(hit);
        highlight=make('g',{'class':'active-variant'},wrapper);highlight.append(copy);highlight.style.display='none';
        if(pressurePoints){const clip=make('clipPath',{id:'pressure-region-'+key,clipPathUnits:'userSpaceOnUse'},defs);make('polygon',{points:pressurePoints},clip);for(const node of [hit,highlight])node.setAttribute('clip-path',`url(#pressure-region-${key})`);}
      }
      idle.setAttribute('transform',matrixText(matrix));idle.style.pointerEvents='none';wrapper.append(idle);
      if(clipPoints){const clip=make('clipPath',{id:'mapped-region-'+key,clipPathUnits:'userSpaceOnUse'},defs);make('polygon',{points:clipPoints},clip);wrapper.setAttribute('clip-path',`url(#mapped-region-${key})`);}
      if(tint&&highlight){highlight.remove();highlight=null;}
      entries[key]={idle,active:highlight,wrapper,keepIdle:true,sourceId,tint:tint?shape:null};pad.map[key]=sourceId;
      const box=wrapper.getBBox();
      if(key==='l2'||key==='r2'){
        const clip=make('clipPath',{id:'mapped-fill-'+key,clipPathUnits:'userSpaceOnUse'},defs),rect=make('rect',{x:box.x-2,y:box.y+box.height,width:box.width+4,height:0},clip);
        if(highlight)highlight.setAttribute('clip-path',`url(#mapped-fill-${key})`);fills[key]={rect,top:box.y,h:box.height};
      }
      if(key==='l3'||key==='r3'){const side=key==='l3'?'l':'r';caps[side]=wrapper;centers[side]=localCenter(wrapper);}
      return wrapper;
    }
    function touchSurface(layer,idle) {
      const side=layer.id.endsWith('l')?'l':'r',key='touch-'+side,matrix=matrixFor(idle),wrapper=make('g',{'data-touch':side,tabindex:0,role:'button','aria-label':label(key)}),bounds={x:(480-vw*scale)/2+(layer.bounds.x-vx)*scale,y:(320-vh*scale)/2+(layer.bounds.y-vy)*scale,w:layer.bounds.width*scale,h:layer.bounds.height*scale};
      const candidates=[...(idle.matches('path,rect,circle,ellipse')?[idle]:[]),...idle.querySelectorAll('path,rect,circle,ellipse')].filter(visible);let shape=null,largest=-1;
      for(const candidate of candidates){const b=candidate.getBBox(),m=matrixFor(candidate),area=b.width*b.height*Math.abs(m.a*m.d-m.b*m.c);if(area>largest){shape=candidate;largest=area;}}
      const clip=make('clipPath',{id:'touch-region-'+side,clipPathUnits:'userSpaceOnUse'},defs);
      if(shape){const copy=shape.cloneNode(false);copy.removeAttribute('id');copy.setAttribute('transform',matrixText(matrixFor(shape)));copy.setAttribute('style','fill:white;stroke:none;opacity:1');clip.append(copy);const hit=copy.cloneNode(true);hit.setAttribute('style','fill:transparent;stroke:none;pointer-events:all');wrapper.append(hit);}
      idle.setAttribute('transform',matrixText(matrix));wrapper.append(idle);
      const marker=make('circle',{'data-touch-marker':side,r:Math.min(bounds.w,bounds.h)*0.075,fill:'#4cb8ff','fill-opacity':0.8,stroke:'#ffffff','stroke-width':0.7,'clip-path':`url(#touch-region-${side})`,'pointer-events':'none'},wrapper);marker.style.display='none';
      touches[side]={marker,bounds,wrapper};entries[key]={idle,active:null,wrapper,sourceId:layer.id,keepIdle:true,binding:null,touch:true};pad.map[key]=layer.id;
      if(layer.axisOutput){const output=make('circle',{'data-axis-output':side,r:Math.min(bounds.w,bounds.h)*0.075,fill:'none',stroke:'#4cb8ff','stroke-width':1.5,'clip-path':`url(#touch-region-${side})`,'pointer-events':'none'},wrapper);output.style.display='none';axisOutputs[side]={marker:output,bounds,sourceId:layer.id,...layer.axisOutput};}
    }
    for(const layer of mapping.layers){
      if(layer.role==='body'||layer.visible===false)continue;
      const idle=drawing.querySelector('#'+layer.id);if(!idle)continue;
      if(layer.role==='touch-surface'){touchSurface(layer,idle);continue;}
      const binding=layer.binding;
      let key=binding?.type==='button'?maskKeys[binding.mask]:binding?.type==='stick'?(binding.clickMask===4096?'l3':'r3'):binding?.type==='axis'?(binding.field==='left_trigger'?'l2':'r2'):binding?.type==='auxiliary'?({home:'home',share:'extra',touchpad:'extra'})[binding.field]:null;
      if(binding?.type==='directional-pad'){
        const matrix=matrixFor(idle),box=layer.bounds,x=(480-vw*scale)/2+(box.x-vx)*scale,y=(320-vh*scale)/2+(box.y-vy)*scale,w=box.width*scale,h=box.height*scale,cx=x+w/2,cy=y+h/2;
        const regions={up:`${cx},${cy} ${x},${y} ${x+w},${y}`,right:`${cx},${cy} ${x+w},${y} ${x+w},${y+h}`,down:`${cx},${cy} ${x+w},${y+h} ${x},${y+h}`,left:`${cx},${cy} ${x},${y+h} ${x},${y}`};
        for(const [direction,points]of Object.entries(regions)){const copy=idle.cloneNode(true);copy.removeAttribute('id');copy.querySelectorAll('[id]').forEach(node=>node.id=direction+'-'+node.id);copy.setAttribute('transform',matrixText(matrix));host.append(copy);entry(direction,copy,layer.id,points,layer.pressElement?direction+'-'+layer.pressElement:null);}
        idle.remove();continue;
      }
      key=key||layer.preview?.key;
      if(!key)continue;
      let clipPoints=null;
      if(layer.clip){const b=layer.clip,x=(480-vw*scale)/2+(b.x-vx)*scale,y=(320-vh*scale)/2+(b.y-vy)*scale,w=b.width*scale,h=b.height*scale;clipPoints=`${x},${y} ${x+w},${y} ${x+w},${y+h} ${x},${y+h}`;}
      let pressurePoints=null;if(layer.pressClip){const b=layer.pressClip,x=(480-vw*scale)/2+(b.x-vx)*scale,y=(320-vh*scale)/2+(b.y-vy)*scale,w=b.width*scale,h=b.height*scale;pressurePoints=`${x},${y} ${x+w},${y} ${x+w},${y+h} ${x},${y+h}`;}
      entry(key,idle,layer.id,clipPoints,layer.pressElement,pressurePoints,layer.pressMode==='tint');entries[key].binding=binding;
      if(layer.role==='stick')travel[key==='l3'?'l':'r']=layer.motion.travelRadius*scale;
    }
    if(mapping.layers.some(layer=>layer.role==='directional-pad'))for(const key of ['up','right','down','left'])if(entries[key])host.append(entries[key].wrapper);
    view={entries,fills,caps,centers,travel,mapping,touches,axisOutputs};rememberDetails(true);applyLook();buildMappings();
    host.dataset.layout=pad.id;
    $('touch-note').hidden=!Object.keys(touches).length;
  }
  async function loadPad(id) {
    const serial = ++request;
    if(id==='steam_deck')id=catalog.defaultLayouts.steam_deck;
    selected = pads.find(p => p.id === id) || pads[0];
    const pad = selected;
    $('layout').value = selected.id;
    $('asset-status').textContent = selected.kind;
    document.querySelectorAll('[data-pad]').forEach(n => n.setAttribute('aria-pressed',String(n.dataset.pad===selected.id)));
    view = null; host.replaceChildren();host.dataset.layout='';
    $('reference').hidden = !selected.reference;
    svg.style.display = selected.reference?'none':'';
    $('reference-note').hidden = !selected.reference;
    $('look').disabled = selected.reference || !!selected.manifest;
    if(selected.manifest)$('look').value='lines';
    $('look').title=selected.manifest?'Für diese SVG-Quelle ist aktuell die Linienvariante verfügbar.':'';
    $('glow').disabled = selected.reference;
    if (selected.reference) {
      $('reference').src = selected.file;
      $('reference').alt = selected.name+' – lizenzierte Referenzgrafik';
      buildMappings(); save(); return;
    }
    try {
      await Promise.all([pad.file,pad.manifest].filter(Boolean).map(async file=>{if(cache.has(file))return;const response=await fetch(file,{cache:'no-cache'});if(!response.ok)throw Error(`HTTP ${response.status}`);cache.set(file,await response.text());}));
      if (serial!==request) return;
      buildMapped(cache.get(pad.file),pad,JSON.parse(cache.get(pad.manifest)));
      save();update(performance.now(),true);
    } catch (error) { $('source-status').textContent = 'Grafik konnte nicht geladen werden: '+error.message; }
  }
  function buildMappings() {
    const body = $('mapping-body'); body.replaceChildren();
    for (const key of [...new Set([...STD,...Object.keys(view?.entries||{})])]) {
      const entry = view?.entries[key];
      if (!entry) continue;
      const row = document.createElement('tr'); row.dataset.logical = key;
      const name = document.createElement('td');
      const button = document.createElement('button');
      button.type = 'button'; button.textContent = label(key); button.className='map-button';
      button.disabled = !entry;
      button.addEventListener('click',() => toggle(key)); name.append(button); row.append(name);
      const values = [entry.sourceId||selected.map[key],
        entry.binding===null?'nur Vorschau · nicht qualifiziert':key==='l2'?'leftTrigger':key==='r2'?'rightTrigger':bits[key]?'0x'+bits[key].toString(16).toUpperCase().padStart(4,'0'):'optional / geräteabhängig',
        STD.includes(key)?key==='extra'?(selected.sony?'b17*':'nicht standardisiert'):'b'+STD.indexOf(key):'nicht standardisiert','0'];
      for (const value of values) {const td=document.createElement('td'); td.textContent=value; row.append(td);}
      body.append(row);
    }
    for (const [side,field,axes] of [['Linker Stick','leftThumbstickX/Y','a0 / −a1'],['Rechter Stick','rightThumbstickX/Y','a2 / −a3']]) {
      const key=side==='Linker Stick'?'l':'r',sourceId=selected.map[key==='l'?'l3':'r3']||view?.axisOutputs?.[key]?.sourceId;
      if(!sourceId)continue;
      const row=document.createElement('tr');
      for(const text of [view?.axisOutputs?.[key]?side+'-Ausgabe':side,sourceId,field,axes,'X/Y']){const td=document.createElement('td');td.textContent=text;row.append(td);}
      body.append(row);
    }
  }
  function applyLook() {
    host.classList.toggle('body-only',$('body-only').checked);
    const glass = $('look').value === 'glass';
    svg.classList.toggle('glass',glass); svg.classList.toggle('glow',$('glow').checked);
    svg.style.filter=$('shadow').checked?'drop-shadow(0 1px 1px #000b)':'';
    if (!selected.legacy) {
      const body=host.querySelector('#body'), glassBody=host.querySelector('#body-glass');
      if(body)body.style.display=glass?'none':'';
      if(glassBody)glassBody.style.display=glass?'':'none';
    }
  }
  function radial(x,y) {const length=Math.hypot(x,y);return length<=0.08?[0,0]:length>1?[x/length,y/length]:[x,y];}
  function gamepadState(p) {
    const s=empty();
    if(p.mapping!=='standard')return s;
    STD.forEach((key,i)=>{
      if(key==='extra' && !selected.sony)return;
      const b=p.buttons[i];if(!b)return;
      if(b.pressed)s.on.add(key);
      if(key==='l2')s.lt=b.value;if(key==='r2')s.rt=b.value;
    });
    [s.lx,s.ly]=radial(p.axes[0]||0,-(p.axes[1]||0));
    [s.rx,s.ry]=radial(p.axes[2]||0,-(p.axes[3]||0));return s;
  }
  function readDevice() {
    let connected=[];
    try {connected=[...(navigator.getGamepads?.()||[])].filter(p=>p?.connected);}catch{}
    const signature=connected.map(p=>p.index+':'+p.id).join('|');
    if(signature!==deviceSignature){
      deviceSignature=signature;const previous=$('device').value;
      $('device').replaceChildren(new Option('Automatisch · aktives Gerät','auto'),...connected.map(p=>new Option(`${p.index+1}: ${p.id}`,String(p.index))));
      if(previous!=='auto' && !connected.some(p=>String(p.index)===previous))$('device').add(new Option('Gewähltes Gerät getrennt',previous));
      $('device').value=previous;
    }
    if($('device').value!=='auto')return connected.find(p=>String(p.index)===$('device').value)||null;
    const active=p=>p.buttons.some(b=>b.value>0.2)||p.axes.some(a=>Math.abs(a)>0.2);
    let p=connected.find(p=>p.index===automaticDevice);
    if(!p){p=connected.find(active)||connected[0];automaticEngaged=!!p&&active(p);}
    else if(!automaticEngaged){p=connected.find(active)||p;automaticEngaged=active(p);}
    automaticDevice=p?.index??null;return p||null;
  }
  function demo(t) {
    const s=empty();s.lx=Math.cos(t*1.6)*0.9;s.ly=Math.sin(t*1.6)*0.9;s.rx=Math.sin(t*2.1)*0.8;s.ry=Math.sin(t*4.2)*0.5;
    s.lt=Math.max(0,Math.sin(t*1.3));s.rt=Math.max(0,Math.sin(t*1.3+Math.PI));
    const sequence=['south','east','north','west','up','right','down','left','l1','r1','menu','view','home','extra'];
    const i=Math.floor(t/0.55)%(sequence.length+4);if(i<sequence.length)s.on.add(sequence[i]);
    if(t%7>5.2)s.on.add('l3');if((t+3.5)%7>5.2)s.on.add('r3');
    for(const [side,index]of [['l',0],['r',1]])if(view?.touches?.[side]&&(t+index*1.2)%6>3){s.touches[side]={x:0.5+Math.sin(t*1.3+index)*0.28,y:0.5+Math.cos(t*1.1+index)*0.28};s.on.add('touch-'+side);}
    return s;
  }
  function render(s,now,force=false) {
    if(view){
      for(const [key,e] of Object.entries(view.entries)){
        const trigger=key==='l2'?s.lt:key==='r2'?s.rt:null;
        const on=trigger===null?s.on.has(key):trigger>0;
        e.wrapper.classList.toggle('on',on);e.idle.classList.toggle('on',on);e.wrapper.setAttribute('aria-pressed',String(on));
        if(e.tint)e.tint.style.fill=on?'#4cb8ff':'#ffffff';
        if(e.active){e.active.style.display=on?'':'none';if(trigger===null&&!e.keepIdle)e.idle.style.display=on?'none':'';}
      }
      for(const [key,value]of [['l2',s.lt],['r2',s.rt]]){const f=view.fills[key];if(f){f.rect.setAttribute('y',f.top+f.h*(1-value));f.rect.setAttribute('height',f.h*value);}}
      const [lx,ly]=radial(s.lx,s.ly),[rx,ry]=radial(s.rx,s.ry);
      if(view.caps.l)view.caps.l.setAttribute('transform',`translate(${lx*(view.travel?.l||TRAVEL)} ${-ly*(view.travel?.l||TRAVEL)})`);
      if(view.caps.r)view.caps.r.setAttribute('transform',`translate(${rx*(view.travel?.r||TRAVEL)} ${-ry*(view.travel?.r||TRAVEL)})`);
      for(const [side,touch]of Object.entries(view.touches||{})){
        const contact=s.touches[side],b=touch.bounds;touch.marker.style.display=contact?'':'none';
        if(contact){touch.marker.setAttribute('cx',b.x+contact.x*b.w);touch.marker.setAttribute('cy',b.y+contact.y*b.h);}
        const output=view.axisOutputs?.[side];if(output){const x=s[side+'x'],y=s[side+'y'],clicked=s.on.has(side==='l'?'l3':'r3');output.marker.style.display=!contact&&(Math.hypot(x,y)>0.08||clicked)?'':'none';output.marker.setAttribute('cx',b.x+b.w*(0.5+x*0.4));output.marker.setAttribute('cy',b.y+b.h*(0.5-y*0.4));output.marker.setAttribute('stroke-width',clicked?2.4:1.5);}
      }
    }
    renderStickFields(s);
    if(!force&&now-lastReadout<80)return;lastReadout=now;
    $('readout').textContent=`LS ${s.lx.toFixed(2)} / ${s.ly.toFixed(2)}    RS ${s.rx.toFixed(2)} / ${s.ry.toFixed(2)}    L2 ${s.lt.toFixed(2)}    R2 ${s.rt.toFixed(2)}    ${[...s.on].join(' · ')||'Ruhezustand'}`;
    document.querySelectorAll('tr[data-logical]').forEach(row=>{
      const key=row.dataset.logical;const value=key==='l2'?s.lt:key==='r2'?s.rt:s.on.has(key)?1:0;
      row.classList.toggle('active',value>0);row.lastElementChild.textContent=value.toFixed(2);
      row.querySelector('button')?.setAttribute('aria-pressed',String(value>0));
    });
  }
  function update(now=performance.now(),force=false) {
    const p=readDevice();const mode=$('mode').value;let s=empty(),message='Manuelle Vorschau';
    if(mode==='demo'){s=demo(matchMedia('(prefers-reduced-motion: reduce)').matches?1.2:now/1000);message='Animation · simulierte Eingabe';}
    else if(mode==='manual')s=manual;
    else if(p){s=gamepadState(p);message=p.mapping==='standard'?`Browser-Gamepad · ${p.id}`:'Browser meldet kein Standard-Mapping · Rohdaten unten';}
    else message='Browser-Gamepad · kein Gerät erkannt; Taste drücken';
    if(selected.reference)message='Referenzgrafik · SVG-Mapping noch offen';
    $('source-status').textContent=message;
    $('status').dataset.src=mode==='browser'&&p?'pad':mode;
    $('status').firstElementChild.textContent=selected.reference?'Referenz':mode==='browser'?(p?'Browser verbunden':'Browser wartet'):mode==='demo'?'Demo':'Manuell';
    if(p && mode==='browser')$('raw').textContent=JSON.stringify({id:p.id,mapping:p.mapping,buttons:p.buttons.map(b=>Number(b.value.toFixed(3))),axes:p.axes.map(a=>Number(a.toFixed(3)))},null,2);
    else $('raw').textContent='Browser-Rohdaten erscheinen im Modus „Browser-Gamepad“. Die Browser-API prüft nicht die native GameInput-Anbindung.';
    render(s,now,force);
  }
  function frame(now){update(now);requestAnimationFrame(frame);}
  function toggle(key){$('mode').value='manual';if(manual.on.has(key))manual.on.delete(key);else manual.on.add(key);if(key.startsWith('touch-'))manual.touches[key.at(-1)]=manual.on.has(key)?{x:0.5,y:0.5}:null;if(key==='l2')manual.lt=manual.on.has(key)?1:0;if(key==='r2')manual.rt=manual.on.has(key)?1:0;syncSliders();update(performance.now(),true);}
  function syncSliders(){for(const key of ['lx','ly','rx','ry','lt','rt'])$(key).value=manual[key];}
  function renderStickFields(s) {
    for(const side of ['l','r']){
      const field=$('stick-'+side),output=view?.axisOutputs?.[side],enabled=!!view?.caps[side]||!!output,x=s[side+'x'],y=s[side+'y'];
      field.closest('fieldset').querySelector('legend').textContent=output&&!view?.caps[side]?(side==='l'?'Linke':'Rechte')+' Stick-Ausgabe · Trackpad':(side==='l'?'Linker':'Rechter')+' Stick';
      field.setAttribute('aria-disabled',String(!enabled));field.tabIndex=enabled?0:-1;
      field.querySelector('.stick-thumb').style.left=(50+(enabled?x:0)*36)+'%';field.querySelector('.stick-thumb').style.top=(50-(enabled?y:0)*36)+'%';
      $('stick-'+side+'-value').textContent=enabled?x.toFixed(2)+' / '+y.toFixed(2):'nicht vorhanden';
      $('click-'+side).disabled=!enabled;$('click-'+side).setAttribute('aria-pressed',String(enabled&&s.on.has(side==='l'?'l3':'r3')));
      $('center-'+side).disabled=!enabled;
    }
  }
  function setStick(side,x,y) {
    const length=Math.hypot(x,y);if(length>1){x/=length;y/=length;}
    manual[side+'x']=x;manual[side+'y']=y;$('mode').value='manual';syncSliders();update(performance.now(),true);
  }
  for(const side of ['l','r']){
    const field=$('stick-'+side);let pointer=null;
    const move=e=>{const rect=field.getBoundingClientRect(),radius=Math.min(rect.width,rect.height)*0.36;setStick(side,(e.clientX-rect.left-rect.width/2)/radius,(rect.top+rect.height/2-e.clientY)/radius);};
    field.addEventListener('pointerdown',e=>{if(field.getAttribute('aria-disabled')==='true'||e.button!==0)return;e.preventDefault();field.focus({preventScroll:true});pointer=e.pointerId;field.setPointerCapture(pointer);move(e);});
    field.addEventListener('pointermove',e=>{if(e.pointerId===pointer)move(e);});
    for(const type of ['pointerup','pointercancel','lostpointercapture'])field.addEventListener(type,e=>{if(e.pointerId===pointer)pointer=null;});
    field.addEventListener('dblclick',()=>setStick(side,0,0));
    field.addEventListener('keydown',e=>{
      if(field.getAttribute('aria-disabled')==='true')return;
      const step=e.shiftKey?0.02:0.1;let x=manual[side+'x'],y=manual[side+'y'];
      if(e.key==='ArrowLeft')x-=step;else if(e.key==='ArrowRight')x+=step;else if(e.key==='ArrowUp')y+=step;else if(e.key==='ArrowDown')y-=step;else if(['Home',' ','Enter'].includes(e.key))x=y=0;else return;
      e.preventDefault();setStick(side,x,y);
    });
    $('center-'+side).addEventListener('click',()=>setStick(side,0,0));
    $('click-'+side).addEventListener('click',()=>toggle(side==='l'?'l3':'r3'));
  }
  const point=e=>new DOMPoint(e.clientX,e.clientY).matrixTransform(svg.getScreenCTM().inverse());
  function moveTouch(e){const p=point(e),b=view.touches[drag.side].bounds;let x=(p.x-b.x)/b.w,y=(p.y-b.y)/b.h;if(view.mapping.layers.find(layer=>layer.id==='trackpad-'+drag.side)?.touch?.shape==='ellipse'){let dx=(x-0.5)*2,dy=(y-0.5)*2;const length=Math.hypot(dx,dy);if(length>1){dx/=length;dy/=length;}x=0.5+dx/2;y=0.5+dy/2;}manual.touches[drag.side]={x:Math.max(0,Math.min(1,x)),y:Math.max(0,Math.min(1,y))};manual.on.add('touch-'+drag.side);update(performance.now(),true);}
  svg.addEventListener('pointerdown',e=>{
    const surface=e.target.closest('[data-touch]');if(surface&&view){e.preventDefault();$('mode').value='manual';svg.setPointerCapture(e.pointerId);drag={kind:'touch',side:surface.dataset.touch};moveTouch(e);return;}
    const button=e.target.closest('[data-btn]');if(!button||!view)return;
    const key=button.dataset.btn;
    $('mode').value='manual';svg.setPointerCapture(e.pointerId);
    if(key==='l3'||key==='r3'){const side=key==='l3'?'l':'r';const p=point(e);drag={key,side,start:p,center:view.centers[side],moved:0};}
    else toggle(key);
  });
  svg.addEventListener('pointermove',e=>{if(!drag)return;if(drag.kind==='touch'){moveTouch(e);return;}const p=point(e),travel=view.travel?.[drag.side]||TRAVEL;drag.moved=Math.max(drag.moved,Math.hypot(p.x-drag.start.x,p.y-drag.start.y));[manual[drag.side+'x'],manual[drag.side+'y']]=radial((p.x-drag.center.x)/travel,(drag.center.y-p.y)/travel);syncSliders();update(performance.now(),true);});
  const release=()=>{if(!drag)return;if(drag.kind==='touch'){manual.touches[drag.side]=null;manual.on.delete('touch-'+drag.side);}else if(drag.moved<3)toggle(drag.key);else{manual[drag.side+'x']=0;manual[drag.side+'y']=0;}drag=null;syncSliders();update(performance.now(),true);};
  svg.addEventListener('pointerup',release);svg.addEventListener('pointercancel',release);
  svg.addEventListener('keydown',e=>{const key=e.target.dataset.btn||(e.target.dataset.touch?'touch-'+e.target.dataset.touch:null);if((e.key===' '||e.key==='Enter')&&key){e.preventDefault();toggle(key);}});
  function background(){stage.className='stage bg-'+$('background').value;stage.style.setProperty('--custom-bg',$('background-color').value);$('bg-video').hidden=$('background').value!=='file'||!$('bg-video').src;$('bg-image').hidden=$('background').value!=='file'||!$('bg-image').src;if($('bg-video').hidden)$('bg-video').pause();else $('bg-video').play().catch(()=>{});save();}
  $('background-file').addEventListener('change',e=>{const file=e.target.files[0];if(!file)return;if(!file.type.startsWith('image/')&&!file.type.startsWith('video/'))return;if(fileURL)URL.revokeObjectURL(fileURL);fileURL=URL.createObjectURL(file);$('bg-video').removeAttribute('src');$('bg-image').removeAttribute('src');$(file.type.startsWith('video/')?'bg-video':'bg-image').src=fileURL;$('background').value='file';background();});
  $('background').addEventListener('change',background);
  $('background-color').addEventListener('input',()=>{$('background').value='custom';background();});
  for(const key of ['lx','ly','rx','ry','lt','rt'])$(key).addEventListener('input',()=>{$('mode').value='manual';manual[key]=Number($(key).value);if(key==='lt'||key==='rt'){const button=key==='lt'?'l2':'r2';if(manual[key]>0)manual.on.add(button);else manual.on.delete(button);}update(performance.now(),true);});
  for(const id of ['look','glow','shadow','body-only'])$(id).addEventListener('change',()=>{applyLook();save();});
  for(const id of ['detail-width','detail-opacity'])$(id).addEventListener('input',()=>{applyDetails();save();});
  for(const id of ['size','opacity'])$(id).addEventListener('input',()=>{stage.style.setProperty('--pad-size',$('size').value+'%');svg.style.opacity=$('opacity').value/100;$('reference').style.opacity=$('opacity').value/100;$('size-value').textContent=$('size').value+' %';$('opacity-value').textContent=$('opacity').value+' %';save();});
  $('reset').addEventListener('click',()=>{manual=empty();$('mode').value='manual';syncSliders();update(performance.now(),true);});
  $('all').addEventListener('click',()=>{manual=empty();Object.keys(view?.entries||{}).forEach(k=>manual.on.add(k));for(const side of Object.keys(view?.touches||{}))manual.touches[side]={x:0.5,y:0.5};manual.lt=manual.rt=1;$('mode').value='manual';syncSliders();update(performance.now(),true);});
  $('layout').addEventListener('change',()=>loadPad($('layout').value));
  $('device').addEventListener('change',()=>{automaticDevice=null;automaticEngaged=false;update(performance.now(),true);});
  $('mode').addEventListener('change',()=>update(performance.now(),true));
  window.addEventListener('gamepadconnected',()=>update(performance.now(),true));
  window.addEventListener('gamepaddisconnected',()=>update(performance.now(),true));
  for(const p of pads){
    $('layout').add(new Option(p.name,p.id));
    const button=document.createElement('button');button.type='button';button.className='pad-card';button.dataset.pad=p.id;button.setAttribute('aria-pressed','false');
    const title=document.createElement('strong');title.textContent=p.name;const note=document.createElement('span');note.textContent=p.kind;button.append(title,note);button.addEventListener('click',()=>loadPad(p.id));$('gallery').append(button);
  }
  for(const id of ['look','background','background-color','size','opacity']){const key=id==='background'?'bg':id==='background-color'?'color':id;if(prefs[key]!==undefined)$(id).value=prefs[key];}
  $('glow').checked=!!prefs.glow;
  $('shadow').checked=prefs.shadow!==false;
  $('detail-width').value=prefs.detailWidth??50;$('detail-opacity').value=prefs.detailOpacity??40;
  if(!$('look').value)$('look').value='lines';if(!$('background').value)$('background').value='scene';
  stage.style.setProperty('--pad-size',$('size').value+'%');svg.style.opacity=$('opacity').value/100;$('reference').style.opacity=$('opacity').value/100;
  $('size-value').textContent=$('size').value+' %';$('opacity-value').textContent=$('opacity').value+' %';
  $('body-only').checked=new URLSearchParams(location.search).get('body')==='1';
  background();loadPad(new URLSearchParams(location.search).get('layout')||prefs.pad||'switch_pro').then(()=>requestAnimationFrame(frame));
})().catch(error=>{document.getElementById('source-status').textContent='Vorschau konnte nicht geladen werden: '+error.message;});
