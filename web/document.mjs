const defaults={weight:400,italic:false,underline:false,strike:false,size:0,family:'',color:'',background:''};
const markSource='◆(?:\\[[^◆\\]\\r\\n\\u2028\\u2029]*\\])?';
const markPattern=new RegExp(markSource,'g'),singleMark=new RegExp('^'+markSource);
function styleMark(span){
  span.dataset.mark='';span.style.whiteSpace='nowrap';
  span.style.color='#ffb347';span.style.setProperty('--text-color','#ffb347');
}
export const maxNoteCharacters=200000,maxParagraphCharacters=10000,maxNoteParagraphs=20000;
export function characterCount(data) {
  return Math.max(0,data.blocks.reduce((sum,block)=>sum+1+block.runs.reduce((length,run)=>length+run.text.length,0),-1));
}
export function withinTextLimits(data) {
  if(data.blocks.length>maxNoteParagraphs)return false;
  let total=-1;
  for(const block of data.blocks){const length=block.runs.reduce((sum,run)=>sum+run.text.length,0);total+=length+1;if(length>maxParagraphCharacters||total>maxNoteCharacters)return false;}
  return true;
}
function editRange(root,range,contents=true) {
  const paragraphs=Array.from(root.childNodes);
  if(range.collapsed&&range.startContainer===root&&paragraphs.length){
    const offset=range.startOffset;range=range.cloneRange();range.selectNodeContents(paragraphs[Math.min(offset,paragraphs.length-1)]);range.collapse(offset<paragraphs.length);
  }
  const paragraph=(node,offset,end)=>{
    if(node===root)return paragraphs[Math.max(0,Math.min(paragraphs.length-1,offset-(end?1:0)))];
    while(node.parentNode!==root)node=node.parentNode;return node;
  };
  const first=paragraph(range.startContainer,range.startOffset,false),last=paragraph(range.endContainer,range.endOffset,true);
  if(!first||!last)return {paragraphs};
  if(!contents)return {paragraphs,first,last};
  const head=range.cloneRange();head.selectNodeContents(first);
  if(range.startContainer!==root)head.setEnd(range.startContainer,range.startOffset);else head.collapse(true);
  const tail=range.cloneRange();tail.selectNodeContents(last);
  if(range.endContainer!==root)tail.setStart(range.endContainer,range.endOffset);else tail.collapse(false);
  return {paragraphs,first,last,head:head.cloneContents(),tail:tail.cloneContents()};
}
function textLength(node,empty) {
  const text=node.textContent||'';
  return text.length+((empty??!text)?0:node.querySelectorAll?.('br').length||0);
}
export function acceptsEdit(root,text,range=root.ownerDocument.getSelection()?.rangeCount?root.ownerDocument.getSelection().getRangeAt(0):null) {
  if(text.length>maxNoteCharacters||!range||!root.contains(range.commonAncestorContainer))return false;
  // Read plain text only; rich-text serialization stays on the save debounce.
  if(range.collapsed&&!/[\r\n\u2029]/.test(text)){
    const {paragraphs,first}=editRange(root,range,false);
    const length=first?textLength(first):0;
    const total=paragraphs.reduce((sum,node)=>sum+(node===first?length:textLength(node)),Math.max(0,paragraphs.length-1));
    return paragraphs.length<=maxNoteParagraphs&&total+text.length<=maxNoteCharacters&&length+text.length<=maxParagraphCharacters;
  }
  const {paragraphs,first,last,head,tail}=editRange(root,range);
  if(!first||!last)return text.length<=maxParagraphCharacters;
  const prefix=textLength(head,!first.textContent),suffix=textLength(tail,!last.textContent);
  const a=paragraphs.indexOf(first),b=paragraphs.indexOf(last);
  const removed=paragraphs.slice(a,b+1).reduce((sum,node)=>sum+textLength(node),b-a)-prefix-suffix;
  const total=paragraphs.reduce((sum,node)=>sum+textLength(node),Math.max(0,paragraphs.length-1));
  const parts=text.split(/\r\n?|\n|\u2029/).map(part=>part.length);parts[0]+=prefix;parts[parts.length-1]+=suffix;
  return paragraphs.length-(b-a)+parts.length-1<=maxNoteParagraphs&&total-removed+text.length<=maxNoteCharacters&&parts.every(length=>length<=maxParagraphCharacters);
}
const aligns={1:'left',2:'right',4:'center',8:'justify'};
const blockFormat=node=>({align:Number(node.dataset.align)||1,indent:Number(node.dataset.indent)||0,
  list:Number(node.dataset.list)||0,listIndent:Number(node.dataset.listIndent)||1,listGroup:Number(node.dataset.listGroup)||0,listStart:Number(node.dataset.listStart)||1});
function listMarker(style,number) {
  if(style>=-3)return ({'-1':'•','-2':'◦','-3':'▪'})[style];
  if(style===-5||style===-6){let text='';for(let n=number;n>0;n=Math.floor((n-1)/26))text=String.fromCharCode(97+(n-1)%26)+text;return (style===-6?text.toUpperCase():text)+'.';}
  if(style===-7||style===-8){let text='',n=number;for(const [value,symbol] of [[1000,'M'],[900,'CM'],[500,'D'],[400,'CD'],[100,'C'],[90,'XC'],[50,'L'],[40,'XL'],[10,'X'],[9,'IX'],[5,'V'],[4,'IV'],[1,'I']])while(n>=value){text+=symbol;n-=value;}return (style===-7?text.toLowerCase():text)+'.';}
  return number+'.';
}
function styleSpan(span,run) {
  span.dataset.format=JSON.stringify({...run,text:undefined});
  span.style.fontWeight=String(run.weight);span.style.fontStyle=run.italic?'italic':'normal';
  span.style.textDecoration=[run.underline?'underline':'',run.strike?'line-through':''].filter(Boolean).join(' ')||'none';
  if(run.size)span.style.fontSize=`${run.size}pt`;
  if(run.family)span.style.fontFamily=run.family;
  if(run.color){span.style.color=run.color;span.style.setProperty('--text-color',run.color);}
  if(run.background)span.style.backgroundColor=run.background;
}
export function render(root,document) {
  const fragment=root.ownerDocument.createDocumentFragment();
  let number=0,group=-1;
  for(const block of document.blocks) {
    const p=root.ownerDocument.createElement('p');
    Object.entries(block).filter(([key])=>key!=='runs').forEach(([key,value])=>{p.dataset[key]=String(value);});
    p.style.textAlign=aligns[block.align]||'left';
    p.style.marginInlineStart=`${Math.min(12,block.indent+(block.list?block.listIndent:0))*1.4}em`;
    if(block.list) {
      number=block.listGroup===group?number+1:(block.listStart||1);group=block.listGroup;
      p.dataset.marker=listMarker(block.list,number);
    }
    const text=block.runs.map(run=>run.text).join(''),marks=Array.from(text.matchAll(markPattern));
    let offset=0,index=0,group=null;
    for(const run of block.runs){
      let used=0;
      while(used<run.text.length){
        const mark=marks[index];
        if(mark&&offset===mark.index){group=root.ownerDocument.createElement('span');styleMark(group);p.append(group);}
        const boundary=group?mark.index+mark[0].length:mark?.index??text.length;
        const length=Math.min(run.text.length-used,boundary-offset),span=root.ownerDocument.createElement('span');
        styleSpan(span,group?{...run,color:'#ffb347'}:run);span.textContent=run.text.slice(used,used+length);(group||p).append(span);
        used+=length;offset+=length;
        if(group&&offset===boundary){group=null;++index;}
      }
    }
    if(!p.textContent)p.append(root.ownerDocument.createElement('br'));
    fragment.append(p);
  }
  root.replaceChildren(fragment);
}
function colorHex(value) {
  if(!value)return '';
  if(value.startsWith('#'))return value;
  const match=value.match(/^rgba?\((\d+),\s*(\d+),\s*(\d+)/);
  return match?'#'+match.slice(1,4).map(n=>Number(n).toString(16).padStart(2,'0')).join(''):(/^[a-z]{3,9}$/i.test(value)?value:'');
}
function readStyle(node,inherited,metadata=true) {
  let data={...inherited};
  if(metadata&&node.dataset?.format) {try{data={...data,...JSON.parse(node.dataset.format)};}catch{/* Keep inherited text formatting. */}}
  const style=node.style;
  if(['B','STRONG'].includes(node.tagName))data.weight=700;
  if(['I','EM'].includes(node.tagName))data.italic=true;
  if(node.tagName==='U')data.underline=true;
  if(['S','STRIKE','DEL'].includes(node.tagName))data.strike=true;
  if(style.fontWeight)data.weight=style.fontWeight==='bold'?700:Number(style.fontWeight)||400;
  if(style.fontStyle)data.italic=style.fontStyle==='italic';
  if(style.textDecoration){data.underline=style.textDecoration.includes('underline');data.strike=style.textDecoration.includes('line-through');}
  if(style.fontSize)data.size=parseFloat(style.fontSize)*(style.fontSize.endsWith('px')?.75:1);
  if(style.fontFamily)data.family=style.fontFamily.replace(/^['"]|['"]$/g,'');
  if(style.color)data.color=colorHex(style.color);
  if(style.backgroundColor)data.background=colorHex(style.backgroundColor);
  return data;
}
export function serialize(root) {
  const blocks=[];
  const append=node=>{
    const block={...blockFormat(node.nodeType===1?node:root),runs:[]};
    let hasText;
    function walk(child,format) {
      if(child.nodeType===3) {
        const text=child.textContent.replaceAll('\n','\u2028');
        if(!text)return;
        const run={...format,text};const previous=block.runs.at(-1);
        if(previous && JSON.stringify({...previous,text:''})===JSON.stringify({...run,text:''}))previous.text+=text;
        else block.runs.push(run);
      } else if(child.nodeType===1) {
        if(child.tagName==='BR') {hasText??=!!node.textContent;if(hasText)block.runs.push({...format,text:'\u2028'});return;}
        const next=readStyle(child,format);for(const part of child.childNodes)walk(part,next);
      }
    }
    walk(node,{...defaults});blocks.push(block);
  };
  for(const child of root.childNodes)append(child);
  return {blocks:blocks.length?blocks:[{align:1,indent:0,list:0,listIndent:1,listGroup:0,listStart:1,runs:[]}]};
}
function selectionState(root) {
  const selection=root.ownerDocument.getSelection();
  if(!selection.rangeCount)return null;
  const range=selection.getRangeAt(0);
  if(!root.contains(range.commonAncestorContainer))return null;
  // Cache parent text checks when mapping many soft breaks.
  const textParents=new Map();
  const length=node=>{
    if(node.nodeType===3)return node.length;
    if(node.nodeName==='BR'){
      const parent=node.parentElement;
      if(!textParents.has(parent))textParents.set(parent,!!parent.textContent);
      return textParents.get(parent)?1:0;
    }
    return Array.from(node.childNodes).reduce((sum,child)=>sum+length(child),0);
  };
  const position=(container,offset)=>{
    let total=0,result;
    function walk(node){
      if(result!==undefined)return;
      if(node===container){result=total+(node.nodeType===3?offset:Array.from(node.childNodes).slice(0,offset).reduce((sum,child)=>sum+length(child),0));return;}
      if(node.nodeType===3||node.nodeName==='BR')total+=length(node);else for(const child of node.childNodes)walk(child);
    }
    walk(root);return result??0;
  };
  return {range,start:position(range.startContainer,range.startOffset),end:position(range.endContainer,range.endOffset)};
}
function replaceContent(root,data,state) {
  const scroll=root.scrollTop;render(root,data);root.scrollTop=scroll;
  if(state){
    const {start,end}=state;const selection=root.ownerDocument.getSelection();
    const iterator=root.ownerDocument.createTreeWalker(root,4),restored=root.ownerDocument.createRange();let offset=0,anchored=false;
    while(iterator.nextNode()){
      const node=iterator.currentNode;
      if(!anchored&&start<=offset+node.length){restored.setStart(node,Math.max(0,start-offset));anchored=true;}
      if(anchored&&end<=offset+node.length){restored.setEnd(node,Math.max(0,end-offset));break;}
      offset+=node.length;
    }
    if(!anchored){restored.selectNodeContents(root);restored.collapse(true);}
    selection.removeAllRanges();selection.addRange(restored);
  }
  root.dispatchEvent(new Event('input',{bubbles:true}));
}
export function markAt(root,range) {
  if(!root.contains(range.startContainer))return null;
  const element=range.startContainer.nodeType===1?range.startContainer:range.startContainer.parentElement;
  const paragraph=element.closest('p');if(!paragraph||!root.contains(paragraph))return null;
  // Read only the clicked paragraph, including marks split across formatting runs.
  const prefix=range.cloneRange();prefix.selectNodeContents(paragraph);prefix.setEnd(range.startContainer,range.startOffset);
  const position=prefix.toString().length,text=paragraph.textContent;
  const start=text.lastIndexOf('◆',position);if(start<0)return null;
  const match=singleMark.exec(text.slice(start));
  if(position>start+match[0].length)return null;
  const found=root.ownerDocument.createRange(),walker=root.ownerDocument.createTreeWalker(paragraph,4);
  let offset=0,started=false;
  while(walker.nextNode()){
    const node=walker.currentNode;
    if(!started&&start<offset+node.length){found.setStart(node,start-offset);started=true;}
    if(started&&start+match[0].length<=offset+node.length){found.setEnd(node,start+match[0].length-offset);break;}
    offset+=node.length;
  }
  const group=found.startContainer.parentElement.closest('[data-mark]');
  if(group&&root.contains(group)&&group.textContent===match[0])found.selectNode(group);
  return {range:found,text:match[0],atEnd:position===start+match[0].length};
}
export function insertMark(root,text='◆[]',target=null) {
  const selection=root.ownerDocument.getSelection();if(!target&&!selection.rangeCount)return;
  const range=(target||selection.getRangeAt(0)).cloneRange();if(!root.contains(range.commonAncestorContainer))return;
  if(!target)range.collapse(false);
  if(!acceptsEdit(root,text,range))throw new Error('limit');
  const focused=root.ownerDocument.activeElement===root;
  if(!focused)root.focus({preventScroll:true});
  if(target||!focused||!selection.getRangeAt(0).collapsed){selection.removeAllRanges();selection.addRange(range);}
  const span=root.ownerDocument.createElement('span');
  styleMark(span);span.textContent=text;
  // Native editing commands keep mark insertion, replacement and deletion undoable.
  if(!root.ownerDocument.execCommand(text?'insertHTML':'delete',false,text?span.outerHTML:null))throw new Error('edit');
  if(!text)return null;
  const end=selection.getRangeAt(0);
  const node=end.endContainer.nodeType===1?end.endContainer.childNodes[end.endOffset-1]:end.endContainer;
  const inserted=(node?.nodeType===1?node:node?.parentElement)?.closest('span');
  // Locate the new span at the caret before searching the paragraph.
  let mark;
  if(inserted&&root.contains(inserted)&&inserted.textContent===text){
    const range=root.ownerDocument.createRange();range.selectNode(inserted);mark={range,text};
  }else mark=markAt(root,end);
  const element=mark.range.startContainer.nodeType===1?mark.range.startContainer.childNodes[mark.range.startOffset]:mark.range.startContainer.parentElement;
  if(element?.tagName==='SPAN'&&element.textContent===text){
    if(!element.hasAttribute('data-mark'))styleMark(element);
    mark.range.selectNode(element);
  }
  const caret=mark.range.cloneRange();caret.collapse(false);
  // Enter the optional note in the document, leaving search untouched.
  if(text.startsWith('◆[')&&text.endsWith(']')){
    let tail=caret.endContainer.nodeType===3?caret.endContainer:caret.endContainer.childNodes[caret.endOffset-1];
    while(tail?.lastChild)tail=tail.lastChild;
    if(tail?.nodeType===3)caret.setStart(tail,tail.length-1);
    caret.collapse(true);
  }
  selection.removeAllRanges();selection.addRange(caret);
  return mark;
}
export function selectionFormat(root) {
  const selection=root.ownerDocument.getSelection();if(!selection.rangeCount)return null;
  const range=selection.getRangeAt(0);if(!root.contains(range.commonAncestorContainer))return null;
  let node=range.startContainer;
  if(!range.collapsed&&node.nodeType===3&&range.startOffset===node.length){
    const walker=root.ownerDocument.createTreeWalker(root,4);walker.currentNode=node;
    node=walker.nextNode()||node;
  }
  if(node.nodeType===1)node=node.childNodes[Math.min(range.startOffset,node.childNodes.length-1)]||node;
  if(node.nodeType===3)node=node.parentElement;
  const parents=[];while(node&&node!==root){parents.unshift(node);node=node.parentElement;}
  return parents.reduce((format,element)=>readStyle(element,format),{...defaults});
}
export function formatSelection(root,key,value) {
  const state=selectionState(root);if(!state||state.range.collapsed)return false;
  const {start,end}=state;
  const data=serialize(root);let offset=0;
  // Flatten selected formatting so underline can also be removed from inherited spans.
  for(const block of data.blocks){
    const runs=[];
    for(const run of block.runs){
      const a=Math.max(0,start-offset),b=Math.min(run.text.length,end-offset);
      if(b>a){
        if(a)runs.push({...run,text:run.text.slice(0,a)});
        runs.push(key==='clear'?{...defaults,size:12,family:'Arial',text:run.text.slice(a,b)}:{...run,...(key==='paint'?value:{[key]:value}),text:run.text.slice(a,b)});
        if(b<run.text.length)runs.push({...run,text:run.text.slice(b)});
      }else runs.push(run);
      offset+=run.text.length;
    }
    block.runs=runs;
  }
  replaceContent(root,data,state);return true;
}
export function updateParagraphs(data,indices,key,value) {
  const group=Math.max(0,...data.blocks.map(block=>block.listGroup))+1;
  for(const index of indices){
    const block=data.blocks[index];if(!block)continue;
    if(key==='align')block.align=value;
    if(key==='list'){block.list=value;block.listGroup=value?group:0;block.listStart=1;block.listIndent=1;}
    if(key==='indent'){
      const property=block.list?'listIndent':'indent';block[property]=Math.max(block.list?1:0,Math.min(12,block[property]+value));
    }
  }
}
export function formatParagraphs(root,key,value) {
  const state=selectionState(root);if(!state)return false;
  const nodes=Array.from(root.childNodes);
  const indices=nodes.map((node,index)=>{
    if(!state.range.intersectsNode(node))return -1;
    if(!state.range.collapsed&&node.contains(state.range.endContainer)&&state.range.endOffset===0){
      let part=state.range.endContainer;while(part!==node&&!part.previousSibling)part=part.parentNode;
      if(part===node)return -1;
    }
    return index;
  }).filter(index=>index>=0);
  const data=serialize(root);updateParagraphs(data,indices,key,value);replaceContent(root,data,state);return true;
}
export function plainDocument(text) {
  text=text.replace(/^\ufeff/,'').replace(/\r\n?|\u2029/g,'\n');
  if(text.length>maxNoteCharacters)throw new Error('limit');
  const lines=text.split('\n');
  if(lines.some(line=>line.length>maxParagraphCharacters))throw new Error('limit');
  if(lines.length>maxNoteParagraphs)throw new Error('limit');
  if(/[\0\ufffc]/u.test(text))throw new Error('format');
  return {blocks:lines.map(text=>({align:1,indent:0,list:0,listIndent:1,listGroup:0,listStart:1,runs:text?[{...defaults,size:12,family:'Arial',text}]:[]}))};
}
export function importHtml(html,ownerDocument) {
  if(html.length>1024*1024)throw new Error('size');
  // Template content stays inert; only supported text and formatting reach the editor.
  const template=ownerDocument.createElement('template');template.innerHTML=html;
  const blocks=[];let current=null,keepEmpty=false,group=0,total=0,runCount=0;
  const fresh=(align=1,list=0,listGroup=0,listIndent=1,listStart=1)=>({align,indent:0,list,listGroup,listIndent,listStart,runs:[]});
  const finish=()=>{if(current&&(current.runs.length||keepEmpty))blocks.push(current);current=null;keepEmpty=false;};
  const append=(format,text)=>{
    total+=text.length;if(total>1024*1024||blocks.length>=maxNoteParagraphs||++runCount>20000)throw new Error('size');
    if(!current)current=fresh();current.runs.push({...format,text});
  };
  function walk(node,inherited,list=null,depth=0,pre=false){
    if(depth>64)throw new Error('format');
    if(node.nodeType===3){
      const text=node.textContent.replace(/\r\n?|\n/g,pre?'\u2028':' ');
      if(!current&&!text.trim())return;
      append(inherited,text);return;
    }
    if(node.nodeType!==1&&node.nodeType!==11)return;
    const tag=node.tagName;
    if(['SCRIPT','STYLE','IFRAME','OBJECT','EMBED','IMG','SVG','MATH','LINK','META','BASE','AUDIO','VIDEO','FORM','INPUT','BUTTON'].includes(tag))return;
    if(tag==='UL'||tag==='OL'){
      finish();const styles={disc:-1,circle:-2,square:-3,decimal:-4,'lower-alpha':-5,'upper-alpha':-6,'lower-roman':-7,'upper-roman':-8};
      const kind=styles[node.style.listStyleType]||({a:-5,A:-6,i:-7,I:-8})[node.getAttribute('type')]||(tag==='UL'?-1:-4);
      const next={style:kind,group:++group,indent:Math.min(12,(list?.indent||0)+1),start:Math.max(1,Math.min(10000,Number(node.getAttribute('start'))||1))};
      for(const child of node.childNodes)walk(child,inherited,next,depth+1,pre);finish();return;
    }
    const paragraph=['P','DIV','H1','H2','H3','H4','H5','H6','BLOCKQUOTE','PRE','LI','TD','TH'].includes(tag);
    if(paragraph){finish();keepEmpty=!['DIV','BLOCKQUOTE'].includes(tag);const align=({left:1,right:2,center:4,justify:8})[node.style.textAlign]||1;current=tag==='LI'&&list?fresh(align,list.style,list.group,list.indent,list.start):fresh(align);}
    if(tag==='BR'){append(inherited,'\u2028');return;}
    const format=tag?readStyle(node,inherited,false):inherited;
    if(tag&&node.style.fontSize&&!/^\d+(\.\d+)?(pt|px)$/.test(node.style.fontSize))format.size=inherited.size;
    format.size=Math.max(6,Math.min(144,format.size||12));format.family=(format.family.split(',')[0]||'Arial').replace(/^['"]|['"]$/g,'').slice(0,200);
    if(['H1','H2','H3','H4','H5','H6'].includes(tag)){format.weight=700;if(!node.style.fontSize)format.size=({H1:24,H2:20,H3:18,H4:16,H5:14,H6:12})[tag];}
    for(const child of node.childNodes)walk(child,format,list,depth+1,pre||tag==='PRE');
    if(paragraph)finish();
  }
  walk(template.content,{...defaults,size:12,family:'Arial'});finish();
  const result=blocks.length?{blocks}:plainDocument('');
  if(!withinTextLimits(result))throw new Error('limit');
  return result;
}
export function insertText(root,text) {
  const selection=root.ownerDocument.getSelection();if(!selection.rangeCount)return;
  const range=selection.getRangeAt(0);if(!root.contains(range.commonAncestorContainer))return;
  text=text.replace(/\r\n?|\u2029/g,'\n');
  if(!acceptsEdit(root,text,range))throw new Error('limit');
  const {first,last}=editRange(root,range,false),parts=text.split('\n');
  let node;
  if(first===last&&first?.textContent&&parts.length===1&&range.startContainer!==root&&range.endContainer!==root){
    range.deleteContents();node=root.ownerDocument.createTextNode(text);range.insertNode(node);
  }else{
    // Replace whole boundary paragraphs, retaining their unselected formatting.
    const {head,tail}=editRange(root,range);
    const fragment=root.ownerDocument.createDocumentFragment();
    if(first&&!first.textContent)head.querySelectorAll('br').forEach(br=>br.remove());
    if(last&&!last.textContent)tail.querySelectorAll('br').forEach(br=>br.remove());
    for(let i=0;i<parts.length;++i){
      const p=first?.cloneNode(false)||root.ownerDocument.createElement('p');
      if(i===0&&head)p.append(head);
      node=root.ownerDocument.createTextNode(parts[i]);p.append(node);
      if(i===parts.length-1&&tail)p.append(tail);
      if(!p.textContent)p.append(root.ownerDocument.createElement('br'));
      fragment.append(p);
    }
    if(first){range.setStartBefore(first);range.setEndAfter(last);}
    range.deleteContents();range.insertNode(fragment);
  }
  range.setStartAfter(node);range.collapse(true);selection.removeAllRanges();selection.addRange(range);
  root.dispatchEvent(new Event('input',{bubbles:true}));
  return node;
}
