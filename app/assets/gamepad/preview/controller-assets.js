(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const root = '../controllers/';
  let mapping = null, serial = 0;
  function update() {
    for (const stage of document.querySelectorAll('.stage')) {
      stage.className = 'stage bg-' + $('background').value;
      stage.style.setProperty('--custom-bg', $('color').value);
      stage.querySelector('.composition').classList.toggle('contrast-shadow', $('shadow').checked);
      const size = Number($('size').value);
      stage.querySelector('.composition').style.inset = (100-size)/2 + '%';
    }
    $('size-value').value = $('size').value + ' %';
    if (!mapping) return;
    for (const layer of mapping.layers.filter(layer => layer.role === 'stick')) {
      const side = layer.id === 'stick-l-cap' ? 'l' : 'r';
      let x=Number($(side+'x').value), y=Number($(side+'y').value);
      const length=Math.hypot(x,y), deadzone=layer.motion.deadzone;
      const amount=length<=deadzone ? 0 : Math.min(1,(length-deadzone)/(1-deadzone))/length;
      x*=amount; y*=amount;
      const image=$('composite').querySelector('[data-element="'+layer.id+'"]');
      if (image) image.style.transform=`translate(${x*layer.motion.travelRadius/mapping.viewBox[2]*100}%,${-y*layer.motion.travelRadius/mapping.viewBox[3]*100}%)`;
    }
  }
  async function load(entry) {
    const request=++serial;
    try {
      const response=await fetch(root+entry.mapping);if(!response.ok)throw Error('HTTP '+response.status);
      const next=await response.json();if(request!==serial)return;mapping=next;
      $('composite').replaceChildren();$('body-only').replaceChildren();$('mapping-body').replaceChildren();
      document.querySelectorAll('.stage').forEach(stage=>stage.style.aspectRatio=next.viewBox[2]+'/'+next.viewBox[3]);
      for (const layer of next.layers) {
        const image=document.createElement('img');image.src=root+next.id+'/'+layer.file;image.alt='';image.dataset.element=layer.id;
        $('composite').append(image);
        if(layer.role==='body')$('body-only').append(image.cloneNode());
        const row=document.createElement('tr');
        const name=document.createElement('td'),link=document.createElement('a');link.href=image.src;link.textContent=layer.id;name.append(link);row.append(name);
        for(const value of [layer.role,layer.role==='body'?'Statische Basis':layer.binding?JSON.stringify(layer.binding):'Keine qualifizierte Eingabezuordnung',layer.motion?JSON.stringify(layer.motion):'']){const cell=document.createElement('td');cell.textContent=value;row.append(cell);}
        $('mapping-body').append(row);
      }
      for(const side of ['l','r'])for(const axis of ['x','y'])$(side+axis).disabled=!next.layers.some(layer=>layer.id==='stick-'+side+'-cap');
      $('mapping-link').href=root+entry.mapping;$('source-link').href=root+next.id+'/'+next.source;
      $('controller-name').textContent=next.name;$('load-status').textContent='Aus separaten SVG-Elementen und mapping.json zusammengesetzt. Keine Hardwareabnahme.';
      document.querySelectorAll('[data-layout]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.layout===entry.id)));
      $('layout').value=entry.id;for(const axis of ['lx','ly','rx','ry'])$(axis).value=0;update();
    } catch(error) {$('load-status').textContent='Grafik konnte nicht geladen werden: '+error.message;}
  }
  for(const id of ['background','shadow'])$(id).addEventListener('change',update);
  $('color').addEventListener('input',()=>{$('background').value='custom';update();});
  for(const id of ['size','lx','ly','rx','ry'])$(id).addEventListener('input',update);
  $('neutral').addEventListener('click',()=>{for(const id of ['lx','ly','rx','ry'])$(id).value=0;update();});
  fetch(root+'catalog.json').then(response=>{if(!response.ok)throw Error('HTTP '+response.status);return response.json();}).then(catalog=>{
    for(const entry of catalog.controllers){const option=document.createElement('option');option.value=entry.id;option.textContent=entry.name;$('layout').append(option);const button=document.createElement('button');button.type='button';button.className='pad-card';button.dataset.layout=entry.id;button.textContent=entry.name;button.addEventListener('click',()=>load(entry));$('gallery').append(button);}
    $('layout').addEventListener('change',()=>load(catalog.controllers.find(entry=>entry.id===$('layout').value)));
    load(catalog.controllers[0]);
  }).catch(error=>{$('load-status').textContent='Katalog konnte nicht geladen werden: '+error.message;});
  update();
})();
