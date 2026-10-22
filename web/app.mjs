import {Connection} from './protocol.mjs';
import {render,serialize,selectionFormat,formatSelection,formatParagraphs,plainDocument,importHtml,insertText,insertMark,markAt,acceptsEdit,withinTextLimits,characterCount,maxNoteCharacters} from './document.mjs';
const $=id=>document.getElementById(id);
const config=await fetch('/config.json').then(response=>response.json());
const t=key=>config.labels[key]||key;
$('follow').checked=true;
// Use a device category as the editable connection name.
const agent=navigator.userAgent;
$('device').value=/iPhone/i.test(agent)?'iPhone':
  (/iPad/i.test(agent)||(navigator.platform==='MacIntel'&&navigator.maxTouchPoints>1))?'iPad':
  /Android/i.test(agent)?'Android':/Windows/i.test(agent)?'Windows PC':
  /Macintosh|Mac OS X/i.test(agent)?'Mac':/Linux/i.test(agent)?'Linux PC':'Browser';
document.querySelectorAll('[data-text]').forEach(node=>{node.textContent=t(node.dataset.text);});
$('export').selectedIndex=-1;
$('brand').textContent=t('ScenePad');document.title=t('ScenePad');
$('search').placeholder=t('FindPlaceholder');$('search').setAttribute('aria-label',t('FindPlaceholder'));
for(const [id,key] of [['previous','FindPrevious'],['next','FindNext'],['clear-search','CloseSearch']]) {
  $(id).title=t(key);$(id).setAttribute('aria-label',t(key));
}
for(const [id,key] of [['scenes','RemoteScene'],['size','RemoteFontSize'],['family','Font'],['color','TextColor'],['highlight','Highlight'],['speed','Speed'],['multiplier','Speed'],['list','ListStyle'],['clear-format','ClearFormat'],['format-painter','FormatPainter'],['indent-less','DecreaseIndent'],['indent-more','IncreaseIndent'],['insert-time','InsertTime'],['insert-mark','InsertMark'],['next-mark','NextMark'],['import','RemoteImport'],['export','RemoteExport'],['countdown','RemoteCountdown'],['top','RemoteTop'],['guide','RemoteGuide']]) {
  $(id).setAttribute('aria-label',t(key));$(id).title=t(key);
}
$('countdown-label').title=t('RemoteCountdown');
$('target').title=t('RemoteTarget');
for(const id of ['lines','percent'])$(id).title=t('GoTo');
$('go-to').title=$('go-to').placeholder=t('GoToPrompt');$('go-to').setAttribute('aria-label',t('GoToPrompt'));
for(const [selector,key] of [['[data-format="weight"]','Bold'],['[data-format="italic"]','Italic'],['[data-format="underline"]','Underline'],['[data-format="strike"]','Strikethrough'],['[data-align="1"]','AlignLeft'],['[data-align="4"]','AlignCenter'],['[data-align="2"]','AlignRight'],['[data-align="8"]','AlignJustify']]) {
  const button=document.querySelector(selector);const label=t(key).replace(/\s*\(.*\)$/,'');button.setAttribute('aria-label',label);button.title=label;
}
let access=location.hash.slice(1);
// Remember the connection link for app launches without a URL fragment.
try{
  if(/^[A-Za-z0-9_-]{22}$/.test(access))localStorage.setItem('scenepad.link',access);
  else if(!location.hash)access=localStorage.getItem('scenepad.link')||'';
}catch{/* Storage can be disabled by the browser. */}
const sessionKey=`scenepad.session.${config.port}.${access}`;
const editorTools=document.querySelectorAll('#format button:not(#next-mark),#format input,#format select,#paragraph button,#paragraph select');
let connection,session='',epoch='',scene='',revision=0,model,remoteState={},online=false,editing=false,desired=false,connecting=false;
let importing=false,importController,saveTask,finishTask,finishing=false,acquiring=false;
let credential=null,goToState=null,paintFormat=null,paintSource=null,paintContinuous=false,usedCharacters=0;
let sequence=0,savedSequence=0,saving=false,saveTimer,draftTimer,reconnectTimer,heartbeat,reconnectDelay=1000,loadNumber=0,password='';
let playing=false,frame=0,lastFrame=0,smoothTarget=null,scrollPosition=0,expectedScrollTop=0,manualUntil=0,scrollDragging=false,localSpeed=.5,countdownTimer=0,searchTimer,markSearch=false,searchRanges=[],searchIndex=-1,draftProblem=false;
const draftKey=id=>`scenepad.draft.${location.host}.${id}`;
const drafts=new Map();
const notice=(key,conflict=false)=>{
  $('login-error').textContent=t(key);$('login-error').hidden=$('login').hidden;
  $('message').textContent=t(key);$('message').title=t(key);$('problem').hidden=false;
  $('message').classList.toggle('locked',key==='RemoteLocked');
  $('problem').classList.toggle('storage-error',key==='RemoteStorageFailed'||key==='RemoteNoDraft');
  $('reload').hidden=!conflict;$('download').hidden=!draftProblem;$('retry').hidden=conflict||key==='WordSimplified';
};
const clearNotice=()=>{$('problem').hidden=true;$('login-error').hidden=true;};
function forgetSession() {
  credential=null;password='';$('password').value='';
  try{sessionStorage.removeItem(sessionKey);}catch{/* Storage can be disabled by the browser. */}
}
function rememberSession() {
  try {
    if($('remember').checked&&credential)sessionStorage.setItem(sessionKey,JSON.stringify({credential,name:$('device').value}));
    else sessionStorage.removeItem(sessionKey);
  }catch{/* Storage can be disabled by the browser. */}
}
function storeDraft(document=serialize($('document'))) {
  // Keep oversized recovery data available for download.
  if(draftProblem){const previous=readDraft(scene);if(previous&&!withinTextLimits(previous.document))return false;}
  updateCharacters(document);
  const draft={epoch,revision,document};drafts.set(scene,draft);
  try {localStorage.setItem(draftKey(scene),JSON.stringify(draft));return true;}
  catch {notice('RemoteNoDraft');return false;}
}
function clearDraft() {drafts.delete(scene);try{localStorage.removeItem(draftKey(scene));}catch{/* Storage can be disabled by the browser. */}}
function readDraft(id) {
  if(drafts.has(id))return drafts.get(id);
  try {const value=JSON.parse(localStorage.getItem(draftKey(id)));return value?.document?.blocks?value:null;}
  catch {return null;}
}
async function call(action,extra={}) {
  const result=await connection.request({access,session,action,...extra});
  if(!result.ok)throw Object.assign(new Error(result.code),{code:result.code});return result;
}
function failure(error) {
  const code=error.code||error.message;
  notice(code==='limit'?'TextLimit':code==='locked'?'RemoteLocked':code==='word'?'WordOpenError':code==='upload'?'RemoteImportFailed':code==='busy'?'RemoteBusy':code==='format'?'RemoteFormat':code==='size'?'RemoteTooLarge':code==='read'||code==='save'?'RemoteStorageFailed':code==='conflict'?'RemoteConflict':code==='password-required'?'RemotePasswordRequired':'RemoteFailure',code==='conflict');
}
function iconButton(id,label,path) {
  const button=$(id);if(button.title===label&&button.firstElementChild?.tagName.toLowerCase()==='svg')return;button.title=label;button.setAttribute('aria-label',label);
  button.innerHTML=`<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${path}"/></svg>`;
}
function playState(active=false,remaining=0) {
  iconButton('play',remaining?String(remaining):t(active?'Pause':'RemotePlay'),active?'M8 5v14M16 5v14':'m6 4 12 8-12 8z');
  if(remaining)$('play').textContent=String(remaining);
}
function editUI() {
  if(goToState&&(!model||importing||goToState.scene!==scene||goToState.epoch!==epoch))hideGoTo();
  const writable=canWrite();
  if(!writable)cancelPainter();
  $('document').contentEditable=String(writable);
  editorTools.forEach(node=>{if(node.disabled===writable)node.disabled=!writable;});
  $('import').disabled=!online||!scene||saving||importing||finishing||draftProblem||remoteState.locked||(!editing&&!!remoteState.editor);
  $('export').disabled=!online||!scene||saving||importing||draftProblem;
  $('next-mark').disabled=!model||importing;
  $('lines').disabled=$('percent').disabled=!model||importing;
  $('countdown').disabled=$('target').value==='obs';
  $('disconnect').disabled=importing;
  $('play').disabled=importing;
  $('scenes').disabled=importing||draftProblem;$('follow').disabled=importing||draftProblem;
  const status=remoteState.locked?t('RemoteLocked'):importing?t('RemoteImporting'):editing?t(!saving&&sequence===savedSequence?'RemoteSaved':'RemoteSaving'):
    remoteState.editor?`${t('RemoteEditing')} · ${remoteState.editor}`:model&&!model.editable?t('RemoteReadOnly'):online?t('RemoteClickEdit'):'';
  $('owner').textContent=status;$('owner').title=model&&!model.editable&&!remoteState.locked&&!remoteState.editor?t('RemoteFormat'):status;
  $('owner').classList.toggle('locked',!!remoteState.locked);
  $('document').setAttribute('aria-readonly',String(!writable));
  $('retry').disabled=finishing||acquiring||importing||!!remoteState.locked&&draftProblem;
}
function canWrite(){return editing&&online&&!importing&&!finishing&&!remoteState.locked&&!remoteState.localEditing&&model?.editable;}

function updateState(state) {
  remoteState=state;
  const ids=Array.from($('scenes').options).map(option=>option.value).join(',');
  if(ids!==state.scenes.map(item=>item.id).join(',')) {
    $('scenes').replaceChildren(...state.scenes.map(item=>{const option=document.createElement('option');option.value=item.id;option.textContent=`[ ${item.name} ]`;return option;}));
  } else state.scenes.forEach((item,i)=>{$('scenes').options[i].textContent=`[ ${item.name} ]`;});
  $('scenes').value=scene;editUI();
  if($('target').value==='obs') {$('speed').value=state.speed;updateSpeed();playState(state.scrolling);syncScroll();}
}
function syncScroll() {
  // OBS owns the position only while viewing its current scene in OBS mode.
  if($('target').value!=='obs'||!online||editing||importing||draftProblem||document.hidden||
    remoteState.epoch!==epoch||remoteState.activeScene!==scene||!Number.isFinite(remoteState.position))return;
  const root=$('document');
  root.scrollTop=Math.max(0,Math.min(1,remoteState.position))*Math.max(0,root.scrollHeight-root.clientHeight);
}
async function load(id,{restore=true}={}) {
  if(!id)return;
  cancelPainter();
  const number=++loadNumber;
  let result;
  try {result=await call('GetDocument',{scene:id});}
  catch(error) {
    if(number!==loadNumber)return;
    if(!['size','read','save'].includes(error.code))throw error;
    // Keep document errors local to the selected scene and preserve any draft.
    stopLocal();scene=id;epoch=remoteState.epoch;revision=0;model=null;
    const draft=restore?readDraft(id):null;draftProblem=!!draft;sequence=savedSequence=0;
    const limited=draft&&!withinTextLimits(draft.document);
    render($('document'),limited?{blocks:[]}:draft?.document||{blocks:[]});$('scenes').value=id;
    updateCharacters(limited?{blocks:[]}:draft?.document||{blocks:[]});
    updateMetrics();scheduleSearch();editUI();notice(limited?'TextLimit':error.code==='size'?'RemoteTooLarge':'RemoteStorageFailed',draftProblem);
    if(limited)$('retry').hidden=true;
    return;
  }
  if(number!==loadNumber)return;
  const draft=restore?readDraft(id):null;
  stopLocal();scene=id;epoch=result.epoch;revision=result.revision;model=result;sequence=savedSequence=0;draftProblem=false;
  render($('document'),result);$('document').scrollTop=0;
  $('scenes').value=id;updateCharacters(result);
  if(draft) {
    draftProblem=true;
    if(!withinTextLimits(draft.document)){editUI();notice('TextLimit',true);$('retry').hidden=true;return;}
    render($('document'),draft.document);
    updateCharacters(draft.document);
    notice(draft.epoch===epoch&&draft.revision===revision?'RemoteDraft':'RemoteConflict',true);
    $('retry').hidden=!(draft.epoch===epoch&&draft.revision===revision&&result.editable);
  } else clearNotice();
  syncScroll();updateMetrics();scheduleSearch();editUI();
}
async function beginEdit(fromDraft=false) {
  if(playing||countdownTimer||editing||acquiring||finishing||!online||!scene||!model?.editable||remoteState.locked||remoteState.localEditing||(!fromDraft&&draftProblem))return;
  acquiring=true;const requestedScene=scene;
  try {
    const result=await call('AcquireEdit',{scene,epoch,revision});
    if(!online||scene!==requestedScene||remoteState.locked||(!fromDraft&&!$('document').contains(document.activeElement))) {
      if(online)await call('ReleaseEdit');return;
    }
    model=result;revision=result.revision;editing=true;draftProblem=false;stopLocal();sequence=fromDraft?1:0;savedSequence=0;
    clearNotice();editUI();$('document').focus();
    if(fromDraft){storeDraft();scheduleSave();}
  } finally {acquiring=false;editUI();}
}
function save() {
  clearTimeout(saveTimer);
  if(saveTask)return saveTask;
  if(!editing||!online||sequence===savedSequence)return Promise.resolve();
  saveTask=saveDocument().finally(()=>{saveTask=null;});return saveTask;
}
async function saveDocument() {
  // Reuse one snapshot for the recovery draft and the save request.
  const document=serialize($('document'));
  if(!withinTextLimits(document)){notice('TextLimit');return;}
  saving=true;clearTimeout(draftTimer);storeDraft(document);editUI();const sentSequence=sequence;const sentScene=scene,sentEpoch=epoch;
  try {
    const result=await call('SaveDocument',{scene,epoch,revision,document});
    if(scene!==sentScene||epoch!==sentEpoch)return;
    revision=result.revision;savedSequence=sentSequence;
    if(sequence===savedSequence)clearDraft();else storeDraft();
  } catch(error) {
    storeDraft();draftProblem=true;editing=false;failure(error);
    try {if(online)await call('ReleaseEdit');}catch{/* The editing lease also expires after disconnection. */}
  } finally {
    saving=false;editUI();if(editing&&!importing&&!finishing&&sequence!==savedSequence)scheduleSave();
  }
}
function scheduleSave(){clearTimeout(saveTimer);saveTimer=setTimeout(save,800);}
function finishEdit() {
  if(finishTask)return finishTask;
  if(!editing||importing)return Promise.resolve();
  finishing=true;editUI();
  finishTask=(async()=>{
    try {
      await save();if(editing&&sequence!==savedSequence)await save();
      if(!editing||draftProblem||!online||sequence!==savedSequence)return;
      const state=await call('ReleaseEdit');editing=false;updateState(state);await load(scene);
    } finally {finishing=false;finishTask=null;editUI();}
  })();
  return finishTask;
}
function disconnected(event) {
  importController?.abort();
  if(event?.code===4011){desired=false;forgetSession();}
  if(editing&&sequence!==savedSequence){storeDraft();draftProblem=true;}
  editing=false;online=false;session='';clearInterval(heartbeat);clearTimeout(saveTimer);clearTimeout(draftTimer);stopLocal();editUI();
  $('connection').textContent=t('RemoteDisconnected');
  if(desired){clearTimeout(reconnectTimer);reconnectTimer=setTimeout(connect,reconnectDelay);reconnectDelay=Math.min(10000,reconnectDelay*2);}
}
async function onEvent(state,type='Changed') {
  if(!online)return;
  if(type==='Scroll') {
    if(state.epoch===remoteState.epoch&&state.scene===remoteState.activeScene) {
      remoteState.position=state.position;syncScroll();
    }
    return;
  }
  updateState(state);
  try {
    if(importing) {
      if(state.locked||state.localEditing||state.epoch!==epoch||!state.scenes.some(item=>item.id===scene)||(editing&&state.editorScene!==scene)) {
        importController?.abort();editing=false;editUI();
      }
      return;
    }
    if(state.locked&&editing) {
      if(sequence!==savedSequence){storeDraft();draftProblem=true;}
      editing=false;editUI();if(draftProblem)notice('RemoteLocked');return;
    }
    if(editing) {
      if(state.localEditing||state.epoch!==epoch||state.editorScene!==scene) {
        if(sequence!==savedSequence){storeDraft();draftProblem=true;notice('RemoteConflict',true);}
        editing=false;editUI();
      }
      return;
    }
    if(!state.locked&&!draftProblem&&$('message').textContent===t('RemoteLocked'))clearNotice();
    if(draftProblem)return;
    let target=$('follow').checked?state.activeScene:scene;
    if(!state.scenes.some(item=>item.id===target))target=state.activeScene||state.scenes[0]?.id;
    if(!target){scene='';model=null;$('document').replaceChildren();updateCharacters({blocks:[]});scheduleSearch();updateMetrics();editUI();return;}
    if(target!==scene||state.epoch!==epoch||state.changed?.includes(scene))await load(target);
  }catch(error){failure(error);}
}
async function connect() {
  if(!desired||connecting)return;
  connecting=true;clearTimeout(reconnectTimer);$('login').querySelector('button').disabled=true;
  $('connection').textContent=t('RemoteConnecting');
  const current=new Connection((state,type)=>{if(connection===current)onEvent(state,type);},event=>{if(connection===current)disconnected(event);});
  connection=current;
  try {
    await connection.connect(`ws://${location.hostname}:${config.port}`,password,credential);
    if(!desired){current.close();return;}
    const state=await call('Open',{name:$('device').value});
    if(!desired){current.close();return;}
    credential=current.credential;password='';$('password').value='';rememberSession();clearNotice();
    session=state.session;online=true;reconnectDelay=1000;
    $('connection').textContent=t('RemoteConnected');$('login').hidden=true;$('workspace').hidden=false;$('disconnect').hidden=false;
    updateState(state);
    await load(scene&&state.scenes.some(item=>item.id===scene)?scene:state.activeScene||state.scenes[0]?.id);
    heartbeat=setInterval(async()=>{
      try{const result=await call('KeepAlive');if(editing&&(!result.editing||result.epoch!==epoch))throw Object.assign(new Error('conflict'),{code:result.locked?'locked':'conflict'});}
      catch(error){
        importController?.abort();
        if(editing){if(sequence!==savedSequence){storeDraft();draftProblem=true;}editing=false;editUI();failure(error);}
        // Background tabs can outlive their server session; reconnect to open a new one.
        if(['session','timeout','disconnected'].includes(error.code||error.message))current.close();
      }
    },5000);
  }catch(error){
    if(['authentication','password-required','access'].includes(error.code||error.message)) {
      desired=false;clearTimeout(reconnectTimer);forgetSession();$('login').hidden=false;$('workspace').hidden=true;$('disconnect').hidden=true;
    }
    failure(error);current.close();
  }
  finally{connecting=false;$('login').querySelector('button').disabled=false;if(!$('login').hidden)$('password').focus({preventScroll:true});}
}
$('login').addEventListener('submit',event=>{event.preventDefault();password=$('password').value;desired=true;connect();});
$('remember').onchange=rememberSession;
$('disconnect').onclick=async()=>{
  if(editing){await finishEdit();if(editing||draftProblem)return;}
  desired=false;clearTimeout(reconnectTimer);forgetSession();connection.close();
  $('login').hidden=false;$('workspace').hidden=true;$('disconnect').hidden=true;
  $('password').focus({preventScroll:true});
};
$('document').onfocus=()=>{if(!remoteState.editor)beginEdit().catch(failure);};
$('document').onclick=async event=>{
  if(event.altKey&&event.shiftKey)return;
  try{if(!remoteState.editor)await beginEdit();}
  catch(error){failure(error);}
};
// Formatting controls stay in the editing session; other clicks release it.
document.addEventListener('click',event=>{
  if(editing&&!event.target.closest('#document,#text-tools,#paragraph'))finishEdit().catch(failure);
});
document.addEventListener('focusin',event=>{
  if(editing&&!event.target.closest('#document,#text-tools,#paragraph'))finishEdit().catch(failure);
});
$('scenes').onchange=async()=>{const target=$('scenes').value;try{await finishEdit();if(draftProblem)return;$('follow').checked=false;await load(target);}catch(error){failure(error);}};
$('follow').onchange=async()=>{try{await finishEdit();if(!draftProblem&&$('follow').checked)await load(remoteState.activeScene);}catch(error){failure(error);}};
let composing;
$('document').addEventListener('compositionstart',()=>{composing=serialize($('document'));clearTimeout(saveTimer);clearTimeout(draftTimer);});
$('document').addEventListener('compositionend',()=>{
  if(composing&&!withinTextLimits(serialize($('document')))){render($('document'),composing);notice('TextLimit');}
  composing=null;$('document').dispatchEvent(new Event('input',{bubbles:true}));
});
$('document').addEventListener('input',event=>{
  cancelPainter();
  if(!canWrite()||event.isComposing||composing)return;
  const wasSaved=sequence===savedSequence;++sequence;
  clearTimeout(draftTimer);draftTimer=setTimeout(storeDraft,250);scheduleSave();scheduleMetrics();scheduleSearch();
  if(wasSaved)editUI();
});
const insertCheckedText=text=>{try{insertText($('document'),text);}catch(error){failure(error);}};
$('document').addEventListener('paste',event=>{if(canWrite()){event.preventDefault();const content=event.clipboardData.getData('text/plain');if(content)insertCheckedText(content);}});
$('document').addEventListener('drop',event=>event.preventDefault());
$('document').addEventListener('beforeinput',event=>{
  if(!canWrite()){event.preventDefault();return;}
  if(event.inputType.startsWith('insert'))leaveMarkBoundary();
  const target=event.getTargetRanges?.()[0];
  let range;
  if(target){range=document.createRange();range.setStart(target.startContainer,target.startOffset);range.setEnd(target.endContainer,target.endOffset);}
  else if(document.getSelection().rangeCount)range=document.getSelection().getRangeAt(0).cloneRange();
  if(!target&&range?.collapsed&&event.inputType.startsWith('delete')) {
    const p=(range.startContainer.nodeType===1?range.startContainer:range.startContainer.parentElement).closest('p');
    if(p){const part=range.cloneRange();part.selectNodeContents(p);part.setEnd(range.startContainer,range.startOffset);
      if(event.inputType.endsWith('Backward')&&!part.toString()&&p.previousElementSibling)range.setStart(p.previousElementSibling,p.previousElementSibling.childNodes.length);
      part.selectNodeContents(p);part.setStart(range.endContainer,range.endOffset);
      if(event.inputType.endsWith('Forward')&&!part.toString()&&p.nextElementSibling)range.setEnd(p.nextElementSibling,0);
    }
  }
  const inserted=event.inputType==='insertParagraph'?'\n':event.inputType==='insertLineBreak'?'\u2028':event.data||'';
  if(range&&!acceptsEdit($('document'),inserted,range)){if(event.cancelable)event.preventDefault();notice('TextLimit');return;}
  if(event.inputType==='insertParagraph') {
    const selection=document.getSelection();if(!selection.rangeCount)return;
    const range=selection.getRangeAt(0);
    const p=(range.startContainer.nodeType===1?range.startContainer:range.startContainer.parentElement).closest('p');
    if(!p||!$('document').contains(p))return;
    event.preventDefault();range.deleteContents();
    const tail=range.cloneRange();tail.setEnd(p,p.childNodes.length);const next=p.cloneNode(false);next.append(tail.extractContents());
    if(!next.textContent)next.replaceChildren(document.createElement('br'));if(!p.textContent)p.replaceChildren(document.createElement('br'));
    p.after(next);range.setStart(next,0);range.collapse(true);selection.removeAllRanges();selection.addRange(range);$('document').dispatchEvent(new Event('input'));
  }
});
const formatButtons=document.querySelectorAll('[data-format]');
formatButtons.forEach(button=>{
  button.onpointerdown=event=>event.preventDefault();
  button.onclick=()=>{if(!canWrite())return;const active=button.getAttribute('aria-pressed')!=='true';
    cancelPainter();if(formatSelection($('document'),button.dataset.format,button.dataset.format==='weight'?(active?700:400):active))button.setAttribute('aria-pressed',String(active));};
});
let selectionRange;
function leaveMarkBoundary(){
  const selection=document.getSelection(),root=$('document');
  if(!selection.rangeCount||!root.contains(selection.anchorNode))return;
  const range=selection.getRangeAt(0);if(!range.collapsed)return;
  const element=range.startContainer.nodeType===1?range.startContainer:range.startContainer.parentElement;
  let mark=element.closest('[data-mark]'),start=false,end=false;
  if(mark){
    let node=range.startContainer;start=range.startOffset===0;end=range.startOffset===(node.nodeType===3?node.length:node.childNodes.length);
    while(node!==mark){start&&=!node.previousSibling;end&&=!node.nextSibling;node=node.parentNode;}
    if(!start&&!end)return;
    // Leave the span before the browser inserts another character.
    if(end)range.setStartAfter(mark);else range.setStartBefore(mark);
    range.collapse(true);selection.removeAllRanges();selection.addRange(range);
  }else{
    const neighbor=before=>{
      let node=range.startContainer;
      if(node.nodeType===1){const child=node.childNodes[range.startOffset-(before?1:0)];if(child)return child;}
      else if(range.startOffset!==(before?0:node.length))return null;
      while(node!==root){const sibling=before?node.previousSibling:node.nextSibling;if(sibling)return sibling;node=node.parentNode;}
      return null;
    };
    const previous=neighbor(true),next=neighbor(false);
    if(previous?.nodeType===1&&previous.hasAttribute('data-mark')){mark=previous;end=true;}
    else if(next?.nodeType===1&&next.hasAttribute('data-mark')){mark=next;start=true;}
    else return;
  }
  // A caret outside a mark can retain its previous typing color.
  const adjacent=end?mark.nextSibling:mark.previousSibling;
  const color=getComputedStyle(adjacent?.nodeType===1?adjacent:mark.parentElement).color;
  if(document.queryCommandValue('foreColor')!==color)document.execCommand('foreColor',false,color);
}
document.addEventListener('selectionchange',()=>{
  updateCharacterStatus();
  const selection=document.getSelection();if(!selection.rangeCount||!$('document').contains(selection.anchorNode))return;
  leaveMarkBoundary();
  const match=searchRanges[searchIndex],range=selection.getRangeAt(0);
  if(match&&(range.startContainer!==match.startContainer||range.startOffset!==match.startOffset||range.endContainer!==match.endContainer||range.endOffset!==match.endOffset))setSearchHighlight(null);
  selectionRange=selection.getRangeAt(0).cloneRange();
  const format=selectionFormat($('document'))||{};
  for(const button of formatButtons)button.setAttribute('aria-pressed',String(button.dataset.format==='weight'?format.weight>=700:!!format[button.dataset.format]));
  const node=selection.anchorNode.nodeType===1?selection.anchorNode:selection.anchorNode.parentElement;
  const paragraph=node.closest('p');$('list').value=paragraph?.dataset.list||'0';
});
function restoreSelection(){if(selectionRange){const selection=document.getSelection();selection.removeAllRanges();selection.addRange(selectionRange);}}
document.addEventListener('keydown',event=>{
  if(!editing&&!event.altKey&&!event.ctrlKey&&!event.metaKey&&!event.shiftKey&&['ArrowUp','ArrowDown'].includes(event.key)&&$('document').contains(document.activeElement)){
    event.preventDefault();smoothScrollBy((event.key==='ArrowUp'?-1:1)*parseFloat(getComputedStyle($('document')).lineHeight));return;
  }
  if(event.key==='Backspace'&&!event.altKey&&!event.ctrlKey&&!event.metaKey&&!event.shiftKey&&canWrite()){
    const selection=document.getSelection(),range=selection.rangeCount&&selection.getRangeAt(0);
    const mark=range?.collapsed&&markAt($('document'),range);
    if(mark?.atEnd){event.preventDefault();try{insertMark($('document'),'',mark.range);}catch(error){failure(error);}return;}
  }
  if(event.key!=='Escape')return;
  if(paintFormat){event.preventDefault();cancelPainter();return;}
  if(document.activeElement===$('characters')){event.preventDefault();$('characters').blur();return;}
  if(editing||$('document').contains(document.activeElement)) {
    event.preventDefault();
    finishEdit().then(()=>{if(!editing&&!draftProblem)$('document').blur();}).catch(failure);
  }
});
function applyTextFormat(key,value){if(!canWrite())return;cancelPainter();restoreSelection();if(!formatSelection($('document'),key,value))notice('RemoteSelectText');}
$('size').onchange=()=>{const size=Number($('size').value);if(size>=6&&size<=144)applyTextFormat('size',size);};
$('family').onchange=()=>applyTextFormat('family',$('family').value);
$('color').onchange=()=>applyTextFormat('color',$('color').value);
$('highlight').onchange=()=>applyTextFormat('background',$('highlight').value);
$('clear-format').onpointerdown=event=>event.preventDefault();
$('clear-format').onclick=()=>applyTextFormat('clear',true);
function cancelPainter(){
  if(!paintFormat)return;
  paintFormat=null;paintSource=null;paintContinuous=false;
  const button=$('format-painter');button.setAttribute('aria-pressed','false');button.title=t('FormatPainter');button.setAttribute('aria-label',button.title);
  $('document').classList.remove('painting');
}
function startPainter(continuous=false,format=null){
  if(!canWrite())return;
  if(!format)restoreSelection();paintFormat=format||selectionFormat($('document'));if(!paintFormat)return;
  paintContinuous=continuous;paintSource=document.getSelection().getRangeAt(0).cloneRange();
  const button=$('format-painter');button.setAttribute('aria-pressed','true');button.title=t(continuous?'FormatPainterContinuous':'FormatPainter');button.setAttribute('aria-label',button.title);
  $('document').classList.add('painting');$('document').focus({preventScroll:true});
}
$('format-painter').onpointerdown=event=>event.preventDefault();
$('format-painter').onclick=event=>{if(event.detail>1)return;if(paintFormat)cancelPainter();else startPainter();};
$('format-painter').ondblclick=event=>{event.preventDefault();startPainter(true,paintFormat);};
function paintSelection(){
  const selection=document.getSelection(),range=selection.rangeCount&&selection.getRangeAt(0);
  if(!paintFormat||!canWrite()||composing||!range||range.collapsed||!$('document').contains(range.commonAncestorContainer))return;
  if(paintSource&&range.startContainer===paintSource.startContainer&&range.startOffset===paintSource.startOffset&&range.endContainer===paintSource.endContainer&&range.endOffset===paintSource.endOffset)return;
  const format=paintFormat,continuous=paintContinuous;cancelPainter();
  if(formatSelection($('document'),'paint',format)&&continuous)startPainter(true,format);
}
$('document').addEventListener('pointerup',()=>queueMicrotask(paintSelection));
$('document').addEventListener('keyup',event=>{if(['Shift','ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Home','End'].includes(event.key))paintSelection();});
$('insert-mark').onpointerdown=event=>event.preventDefault();
const insertCheckedMark=()=>{try{insertMark($('document'));}catch(error){failure(error);}};
$('insert-mark').onclick=()=>{
  if(!canWrite())return;restoreSelection();insertCheckedMark();
};
function caretAtPoint(event){
  const point=document.caretPositionFromPoint?.(event.clientX,event.clientY);
  const range=point?document.createRange():document.caretRangeFromPoint?.(event.clientX,event.clientY);
  if(point){range.setStart(point.offsetNode,point.offset);range.collapse(true);}
  return range;
}
$('document').addEventListener('mousedown',event=>{
  if(event.button!==0||!event.altKey||!event.shiftKey||event.ctrlKey||event.metaKey||!canWrite())return;
  const range=caretAtPoint(event);
  if(!range||!$('document').contains(range.startContainer))return;
  event.preventDefault();const selection=document.getSelection();selection.removeAllRanges();selection.addRange(range);
  insertCheckedMark();
});
function applyParagraph(key,value){if(!canWrite())return;restoreSelection();formatParagraphs($('document'),key,value);}
$('list').onchange=()=>applyParagraph('list',Number($('list').value));
for(const [id,delta] of [['indent-less',-1],['indent-more',1]]) {
  $(id).onpointerdown=event=>event.preventDefault();$(id).onclick=()=>applyParagraph('indent',delta);
}
document.querySelectorAll('[data-align]').forEach(button=>{
  button.onpointerdown=event=>event.preventDefault();button.onclick=()=>applyParagraph('align',Number(button.dataset.align));
});
$('insert-time').onpointerdown=event=>event.preventDefault();
$('insert-time').onclick=()=>{
  if(!canWrite())return;restoreSelection();const date=new Date(),pad=value=>String(value).padStart(2,'0');
  insertCheckedText(`${date.getFullYear()}-${pad(date.getMonth()+1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())} `);
};
$('import').onclick=()=>{if(!$('import').disabled)$('import-file').click();};
$('import-file').onchange=async()=>{
  const file=$('import-file').files[0];$('import-file').value='';if(!file||$('import').disabled)return;
  const importScene=scene,importEpoch=epoch,wasEditing=editing;
  let frozen=false;
  try {
    const word=/\.docx$/i.test(file.name),html=/\.html?$/i.test(file.name);
    if(!word&&!html&&!/\.txt$/i.test(file.name)){notice('RemoteFileType');return;}
    if(file.size>(word?5:1)*1024*1024){notice(word?'RemoteWordSize':'RemoteFileSize');return;}
    const data=word?null:html?importHtml(await file.text(),document):plainDocument(await file.text());
    if(data&&new TextEncoder().encode(JSON.stringify(data)).length>4*1024*1024)throw new Error('size');
    if(!online||scene!==importScene||epoch!==importEpoch||$('import').disabled)return;
    if(!confirm(t('ReplacePrompt')+(word?'\n'+t('RemoteWordNotice'):html?'\n'+t('RemoteHtmlNotice'):'')))return;
    importing=true;frozen=true;stopLocal();editUI();clearTimeout(saveTimer);
    await save();if(editing&&sequence!==savedSequence)await save();
    if(!online||draftProblem||scene!==importScene||epoch!==importEpoch)throw new Error('conflict');
    if(!editing){await call('AcquireEdit',{scene,epoch,revision,replace:true});editing=true;}
    let result;
    if(word) {
      const controller=new AbortController();importController=controller;
      const timer=setTimeout(()=>controller.abort(),35000);
      try {
        const response=await fetch('/import.docx',{method:'POST',body:file,signal:controller.signal,headers:{
          'Content-Type':'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
          'X-ScenePad-Access':access,'X-ScenePad-Session':session,'X-ScenePad-Scene':scene,
          'X-ScenePad-Epoch':epoch,'X-ScenePad-Revision':String(revision)}});
        if(!response.ok)throw new Error('upload');
        result=await response.json();if(!result.ok)throw Object.assign(new Error(result.code),{code:result.code});
      } catch(error){if(error.name==='AbortError')throw new Error(remoteState.locked?'locked':'upload');if(error instanceof TypeError)throw new Error('upload');throw error;}
      finally{clearTimeout(timer);importController=null;}
    } else result=await call('ReplaceDocument',{scene,epoch,revision,document:data});
    if(!online||scene!==importScene||epoch!==importEpoch)throw new Error('conflict');
    revision=result.revision;model=result;clearDraft();sequence=savedSequence=0;selectionRange=null;
    render($('document'),result);$('document').scrollTop=0;updateMetrics();scheduleSearch();clearNotice();
    updateCharacters(result);
    if(!wasEditing||!result.editable){editing=false;await call('ReleaseEdit');}
    if(result.simplified)notice('WordSimplified');
  } catch(error){failure(error);}
  finally {
    if(frozen) {
      if(!wasEditing&&editing){try{if(online)await call('ReleaseEdit');}catch{/* The lease expires if the connection is lost. */}editing=false;}
      importing=false;editUI();
    }
  }
};
$('export').onchange=async()=>{
  const format=$('export').value;$('export').selectedIndex=-1;if(!format||$('export').disabled)return;
  try{if(editing)await save();if(draftProblem||!online||importing)return;const plain=format==='txt',data=await call(plain?'ExportText':'ExportHtml',{scene});
    downloadFile(plain?data.text:data.html,plain?'ScenePad.txt':'ScenePad.html',plain?'text/plain':'text/html');}
  catch(error){failure(error);}
};
function updateCharacters(data){
  // Reuse loaded models and debounced draft snapshots, never scan on scroll.
  usedCharacters=characterCount(data);updateCharacterStatus();
}
function updateCharacterStatus(){
  const selection=document.getSelection(),range=selection.rangeCount&&selection.getRangeAt(0);
  const selected=range&&!range.collapsed&&$('document').contains(range.commonAncestorContainer);
  const count=selected?range.toString().length:usedCharacters;
  const value=t(selected?'SelectedCharacters':'CharacterCount').replace('%1',count.toLocaleString());
  const details=t('CharacterRemaining').replace('%1',usedCharacters.toLocaleString()).replace('%2',maxNoteCharacters.toLocaleString()).replace('%3',Math.max(0,maxNoteCharacters-usedCharacters).toLocaleString());
  $('characters').textContent=value;$('characters').title=details;$('characters').setAttribute('aria-label',selected?value+'; '+details:details);
}
function hideGoTo(restoreFocus=false){
  const current=goToState;goToState=null;$('go-to').hidden=true;
  if(restoreFocus&&current)$(current.percentage?'percent':'lines').focus({preventScroll:true});
}
function showGoTo(percentage=false){
  if(!model||importing)return;
  goToState={scene,epoch,percentage};const input=$('go-to');
  input.dataset.percentage=String(percentage);input.removeAttribute('aria-invalid');input.title=t('GoToPrompt');
  input.value=percentage?$('percent').textContent:'1';input.hidden=false;input.focus({preventScroll:true});
  input.select();
}
async function goToPosition(){
  const current=goToState;if(!current)return;const input=$('go-to'),root=$('document');
  const target=input.value.trim();
  const match=/^(\d+)(%)?$/.exec(target),value=match?Number(match[1]):-1,percent=!!match?.[2];
  if(!Number.isSafeInteger(value)||value<(percent?0:1)||value>(percent?100:root.children.length)){
    input.setAttribute('aria-invalid','true');input.title=t('GoToInvalid').replace('%1',root.children.length);input.select();return;
  }
  try{
    await finishEdit();if(goToState!==current)return;
    if(draftProblem||editing||scene!==current.scene||epoch!==current.epoch){hideGoTo();return;}
    if($('target').value==='obs'&&remoteState.activeScene===scene)await call('Scroll',{command:'seek',scene,target}).then(updateState);
    else{
      stopLocal();
      // Index existing paragraphs; percentage jumps only read the scroll range.
      if(percent)root.scrollTop=(root.scrollHeight-root.clientHeight)*value/100;
      else root.scrollTop+=root.children[value-1].getBoundingClientRect().top-root.getBoundingClientRect().top-root.clientHeight*.382;
      updateMetrics();
    }
    if(goToState===current)hideGoTo(true);
  }catch(error){failure(error);}
}
for(const id of ['lines','percent'])$(id).onclick=()=>showGoTo(id==='percent');
$('go-to').onblur=()=>hideGoTo();
$('go-to').oninput=()=>$('go-to').removeAttribute('aria-invalid');
$('go-to').onkeydown=async event=>{
  if(event.key==='Escape'){event.preventDefault();hideGoTo(true);}
  if(event.key==='Enter'){event.preventDefault();await goToPosition();}
};
let metricsFrame=0;
function scheduleMetrics(){
  if(!metricsFrame)metricsFrame=requestAnimationFrame(()=>{metricsFrame=0;updateMetrics();});
}
function updateMetrics(){
  const root=$('document'),lines=t('RemoteLines').replace('%1',String(root.children.length)),percent=`${Math.round(root.scrollTop/Math.max(1,root.scrollHeight-root.clientHeight)*100)}%`;
  if($('lines').textContent!==lines)$('lines').textContent=lines;
  if($('percent').textContent!==percent)$('percent').textContent=percent;
}
$('document').onscroll=updateMetrics;
new ResizeObserver(()=>{syncScroll();updateMetrics();}).observe($('document'));
// Measure only the toolbar when its available space or labels change.
const footer=document.querySelector('footer'),paragraphGroups=[...$('paragraph').children];
const readingButtons=footer.querySelector('.reading-buttons'),readingMetrics=footer.querySelector('.reading-metrics'),readingTarget=footer.querySelector('.reading-target'),readingSettings=footer.querySelector('.reading-settings'),speedValue=footer.querySelector('.speed-value');
function fitFooter(){
  const style=getComputedStyle(footer),gap=parseFloat(style.columnGap),available=footer.clientWidth-parseFloat(style.paddingLeft)-parseFloat(style.paddingRight);
  if(available<=0)return;
  const buttons=readingButtons.offsetWidth,metrics=readingMetrics.offsetWidth,settingsStyle=getComputedStyle(readingSettings);
  const speed=speedValue.offsetWidth+parseFloat(settingsStyle.paddingLeft)+parseFloat(settingsStyle.paddingRight);
  const paragraph=paragraphGroups.reduce((width,group)=>width+group.offsetWidth,0)+parseFloat(getComputedStyle($('paragraph')).columnGap)*2+10;
  const minimum=buttons+paragraph+readingTarget.offsetWidth+speed+metrics+gap*4;
  const stacked=available<Math.ceil(minimum),readingMinimum=stacked?buttons+speed+metrics+gap*2:minimum;
  footer.classList.toggle('two-rows',stacked);
  footer.classList.toggle('slider-hidden',available<Math.ceil(readingMinimum+24+parseFloat(settingsStyle.columnGap)));
}
let footerFrame=0;
const footerObserver=new ResizeObserver(()=>{
  if(!footerFrame)footerFrame=requestAnimationFrame(()=>{footerFrame=0;fitFooter();});
});
for(const node of [footer,...paragraphGroups,readingButtons,readingMetrics,readingTarget])footerObserver.observe(node);
function stopLocal(){playing=false;smoothTarget=null;cancelAnimationFrame(frame);frame=0;clearInterval(countdownTimer);countdownTimer=0;$('band').hidden=true;if($('target').value==='local')playState();}
function smoothScrollBy(delta){
  const root=$('document');
  if(smoothTarget===null)scrollPosition=root.scrollTop;
  smoothTarget=Math.max(0,Math.min(root.scrollHeight-root.clientHeight,(smoothTarget??root.scrollTop)+delta));
  if(!frame){lastFrame=performance.now();frame=requestAnimationFrame(tick);}
}
$('document').addEventListener('wheel',event=>{
  if(event.ctrlKey||event.metaKey||event.altKey||event.shiftKey||!event.deltaY)return;
  event.preventDefault();const root=$('document');
  smoothScrollBy(event.deltaY*(event.deltaMode===1?parseFloat(getComputedStyle(root).lineHeight):event.deltaMode===2?root.clientHeight:1));
},{passive:false});
function tick(time){
  frame=0;if(!playing&&smoothTarget===null)return;
  const root=$('document'),elapsed=Math.max(0,time-lastFrame);lastFrame=time;
  if(smoothTarget!==null){
    scrollPosition+=(smoothTarget-scrollPosition)*(1-Math.exp(-elapsed/35));
    if(Math.abs(smoothTarget-scrollPosition)<.5){scrollPosition=smoothTarget;smoothTarget=null;}
    root.scrollTop=scrollPosition;expectedScrollTop=root.scrollTop;manualUntil=0;
    if(playing||smoothTarget!==null)frame=requestAnimationFrame(tick);return;
  }
  // Manual scrolling rebases playback; only the new position is read.
  if(root.scrollTop!==expectedScrollTop){scrollPosition=expectedScrollTop=root.scrollTop;manualUntil=time+1000;}
  if(time>=manualUntil&&!scrollDragging){
    scrollPosition+=elapsed*.01*Number($('speed').value);root.scrollTop=scrollPosition;expectedScrollTop=root.scrollTop;
    if(scrollPosition>=root.scrollHeight-root.clientHeight){stopLocal();return;}
  }
  if(playing)frame=requestAnimationFrame(tick);
}
function updateSpeed(){$('multiplier').value=Number($('speed').value).toFixed(1);}
$('speed').oninput=()=>{updateSpeed();if($('target').value==='obs')sendScroll('speed',{speed:Number($('speed').value)});else localSpeed=Number($('speed').value);};
$('multiplier').onchange=()=>{
  const speed=$('multiplier').valueAsNumber;
  if(!Number.isFinite(speed)){updateSpeed();return;}
  $('speed').value=String(Math.max(.1,Math.min(5,speed)));$('speed').dispatchEvent(new Event('input'));
};
$('multiplier').onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();$('multiplier').blur();}};
const sendScroll=(command,data={})=>call('Scroll',{command,...data}).then(updateState).catch(failure);
$('play').onclick=async()=>{
  if(editing){try{await finishEdit();}catch(error){failure(error);return;}if(editing||draftProblem)return;}
  if($('target').value==='obs'){sendScroll(remoteState.scrolling?'pause':'start');return;}
  if(playing||countdownTimer){stopLocal();return;}if(editing)return;
  const start=()=>{cancelAnimationFrame(frame);smoothTarget=null;playing=true;expectedScrollTop=scrollPosition=$('document').scrollTop;manualUntil=0;scrollDragging=false;lastFrame=performance.now();playState(true);$('band').hidden=$('guide').getAttribute('aria-pressed')!=='true';frame=requestAnimationFrame(tick);};
  if(!$('countdown').checked){start();return;}
  let remaining=3;playState(false,remaining);
  countdownTimer=setInterval(()=>{if(--remaining===0){clearInterval(countdownTimer);countdownTimer=0;start();}else playState(false,remaining);},1000);
};
$('top').onclick=()=>{if($('target').value==='obs')sendScroll('top');else{stopLocal();$('document').scrollTop=0;}};
$('target').onclick=()=>{
  stopLocal();const obs=$('target').value==='local';
  $('target').value=obs?'obs':'local';$('target').textContent=t(obs?'RemoteOBS':'RemoteLocal');
  $('target').setAttribute('aria-pressed',String(obs));
  if(obs)updateState(remoteState);else{$('speed').value=localSpeed;playState();updateSpeed();}editUI();
};
$('guide').onclick=()=>{const enabled=$('guide').getAttribute('aria-pressed')!=='true';$('guide').setAttribute('aria-pressed',String(enabled));$('band').hidden=!playing||!enabled;};
$('countdown').onchange=()=>{if(countdownTimer)stopLocal();};
$('document').addEventListener('pointerdown',()=>{smoothTarget=null;if(!playing){cancelAnimationFrame(frame);frame=0;}if(playing)scrollDragging=true;},{passive:true});
for(const event of ['pointerup','pointercancel','blur'])window.addEventListener(event,()=>{if(scrollDragging){scrollDragging=false;manualUntil=performance.now()+1000;}},{passive:true});
document.addEventListener('visibilitychange',()=>{if(document.hidden){stopLocal();finishEdit().catch(failure);}else syncScroll();});
function scheduleSearch(fromStart=false){
  // Document changes end automatic mark search; manual queries stay active.
  if(markSearch){markSearch=false;$('search').value='';}
  setSearchHighlight(null);
  clearTimeout(searchTimer);searchTimer=0;searchRanges=[];searchIndex=-1;
  $('clear-search').hidden=!$('search').value;$('matches').textContent='';$('matches').removeAttribute('aria-label');$('matches').title='';
  $('previous').disabled=$('next').disabled=true;
  if($('search').value)searchTimer=setTimeout(()=>search(fromStart),180);
}
function updateSearchStatus(){
  const count=$('search').value?`${searchIndex+1}/${searchRanges.length}`:'';
  const description=count&&!searchRanges.length?t('NoMatch'):count;
  $('matches').textContent=count;$('matches').title=description;$('matches').setAttribute('aria-label',description);
  $('previous').disabled=$('next').disabled=!searchRanges.length;
}
function search(fromStart=false){
  clearTimeout(searchTimer);searchTimer=0;
  searchRanges=[];searchIndex=-1;const query=$('search').value;
  if(query){const root=$('document');const iterator=document.createTreeWalker(root,4);const nodes=[];let text='';
    let paragraph;
    while(iterator.nextNode()){
      const node=iterator.currentNode;const next=node.parentElement.closest('p');
      if(paragraph&&paragraph!==next)text+='\n';paragraph=next;
      nodes.push({node,start:text.length});text+=node.textContent;
    }
    const nodeAt=position=>{let low=0,high=nodes.length;while(low<high){const middle=(low+high)>>>1;if(nodes[middle].start<=position)low=middle+1;else high=middle;}return nodes[low-1];};
    const escaped=query.replace(/[.*+?^${}()|[\]\\]/g,'\\$&');const expression=new RegExp(escaped,'giu');let match;
    while(searchRanges.length<10000&&(match=expression.exec(text))!==null){
      const start=match.index,end=start+match[0].length;const first=nodeAt(start),last=nodeAt(end-1);
      if(first&&last){const range=document.createRange();range.setStart(first.node,start-first.start);range.setEnd(last.node,end-last.start);searchRanges.push(range);}
    }
  }
  if(fromStart&&searchRanges.length)find(1);else updateSearchStatus();
}
function setSearchHighlight(range){
  $('document').classList.toggle('searching',!!range);
  if(globalThis.Highlight&&globalThis.CSS?.highlights){CSS.highlights.delete('scene-search');if(range)CSS.highlights.set('scene-search',new Highlight(range));}
}
function find(direction,fromCursor=false){
  if(!searchRanges.length)return;const selection=document.getSelection(),root=$('document');
  if(fromCursor){
    const current=selection.rangeCount&&root.contains(selection.getRangeAt(0).commonAncestorContainer)?selection.getRangeAt(0):selectionRange;
    if(current&&root.contains(current.commonAncestorContainer)){
      const caret=current.cloneRange();caret.collapse(false);let low=0,high=searchRanges.length;
      while(low<high){const middle=(low+high)>>>1;if(searchRanges[middle].compareBoundaryPoints(Range.START_TO_START,caret)<0)low=middle+1;else high=middle;}
      searchIndex=(low===searchRanges.length?0:low)-1;
    }else searchIndex=-1;
  }
  searchIndex=searchIndex<0?(direction<0?searchRanges.length-1:0):(searchIndex+direction+searchRanges.length)%searchRanges.length;
  stopLocal();
  const range=searchRanges[searchIndex];setSearchHighlight(range);selection.removeAllRanges();selection.addRange(range);
  const bounds=range.getBoundingClientRect(),viewport=root.getBoundingClientRect();
  root.scrollTop+=bounds.top-viewport.top-(root.clientHeight-bounds.height)/2;updateSearchStatus();
}
$('next-mark').onpointerdown=event=>event.preventDefault();
$('next-mark').onclick=()=>{
  if($('next-mark').disabled)return;
  if($('search').value!=='◆'){$('search').value='◆';scheduleSearch();}
  markSearch=true;if(searchTimer)search();find(1,true);
};
$('clear-search').onclick=()=>{$('search').value='';scheduleSearch();$('search').focus();};
$('search').oninput=()=>{markSearch=false;scheduleSearch(true);};$('previous').onclick=()=>find(-1);$('next').onclick=()=>find(1);
$('search').onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();find(1);}};
$('retry').onclick=()=>{if(draftProblem)beginEdit(true).catch(failure);else if(!online){desired=true;clearTimeout(reconnectTimer);connect();}else if(!model&&scene)load(scene).catch(failure);else clearNotice();};
$('reload').onclick=()=>{
  if(!confirm(t('RemoteReload')))return;
  clearDraft();draftProblem=false;editing=false;load(scene,{restore:false}).catch(failure);
};
function downloadFile(content,name,type='text/html'){
  const blob=new Blob([content],{type:type+';charset=utf-8'});
  const link=document.createElement('a');link.href=URL.createObjectURL(blob);link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(link.href),1000);
}
$('download').onclick=()=>{
  const draft=readDraft(scene),container=document.createElement('div');
  render(container,draft&&!withinTextLimits(draft.document)?draft.document:serialize($('document')));
  downloadFile('<!doctype html><meta charset="utf-8"><body>'+container.innerHTML+'</body>','ScenePad-draft.html');
};
window.addEventListener('beforeunload',event=>{if((editing&&sequence!==savedSequence)||draftProblem){storeDraft();event.preventDefault();event.returnValue='';}});
$('connection').textContent=t('RemoteDisconnected');playState();editUI();
if(!/^[A-Za-z0-9_-]{22}$/.test(access)){$('login').querySelector('button').disabled=true;notice('RemoteFailure');}
else {
  // Keep derived credentials only for this tab and this connection link.
  try {
    const stored=JSON.parse(sessionStorage.getItem(sessionKey));
    if(typeof stored?.credential?.salt==='string'&&stored.credential.salt.length<=256&&
        /^[A-Za-z0-9+/]{43}=$/.test(stored.credential.secret)) {
      credential=stored.credential;$('remember').checked=true;
      if(stored.name&&stored.name!=='Browser')$('device').value=String(stored.name).slice(0,32);
      desired=true;connect();
    }
  }catch{/* Storage can be disabled by the browser. */}
}
