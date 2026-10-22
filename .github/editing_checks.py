"""Check mark editing and inline navigation through the browser UI."""
import json
import sys

def check_app_entry(ui):
    from urllib.parse import urlsplit
    origin=ui.url.split('/#',1)[0]
    origin=urlsplit(origin).scheme+'://'+urlsplit(origin).netloc
    assert ui.evaluate("localStorage.getItem('scenepad.link')")=='a'*22
    manifest=ui.evaluate("async()=>{const response=await fetch('/manifest.webmanifest');return {ok:response.ok,type:response.headers.get('content-type'),data:await response.json()};}")
    assert manifest['ok'] and 'application/manifest+json' in manifest['type']
    data=manifest['data']
    assert data['name']==data['short_name']=='ScenePad' and data['display']=='standalone' and data['start_url']=='/'
    assert ui.evaluate("async()=>{const manifest=await fetch('/manifest.webmanifest').then(r=>r.json());for(const icon of manifest.icons){const image=new Image();image.src=icon.src;await image.decode();if(`${image.naturalWidth}x${image.naturalHeight}`!==icon.sizes)return false;}return true;}"), 'App icons do not match the manifest'
    session=ui.context.new_cdp_session(ui)
    try:assert not session.send('Page.getAppManifest')['errors'], 'Chrome rejected the app manifest'
    finally:session.detach()
    ui.goto(origin+'/')
    ui.reload()
    ui.wait_for_function("document.activeElement.id==='password'&&!document.querySelector('#login button').disabled")
    ui.locator('#password').fill('test-only');ui.locator('#login button').click()
    ui.wait_for_function("window.lastAccess==='a'.repeat(22)&&document.getElementById('login').hidden")
    assert ui.evaluate("Object.keys(localStorage).every(key=>!key.includes('credential')&&!key.includes('password'))"), 'App launch persisted credentials'
    ui.goto(origin+'/#invalid')
    ui.reload()
    ui.wait_for_function("document.querySelector('#login button').disabled")
    assert ui.evaluate('window.opens===0'), 'An invalid explicit link used the saved capability'
    ui.evaluate("localStorage.removeItem('scenepad.link')")
    ui.goto(origin+'/')
    ui.reload()
    ui.wait_for_function("document.querySelector('#login button').disabled")
    assert ui.evaluate('window.opens===0'), 'A missing link attempted an unauthenticated connection'
    ui.evaluate('sessionStorage.clear()')
    ui.goto(origin+'/#'+'a'*22)
    ui.reload()
    ui.wait_for_function("!document.getElementById('login').hidden&&document.activeElement.id==='password'")
    assert ui.evaluate("async()=>!navigator.serviceWorker||(await navigator.serviceWorker.getRegistrations()).length===0")
    print('PASS PWA WEB: manifest, icon decoding, Chrome validation, saved-link app launch, password prompt, invalid/missing links and no offline worker',flush=True)

def check_marks(ui):
    modifier = 'Meta' if sys.platform == 'darwin' else 'Control'
    ui.locator('#document').click()
    ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.locator('#search').fill('kept query')
    ui.locator('#document').click()
    ui.evaluate("""async()=>{
        const {render,plainDocument}=await import('/document.mjs'),root=document.getElementById('document');
        render(root,plainDocument('prefix tail'));root.focus();
        const range=document.createRange();range.setStart(root.firstChild.firstChild.firstChild,6);range.collapse(true);
        getSelection().removeAllRanges();getSelection().addRange(range);document.dispatchEvent(new Event('selectionchange'));
    }""")
    ui.locator('#insert-mark').click()
    ui.wait_for_timeout(40)
    assert ui.locator('#document').inner_text() == 'prefix◆[] tail'
    assert ui.evaluate("document.activeElement.id==='document'&&getSelection().anchorOffset===2"), 'Insertion did not focus the empty brackets'
    assert ui.locator('#search').input_value() == 'kept query' and ui.locator('#mark-note-editor').count() == 0
    ui.keyboard.insert_text('Camera 中文 <b>')
    assert ui.locator('#document').inner_text() == 'prefix◆[Camera 中文 <b>] tail'
    assert ui.evaluate("getComputedStyle(document.querySelector('#document [data-mark]')).color==='rgb(255, 179, 71)'"), 'Note color changed'
    ui.keyboard.press('ArrowRight'); ui.wait_for_timeout(20); ui.keyboard.type('X')
    assert ui.locator('#document').inner_text() == 'prefix◆[Camera 中文 <b>]X tail'
    assert ui.evaluate("async()=>{const {serialize}=await import('/document.mjs');return serialize(document.getElementById('document')).blocks[0].runs.find(run=>run.text.includes('X')).color!=='#ffb347';}"), 'Marker color leaked into normal input'
    ui.keyboard.press(modifier+'+z')
    for after in (False,True):
        ui.evaluate("""after=>{
            const root=document.getElementById('document'),mark=root.querySelector('[data-mark]'),range=document.createRange();root.focus();
            if(after)range.setStartAfter(mark);else range.setStartBefore(mark);range.collapse(true);getSelection().removeAllRanges();getSelection().addRange(range);
            document.execCommand('foreColor',false,'#ffb347');
        }""",after)
        ui.keyboard.type('Y')
        assert ui.evaluate("async()=>{const {serialize}=await import('/document.mjs');return serialize(document.getElementById('document')).blocks[0].runs.find(run=>run.text.includes('Y')).color!=='#ffb347';}"), 'A boundary caret retained marker color'
        ui.keyboard.press(modifier+'+z')
    ui.evaluate("()=>{const mark=document.querySelector('#document [data-mark]'),range=document.createRange();range.setStartAfter(mark);range.collapse(true);getSelection().removeAllRanges();getSelection().addRange(range);}")
    ui.keyboard.press('Backspace')
    assert ui.locator('#document').inner_text() == 'prefix tail', 'Whole-note deletion left text behind'
    ui.keyboard.press(modifier+'+z')
    assert ui.locator('#document').inner_text() == 'prefix◆[Camera 中文 <b>] tail', 'Whole-note deletion was not undoable'
    ui.wait_for_function("window.savedDocument?.blocks.some(block=>block.runs.some(run=>run.text.includes('Camera')))" )
    ui.reload(); ui.wait_for_function("document.getElementById('document').textContent.includes('Camera')")
    ui.locator('#document').click(); ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.evaluate("()=>{const mark=document.querySelector('#document [data-mark]'),node=mark.querySelector('span')?.firstChild||mark.firstChild,range=document.createRange();range.setStart(node,2);range.collapse(true);getSelection().removeAllRanges();getSelection().addRange(range);}")
    ui.keyboard.insert_text('Reloaded ')
    assert '◆[Reloaded Camera' in ui.locator('#document').inner_text(), 'A saved mark could not be edited directly'
    ui.wait_for_function("window.savedDocument?.blocks.some(block=>block.runs.some(run=>run.text.includes('Reloaded')))" )
    for width in (1280, 393, 240):
        ui.set_viewport_size({'width':width,'height':844})
        assert ui.evaluate("()=>{const mark=document.querySelector('#document [data-mark]');return getComputedStyle(mark).whiteSpace==='nowrap'&&mark.contentEditable!=='false'&&new Set(Array.from(mark.getClientRects(),r=>Math.round(r.top))).size===1;}"), 'A direct-edit mark became locked or wrapped'
    ui.screenshot(path='preview-web-direct-mark.png')
    ui.evaluate("()=>{window.testState.locked=true;window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}")
    ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
    assert ui.locator('#insert-mark').is_disabled()
    ui.evaluate("()=>{window.testState.locked=false;window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}")
    ui.locator('#document').click();ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    metrics=ui.evaluate("""async()=>{
        const {render,plainDocument,insertMark}=await import('/document.mjs'),root=document.getElementById('document');
        render(root,plainDocument(('A'.repeat(9998)+'\\n').repeat(19)+'tail'));root.focus();
        const original=document.createTreeWalker;let scans=0;
        document.createTreeWalker=function(node,...args){if(node===root)++scans;return original.call(this,node,...args);};
        const start=performance.now();
        try{for(let i=0;i<100;++i){const range=document.createRange();range.selectNodeContents(root.lastChild);range.collapse(false);getSelection().removeAllRanges();getSelection().addRange(range);insertMark(root);}}
        finally{document.createTreeWalker=original;}
        return {elapsed:performance.now()-start,scans,focused:document.activeElement===root,marks:root.querySelectorAll('[data-mark]').length};
    }""")
    assert metrics['focused'] and metrics['marks']==100 and metrics['scans']==0 and metrics['elapsed']<3000, metrics
    print('PASS MARK WEB: body focus, note input, color, deletion, undo, reload, lock and mobile; '+json.dumps(metrics),flush=True)


def check_navigation(ui):
    active=ui.evaluate("window.testState.activeScene")
    ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');render(document.getElementById('document'),plainDocument('123456789\\n'.repeat(19999)+'123456789'));}")
    ui.locator('#percent').click()
    assert ui.locator('#go-to').evaluate('input=>input.selectionStart===0&&input.selectionEnd===input.value.length'), 'Navigation does not select the percentage unit'
    ui.locator('#go-to').fill('15000')
    ui.locator('#go-to').press('Enter')
    ui.wait_for_function("document.getElementById('go-to').hidden")
    assert ui.evaluate("()=>{const root=document.getElementById('document'),line=root.children[14999].getBoundingClientRect(),box=root.getBoundingClientRect();return line.top>=box.top&&line.bottom<=box.bottom;}"), 'Line jump does not show the destination'
    ui.locator('#lines').click()
    ui.locator('#go-to').fill('50%')
    ui.locator('#go-to').press('Enter')
    ui.wait_for_function("document.getElementById('go-to').hidden")
    assert ui.evaluate("()=>{const root=document.getElementById('document');return Math.abs(root.scrollTop/(root.scrollHeight-root.clientHeight)-.5)<.01;}"), 'Percentage jump missed its target'
    position=ui.locator('#document').evaluate('(root)=>root.scrollTop')
    ui.locator('#percent').click()
    ui.locator('#go-to').fill('75')
    ui.locator('#go-to').press('Escape')
    assert ui.locator('#go-to').is_hidden() and ui.locator('#document').evaluate('(root)=>root.scrollTop')==position, 'Canceling a jump changes the position'
    ui.locator('#lines').click()
    ui.locator('#go-to').fill('0')
    ui.locator('#go-to').press('Enter')
    assert ui.locator('#go-to').is_visible() and ui.locator('#go-to').get_attribute('aria-invalid')=='true', 'Invalid jump is not marked inline'
    assert ui.locator('#document').evaluate('(root)=>root.scrollTop')==position, 'Invalid jump changes the position'
    ui.locator('#multiplier').focus()
    assert ui.locator('#go-to').is_hidden(), 'Leaving the input does not cancel navigation'
    for width in (1280,393,240):
        ui.set_viewport_size({'width':width,'height':844})
        before=ui.locator('footer').bounding_box()
        ui.locator('#percent').click()
        assert ui.evaluate("()=>{const field=document.getElementById('go-to').getBoundingClientRect(),footer=document.querySelector('footer').getBoundingClientRect();return field.left>=footer.left&&field.right<=footer.right&&field.top>=footer.top&&field.bottom<=footer.bottom&&parseFloat(getComputedStyle(document.getElementById('go-to')).fontSize)>=16;}"), f'Inline navigation escapes the footer or uses small phone input text at {width}px'
        assert before==ui.locator('footer').bounding_box(), 'Inline editing moves toolbar controls'
        if width==393:ui.screenshot(path='preview-web-inline-navigation.png')
        ui.locator('#go-to').press('Escape')
    ui.set_viewport_size({'width':1280,'height':844})
    navigation=ui.evaluate("""async()=>{
        const root=document.getElementById('document'),html=root.innerHTML,originalWalker=document.createTreeWalker,input=document.getElementById('go-to');
        const getter=Object.getOwnPropertyDescriptor(Node.prototype,'textContent').get;let reads=0,scans=0;
        Object.defineProperty(root,'textContent',{configurable:true,get(){reads++;return getter.call(this);}});
        document.createTreeWalker=function(...args){scans++;return originalWalker.apply(this,args);};
        const start=performance.now();
        try{for(let i=0;i<1000;i++){
            document.getElementById(i%2?'lines':'percent').onclick();input.value=i%2?String(1+i*17%20000):String(i%101)+'%';
            await input.onkeydown({key:'Enter',preventDefault(){}});
        }}finally{document.createTreeWalker=originalWalker;delete root.textContent;}
        return {elapsed:performance.now()-start,reads,scans,unchanged:html===root.innerHTML};
    }""")
    assert navigation['elapsed']<10000 and navigation['reads']==navigation['scans']==0 and navigation['unchanged'], f'Inline navigation exceeds 10 ms per cycle, scans or modifies the note: {navigation}'
    print('PASS: inline navigation, invalid input, focus/Escape cancel, stable phone layout and 1000 indexed jumps: '+json.dumps(navigation),flush=True)
    ui.evaluate("()=>{window.testState.activeScene='small';window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}")
    ui.locator('#target').click()
    ui.locator('#percent').click()
    ui.locator('#go-to').fill('75%')
    ui.locator('#go-to').press('Enter')
    ui.wait_for_function("window.lastSeek?.scene==='small'&&window.lastSeek.target==='75%'&&document.getElementById('percent').textContent==='75%'")
    ui.locator('#target').click()
    ui.evaluate("active=>{window.testState.activeScene=active;window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}",active)
    print('PASS: OBS-mode navigation sends the selected scene and synchronizes final progress',flush=True)


def check_mark_search(ui):
    ui.set_viewport_size({'width':1280,'height':844})
    ui.evaluate(r"async()=>{const {render,plainDocument}=await import('/document.mjs');render(document.getElementById('document'),plainDocument('First ◆\nSecond ◆\nLast'));getSelection().removeAllRanges();}")
    assert not ui.locator('#next-mark').is_disabled(), 'Readonly mark navigation is disabled'
    for expected in ('1/2','2/2','1/2'):
        ui.locator('#next-mark').click()
        assert ui.locator('#matches').inner_text() == expected
        assert ui.evaluate("getSelection().toString()==='◆'&&!window.hasEdit"), 'Mark navigation edits or acquires the document'
    ui.locator('#previous').click()
    assert ui.locator('#matches').inner_text() == '2/2', 'Previous search does not navigate marks'
    ui.set_viewport_size({'width':393,'height':844})
    ui.screenshot(path='preview-web-next-mark.png')
    ui.set_viewport_size({'width':1280,'height':844})
    ui.evaluate(r"async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument(Array(20).fill(('◆'+'A'.repeat(98)).repeat(100)).join('\n')));getSelection().removeAllRanges();}")
    ui.locator('#search').fill('absent')
    navigation = ui.evaluate("""() => {
        const original=document.createTreeWalker.bind(document);let scans=0;
        document.createTreeWalker=(...args)=>{++scans;return original(...args);};
        const root=document.getElementById('document'),html=root.innerHTML,start=performance.now();
        for(let i=0;i<1000;++i)document.getElementById('next-mark').click();
        const elapsed=performance.now()-start,range=getSelection().getRangeAt(0),mark=range.getBoundingClientRect(),viewport=root.getBoundingClientRect();
        document.createTreeWalker=original;
        return {elapsed,scans,unchanged:html===root.innerHTML,visible:mark.top>=viewport.top&&mark.bottom<=viewport.bottom,count:document.getElementById('matches').textContent};
    }""")
    assert navigation['scans']==1 and navigation['elapsed']<5000 and navigation['unchanged'] and navigation['visible'] and navigation['count']=='1000/2000', navigation
    print('PASS: cached next/previous mark navigation, readonly state and 1000 jumps:',json.dumps(navigation),flush=True)
    ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');render(document.getElementById('document'),plainDocument('Short scene'));}")
    ui.locator('#clear-search').click()
    ui.locator('#document').click()
    ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.evaluate("""async()=>{
        const {render,plainDocument}=await import('/document.mjs'),root=document.getElementById('document');
        render(root,plainDocument('prefix◆[] middle◆[] tail◆[] end'));root.dispatchEvent(new Event('input',{bubbles:true}));root.focus();
        const range=document.createRange();range.selectNodeContents(root);range.collapse(true);getSelection().removeAllRanges();getSelection().addRange(range);
    }""")
    ui.locator('#next-mark').click();ui.locator('#next-mark').click()
    assert ui.locator('#matches').inner_text()=='2/3'
    for _ in range(3):ui.keyboard.press('ArrowRight')
    caret=ui.evaluate("()=>{const range=getSelection().getRangeAt(0).cloneRange();range.setStart(document.getElementById('document'),0);return range.toString().length;}")
    ui.keyboard.insert_text('中文')
    ui.wait_for_timeout(220)
    assert ui.locator('#search').input_value()=='' and ui.locator('#matches').inner_text()==''
    assert ui.evaluate("()=>{const root=document.getElementById('document'),range=getSelection().getRangeAt(0).cloneRange();range.setStart(root,0);return document.activeElement===root&&!root.classList.contains('searching')&&!CSS.highlights.has('scene-search')&&range.toString().length;}")==caret+2, 'Automatic search exit moved the caret, focus or left highlights'
    ui.locator('#next-mark').click();assert ui.locator('#matches').inner_text()=='3/3', 'Navigation restarted before the caret'
    ui.locator('#next-mark').click();assert ui.locator('#matches').inner_text()=='1/3', 'Navigation failed to wrap'
    for query in ('tail','◆'):
        ui.locator('#search').fill(query);ui.wait_for_timeout(220)
        ui.locator('#document').focus();ui.keyboard.press('End');ui.keyboard.insert_text('x');ui.wait_for_timeout(220)
        assert ui.locator('#search').input_value()==query and ui.locator('#matches').inner_text(), 'Manual query was cleared on editing'
    ui.evaluate("""async()=>{
        const {render,plainDocument}=await import('/document.mjs'),root=document.getElementById('document');
        render(root,plainDocument(Array(2000).fill('◆'+'A'.repeat(97)).join('\\n')));root.dispatchEvent(new Event('input',{bubbles:true}));root.focus();
        const range=document.createRange();range.selectNodeContents(root);range.collapse(true);getSelection().removeAllRanges();getSelection().addRange(range);
        window.originalSearchWalker=document.createTreeWalker;window.searchScans=0;
        document.createTreeWalker=function(node,...args){if(node===root)++window.searchScans;return window.originalSearchWalker.call(this,node,...args);};
    }""")
    try:
        ui.locator('#next-mark').click()
        assert ui.evaluate('window.searchScans')==1
        edits=ui.evaluate("""()=>{
            const root=document.getElementById('document'),range=document.createRange();root.focus();range.selectNodeContents(root);range.collapse(false);getSelection().removeAllRanges();getSelection().addRange(range);
            window.searchScans=0;const start=performance.now();
            for(let i=0;i<1000;++i)if(!document.execCommand('insertText',false,'x'))throw new Error('Native insertion failed');
            return {elapsed:performance.now()-start,chars:root.textContent.length+root.children.length-1};
        }""")
        ui.wait_for_timeout(300)
        edits['scans']=ui.evaluate('window.searchScans')
        assert edits['elapsed']<10000 and edits['scans']==0 and edits['chars']==198999, edits
        assert ui.locator('#search').input_value()=='' and ui.locator('#matches').inner_text()==''
        assert ui.evaluate("document.activeElement.id==='document'&&!CSS.highlights.has('scene-search')")
        ui.locator('#next-mark').click()
        assert ui.evaluate('window.searchScans')==1 and ui.locator('#matches').inner_text()=='1/2000'
        ui.locator('#next-mark').click()
        assert ui.evaluate('window.searchScans')==1 and ui.locator('#matches').inner_text()=='2/2000'
        ui.screenshot(path='preview-web-mark-search.png')
        print('PASS MARK SEARCH WEB: automatic exit, caret, wraparound, manual queries, one rebuild and cached continuation; '+json.dumps(edits),flush=True)
    finally:
        ui.evaluate('document.createTreeWalker=window.originalSearchWalker;delete window.originalSearchWalker;delete window.searchScans;')
    ui.locator('#clear-search').click()
    ui.keyboard.press('Escape')
    ui.wait_for_function("document.getElementById('document').contentEditable==='false'")


def check_format_tools(ui):
    ui.set_viewport_size({'width':900,'height':844})
    ui.locator('#document').click()
    ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.evaluate("""async()=>{
        const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');
        render(root,plainDocument('source target tail'));
        window.selectText=(a,b)=>{const root=document.getElementById('document'),w=document.createTreeWalker(root,4),r=document.createRange();let offset=0,started=false;while(w.nextNode()){const n=w.currentNode;if(!started&&a<=offset+n.length){r.setStart(n,a-offset);started=true;}if(started&&b<=offset+n.length){r.setEnd(n,b-offset);break;}offset+=n.length;}const s=document.getSelection();s.removeAllRanges();s.addRange(r);};
        root.dispatchEvent(new Event('input',{bubbles:true}));selectText(0,6);
    }""")
    ui.locator('[data-format=strike]').click()
    assert ui.evaluate("async()=>{const {serialize}=await import('/document.mjs');return serialize(document.getElementById('document')).blocks[0].runs[0].strike;}")
    ui.locator('[data-format=strike]').click()
    assert not ui.evaluate("async()=>{const {serialize}=await import('/document.mjs');return serialize(document.getElementById('document')).blocks[0].runs[0].strike;}")
    ui.evaluate("""async()=>{const {formatSelection}=await import('/document.mjs');const root=document.getElementById('document');formatSelection(root,'paint',{weight:700,italic:true,underline:true,strike:true,size:22,family:'Georgia',color:'#b82030',background:'#e0c080'});}""")
    ui.wait_for_function("document.querySelector('[data-format=strike]').getAttribute('aria-pressed')==='true'")
    ui.locator('[data-format=strike]').click()
    assert not ui.evaluate("async()=>{const {serialize}=await import('/document.mjs');return serialize(document.getElementById('document')).blocks[0].runs[0].strike;}")
    ui.locator('[data-format=strike]').click()
    ui.locator('#format-painter').click()
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='true'
    ui.evaluate("selectText(7,13);document.getElementById('document').dispatchEvent(new PointerEvent('pointerup',{bubbles:true}));")
    ui.wait_for_function("document.getElementById('format-painter').getAttribute('aria-pressed')==='false'")
    bounds=ui.evaluate("()=>{selectText(7,13);const b=document.getSelection().getRangeAt(0).getBoundingClientRect();selectText(0,6);document.dispatchEvent(new Event('selectionchange'));return {x:b.x,y:b.y,w:b.width,h:b.height};}")
    ui.locator('#format-painter').click()
    ui.mouse.move(bounds['x']+.2,bounds['y']+bounds['h']/2);ui.mouse.down()
    ui.mouse.move(bounds['x']+bounds['w']-.2,bounds['y']+bounds['h']/2,steps=6);ui.mouse.up()
    ui.wait_for_function("document.getElementById('format-painter').getAttribute('aria-pressed')==='false'")
    painted=ui.evaluate("async()=>{const {serialize}=await import('/document.mjs');return serialize(document.getElementById('document'));}")
    runs=painted['blocks'][0]['runs']
    assert ''.join(r['text'] for r in runs)=='source target tail'
    target=next(r for r in runs if r['text']=='target')
    assert all(target[key]==value for key,value in {'weight':700,'italic':True,'underline':True,'strike':True,'size':22,'family':'Georgia','color':'#b82030','background':'#e0c080'}.items())
    ui.wait_for_function("document.getElementById('characters').textContent==='选中 6 字'")
    ui.screenshot(path='preview-web-format-tools.png')
    ui.locator('#format-painter').click();ui.evaluate("selectText(14,18);document.getElementById('document').dispatchEvent(new KeyboardEvent('keyup',{key:'Shift',bubbles:true}));")
    ui.wait_for_function("document.getElementById('format-painter').getAttribute('aria-pressed')==='false'")
    ui.evaluate("selectText(0,6);document.dispatchEvent(new Event('selectionchange'));")
    ui.locator('#format-painter').dblclick()
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='true'
    assert ui.locator('#format-painter').get_attribute('title')=='格式刷（连续）'
    for a,b in ((7,13),(14,18),(7,13)):
        ui.evaluate("([a,b])=>{selectText(a,b);document.getElementById('document').dispatchEvent(new KeyboardEvent('keyup',{key:'Shift',bubbles:true}));}",[a,b])
        assert ui.locator('#format-painter').get_attribute('aria-pressed')=='true'
        assert ui.evaluate("document.getSelection().toString()") in ('target','tail')
        assert ui.evaluate("async()=>{const {selectionFormat}=await import('/document.mjs');return selectionFormat(document.getElementById('document')).color;}")=='#b82030'
    ui.locator('#format-painter').click()
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='false'
    ui.locator('#format-painter').dblclick();ui.keyboard.press('Escape')
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='false'
    ui.locator('#format-painter').dblclick();ui.evaluate("selectText(18,18)");ui.keyboard.type('x')
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='false'
    ui.keyboard.press('Backspace')
    ui.locator('#format-painter').click();ui.locator('#format-painter').click()
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='false'
    ui.locator('#characters').click()
    ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
    ui.locator('#document').click();ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.locator('#format-painter').dblclick()
    ui.evaluate("()=>{window.testState.locked=true;window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}")
    ui.wait_for_function("document.getElementById('format-painter').disabled&&!document.getElementById('format-painter').classList.contains('painting')")
    assert ui.locator('#format-painter').get_attribute('aria-pressed')=='false' and ui.locator('[data-format=strike]').is_disabled()
    assert ui.evaluate("""()=>{let errors=0;const capture=()=>++errors;window.addEventListener('error',capture);const root=document.getElementById('document'),range=document.createRange(),selection=document.getSelection();range.setStart(root.firstChild.firstChild.firstChild,0);range.setEndAfter(document.querySelector('footer'));selection.removeAllRanges();selection.addRange(range);document.dispatchEvent(new Event('selectionchange'));selection.removeAllRanges();window.removeEventListener('error',capture);return errors===0;}"""), 'Selection outside the readonly script caused a toolbar error'
    ui.evaluate("()=>{window.testState.locked=false;window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}")
    ui.wait_for_function("!document.getElementById('owner').classList.contains('locked')")
    ui.locator('#document').click();ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.evaluate(r"""async()=>{const {render,plainDocument}=await import('/document.mjs');render(document.getElementById('document'),plainDocument(('a'.repeat(98)+'\n').repeat(1999)+'a'.repeat(98)));document.getElementById('document').dispatchEvent(new Event('input',{bubbles:true}));}""")
    elapsed=ui.evaluate("""async()=>{const before=performance.now();for(let i=0;i<1000;++i){selectText(i%2?0:100000,i%2?5:100005);document.dispatchEvent(new Event('selectionchange'));}return performance.now()-before;}""")
    assert elapsed<5000, f'Selection statistics are slow: {elapsed}ms'
    formats=ui.evaluate("""async()=>{const {formatSelection}=await import('/document.mjs'),root=document.getElementById('document'),text=root.textContent,source={weight:700,italic:true,underline:false,strike:false,size:12,family:'Arial',color:'#b82030',background:''};let peak=0;const start=performance.now();for(let i=0;i<50;++i){selectText(100000,100005);const before=performance.now();formatSelection(root,i%2?'paint':'strike',i%2?source:true);peak=Math.max(peak,performance.now()-before);}return {elapsed:performance.now()-start,peak,unchanged:root.textContent===text,paragraphs:root.children.length};}""")
    assert formats['elapsed']<15000 and formats['peak']<2000 and formats['unchanged'] and formats['paragraphs']==2000,formats
    ui.evaluate("selectText(100000,100005);document.dispatchEvent(new Event('selectionchange'));")
    ui.locator('#format-painter').dblclick()
    continuous=ui.evaluate("""()=>{
        const root=document.getElementById('document'),button=document.getElementById('format-painter'),text=root.textContent;
        let inputs=0,peak=0;const count=()=>++inputs;root.addEventListener('input',count);
        const start=performance.now();
        for(let i=0;i<50;++i){
            selectText(i*99,i*99+5);const before=performance.now();
            root.dispatchEvent(new KeyboardEvent('keyup',{key:'Shift',bubbles:true}));peak=Math.max(peak,performance.now()-before);
            if(button.getAttribute('aria-pressed')!=='true')throw Error('Continuous painter stopped');
            root.dispatchEvent(new KeyboardEvent('keyup',{key:'Shift',bubbles:true}));
        }
        root.removeEventListener('input',count);
        return {elapsed:performance.now()-start,peak,inputs,unchanged:root.textContent===text,paragraphs:root.children.length};
    }""")
    assert continuous['elapsed']<15000 and continuous['peak']<2000 and continuous['inputs']==50 and continuous['unchanged'] and continuous['paragraphs']==2000,continuous
    ui.locator('#format-painter').click()
    ui.evaluate("selectText(0,0)")
    ui.locator('#characters').click()
    print(f'PASS CONTINUOUS WEB: 197999 characters, 50 targets, duplicate releases ignored: {continuous}',flush=True)
    print(f'PASS FORMAT WEB: strike toggle, single/continuous painter, reuse, cancel, lock and selected counts; 197999 characters, 1000 selections={elapsed:.1f}ms; 50 formats={formats}',flush=True)

def check_footer(ui):
    for width in (240,280,320,393,520,600,720,800,860,900,990,1000,1001,1280,1440):
        ui.set_viewport_size({'width':width,'height':844})
        ui.evaluate("()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(()=>requestAnimationFrame(done))))")
        geometry=ui.evaluate("""()=>{
            const footer=document.querySelector('footer'),f=footer.getBoundingClientRect();
            const boxes=[...footer.querySelectorAll('button,select,input:not([type=checkbox]),label.check')].filter(n=>n.getClientRects().length).map(n=>n.getBoundingClientRect());
            let overlaps=0;for(let a=0;a<boxes.length;a++)for(let b=a+1;b<boxes.length;b++)if(Math.min(boxes[a].right,boxes[b].right)-Math.max(boxes[a].left,boxes[b].left)>.5&&Math.min(boxes[a].bottom,boxes[b].bottom)-Math.max(boxes[a].top,boxes[b].top)>.5)overlaps++;
            return {inside:boxes.every(b=>b.left>=f.left&&b.right<=f.right&&b.top>=f.top&&b.bottom<=f.bottom),overlaps,fit:document.body.scrollWidth<=innerWidth,stacked:footer.classList.contains('two-rows'),font:parseFloat(getComputedStyle(document.getElementById('multiplier')).fontSize)};
        }""")
        assert geometry['inside'] and geometry['fit'] and not geometry['overlaps'] and geometry['font']>=16,(width,geometry)
        if width==990:assert not geometry['stacked'],geometry
    field=ui.locator('#multiplier')
    for entered,expected in (('2.7','2.7'),('9','5.0'),('0','0.1'),('','0.1'),('3.26','3.3')):
        field.fill(entered);field.press('Enter')
        ui.wait_for_function("value=>document.getElementById('multiplier').value===value",arg=expected)
        assert float(ui.locator('#speed').input_value())==float(expected)
    ui.locator('#speed').focus();ui.keyboard.press('ArrowRight')
    ui.wait_for_function("document.getElementById('multiplier').value==='3.4'")
    ui.set_viewport_size({'width':240,'height':844})
    ui.wait_for_function("document.querySelector('footer').classList.contains('slider-hidden')")
    field.fill('2.4');field.press('Enter')
    ui.wait_for_function("document.getElementById('speed').value==='2.4'")
    ui.locator('#target').click()
    field.fill('2.6');field.press('Enter')
    ui.wait_for_function("window.lastSpeed===2.6&&document.getElementById('multiplier').value==='2.6'")
    ui.locator('#target').click()
    ui.wait_for_function("document.getElementById('multiplier').value==='2.4'")
    field.fill('1');field.press('Enter');ui.set_viewport_size({'width':900,'height':844})
    print('PASS FOOTER WEB: 15 widths, actual one-row fit, no overlaps, numeric speed/clamp/round/reset, hidden slider, local and OBS controls',flush=True)

def check_no_return(ui):
    ui.set_viewport_size({'width':900,'height':844})
    ui.locator('#document').click();ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.evaluate(r"""async()=>{const {render,plainDocument}=await import('/document.mjs'),root=document.getElementById('document');render(root,plainDocument(('a'.repeat(90)+' ◆[note] target\n').repeat(1800)));root.dispatchEvent(new Event('input',{bubbles:true}));}""")
    ui.locator('#characters').click();ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
    for width in (240,320,393,700,900,1440):
        ui.set_viewport_size({'width':width,'height':844})
        ui.evaluate("()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(()=>requestAnimationFrame(done))))")
        geometry=ui.evaluate("""()=>{
            const reader=document.getElementById('reader'),root=document.getElementById('document'),field=document.getElementById('multiplier'),unit=document.querySelector('.speed-unit');
            const box=reader.getBoundingClientRect(),text=root.getBoundingClientRect(),input=field.getBoundingClientRect(),suffix=unit.getBoundingClientRect();
            return {noReturn:!document.querySelector('#return-position,.reader-side'),fullWidth:root.offsetWidth===reader.clientWidth,right:box.right-text.right,
                padding:parseFloat(getComputedStyle(root).paddingRight),unitInside:suffix.left>=input.left&&suffix.right<=input.right,
                unit:unit.textContent,numeric:field.type==='number'&&!field.value.includes('×'),noOverlap:parseFloat(getComputedStyle(field).paddingRight)>=input.right-suffix.left,
                fits:document.body.scrollWidth<=innerWidth};
        }""")
        assert geometry=={'noReturn':True,'fullWidth':True,'right':1,'padding':12,'unitInside':True,'unit':'×','numeric':True,'noOverlap':True,'fits':True},(width,geometry)
    ui.set_viewport_size({'width':900,'height':844})
    field=ui.locator('#multiplier');field.fill('2.4');field.press('Enter')
    ui.wait_for_function("document.getElementById('speed').value==='2.4'")
    field.fill('1');field.press('Enter')
    ui.locator('#search').fill('target');ui.wait_for_function("document.getElementById('matches').textContent.includes('/1800')")
    ui.locator('#next-mark').click();ui.locator('#search').fill('')
    ui.locator('#percent').click();ui.locator('#go-to').fill('80%');ui.locator('#go-to').press('Enter')
    ui.wait_for_function("document.getElementById('go-to').hidden")
    assert ui.locator('#document').evaluate('root=>root.scrollTop>0')
    ui.locator('#target').click()
    ui.evaluate("()=>{window.testState.activeScene='small';window.socket.reply(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:JSON.stringify(window.testState)}}});}")
    ui.locator('#percent').click();ui.locator('#go-to').fill('70%');ui.locator('#go-to').press('Enter')
    ui.wait_for_function("window.lastSeek?.target==='70%'")
    ui.locator('#target').click()
    ui.locator('#document').focus();ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
    ui.keyboard.type('x');ui.locator('#characters').click();ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
    saved_text=ui.locator('#document').text_content()
    ui.locator('#scenes').select_option('large');ui.wait_for_function("!document.getElementById('problem').hidden")
    ui.locator('#scenes').select_option('small');ui.wait_for_function("text=>document.getElementById('document').textContent===text",arg=saved_text)
    assert ui.locator('#return-position,.reader-side').count()==0
    assert ui.evaluate("async()=>{const app=await(await fetch('/app.mjs')).text(),css=await(await fetch('/style.css')).text();return !/returnPosition|return-position|rememberReturn|clearReturn/.test(app)&&!css.includes('reader-side');}")
    print('PASS WEB RETURN REMOVAL: no button, saved origin, handlers or right gutter; six widths; search/marks/local and remote jumps/edit/scene smoke checks; multiplier unit inside numeric field',flush=True)

def check_smooth(ui):
    ui.set_viewport_size({'width':900,'height':844})
    root=ui.locator('#document');root.evaluate('node=>node.scrollTop=(node.scrollHeight-node.clientHeight)*.3')
    ui.evaluate("()=>{window.smoothSamples=[];document.getElementById('document').addEventListener('scroll',()=>window.smoothSamples.push(document.getElementById('document').scrollTop),{passive:true});}")
    before=root.evaluate('node=>node.scrollTop');root.hover();ui.mouse.wheel(0,240);ui.wait_for_timeout(300)
    samples=ui.evaluate('window.smoothSamples')
    assert len(samples)>=2 and any(before<x<before+240 for x in samples) and abs(root.evaluate('node=>node.scrollTop')-before-240)<1.1,samples
    pressure=ui.evaluate("""async()=>{
        const root=document.getElementById('document'),original=requestAnimationFrame,html=root.innerHTML;let frames=0;
        window.requestAnimationFrame=callback=>original(time=>{frames++;callback(time);});
        try{const start=performance.now();for(let i=0;i<1000;i++)root.dispatchEvent(new WheelEvent('wheel',{deltaY:i%2?-120:120,bubbles:true,cancelable:true}));const elapsed=performance.now()-start;
            await new Promise(done=>setTimeout(done,300));const settled=frames;await new Promise(done=>setTimeout(done,150));
            const touch=new TouchEvent('touchmove',{bubbles:true,cancelable:true});root.dispatchEvent(touch);
            return {elapsed,frames,idle:frames===settled,touch:!touch.defaultPrevented,unchanged:root.innerHTML===html};
        }finally{window.requestAnimationFrame=original;}
    }""")
    assert pressure['elapsed']<5000 and pressure['idle'] and pressure['touch'] and pressure['unchanged'],pressure
    transition=ui.evaluate("""async()=>{
        const root=document.getElementById('document'),raf=requestAnimationFrame,cancel=cancelAnimationFrame,pending=new Set();let peak=0;
        window.requestAnimationFrame=callback=>{const id=raf(time=>{pending.delete(id);callback(time);});pending.add(id);peak=Math.max(peak,pending.size);return id;};
        window.cancelAnimationFrame=id=>{pending.delete(id);cancel(id);};
        try{for(let i=0;i<100;i++){root.dispatchEvent(new WheelEvent('wheel',{deltaY:120,bubbles:true,cancelable:true}));document.getElementById('play').click();document.getElementById('play').click();}
            await new Promise(done=>setTimeout(done,300));return {peak,pending:pending.size};
        }finally{window.requestAnimationFrame=raf;window.cancelAnimationFrame=cancel;}
    }""")
    assert transition=={'peak':1,'pending':0},transition
    ui.mouse.wheel(0,240)
    ui.locator('#percent').click();ui.locator('#go-to').fill('50%');ui.locator('#go-to').press('Enter')
    ui.wait_for_function("document.getElementById('go-to').hidden")
    direct=root.evaluate('node=>node.scrollTop');ui.wait_for_timeout(220)
    assert abs(root.evaluate('node=>node.scrollTop')-direct)<1,'Pending smooth motion displaced a direct jump'
    print(f'PASS SMOOTH WEB: short wheel easing, 1000 merged wheel events={pressure}, 100 single-loop play/pause transitions, idle frame loop, native touch and direct-jump cancellation',flush=True)

def check_speed(ui):
    ui.set_viewport_size({'width': 900, 'height': 844})
    assert ui.locator('#target').evaluate('node=>node.value') == 'local'
    for selector in ('#speed', '#multiplier'):
        field = ui.locator(selector)
        assert field.get_attribute('min') == '0.1' and field.get_attribute('max') == '5'
        assert field.get_attribute('step') == '0.1' and field.input_value() == '0.5'
    pressure = ui.evaluate("""()=>{
        const slider=document.getElementById('speed'),field=document.getElementById('multiplier'),start=performance.now();
        for(let i=0;i<2000;i++){
            const speed=(1+i%50)/10;
            if(i%2){field.value=String(speed);field.dispatchEvent(new Event('change'));}
            else{slider.value=String(speed);slider.dispatchEvent(new Event('input'));}
            if(Number(slider.value)!==speed||Number(field.value)!==speed)throw Error('Speed controls diverged');
        }
        return performance.now()-start;
    }""")
    assert pressure < 5000, pressure
    saved = ui.locator('#document').inner_html()
    ui.evaluate(r"""async()=>{
        const {render,plainDocument}=await import('/document.mjs');
        render(document.getElementById('document'),plainDocument(('Reading line with content\n').repeat(1000)));
        document.getElementById('countdown').checked=false;
    }""")
    original = ui.locator('#document').inner_html()
    rates = []
    for speed, duration in ((0.1, 2200), (0.5, 1200), (1, 350), (5, 350)):
        ui.locator('#multiplier').fill(str(speed))
        ui.locator('#multiplier').press('Enter')
        ui.locator('#document').evaluate('root=>root.scrollTop=1000')
        ui.wait_for_timeout(50)
        ui.evaluate("()=>{window.rateOrigin=document.getElementById('document').scrollTop;document.getElementById('play').click();window.rateTime=performance.now();}")
        ui.wait_for_timeout(duration)
        rate = ui.evaluate("""()=>{
            document.getElementById('play').click();
            return {elapsed:performance.now()-window.rateTime,pixels:document.getElementById('document').scrollTop-window.rateOrigin};
        }""")
        expected = rate['elapsed'] * speed / 100
        assert rate['pixels'] > 0 and abs(rate['pixels'] - expected) <= 2, (speed, rate, expected)
        stopped = ui.locator('#document').evaluate('root=>root.scrollTop')
        ui.wait_for_timeout(80)
        assert ui.locator('#document').evaluate('root=>root.scrollTop') == stopped
        rates.append({'speed': speed, **rate, 'expected': expected})
    assert ui.locator('#document').inner_html() == original
    ui.locator('#multiplier').fill('0.3');ui.locator('#multiplier').press('Enter')
    ui.locator('#target').click()
    ui.wait_for_function("document.getElementById('multiplier').value==='0.5'")
    remote = ui.evaluate("""async()=>{
        const slider=document.getElementById('speed'),start=performance.now();
        for(let i=0;i<1000;i++){
            slider.value=String((1+i%50)/10);slider.dispatchEvent(new Event('input'));
            if(i%50===49)await new Promise(requestAnimationFrame);
        }
        return performance.now()-start;
    }""")
    ui.wait_for_function("window.lastSpeed===5&&document.getElementById('multiplier').value==='5.0'")
    assert remote < 15000, remote
    ui.locator('#target').click()
    ui.wait_for_function("document.getElementById('multiplier').value==='0.3'")
    ui.locator('#multiplier').fill('0.5');ui.locator('#multiplier').press('Enter')
    ui.locator('#document').evaluate('(root,html)=>{root.innerHTML=html;root.scrollTop=0;}', saved)
    print(f'PASS SPEED WEB: default 0.5x; all 50 steps; 2000 local changes={pressure:.1f}ms; 1000 OBS controls={remote:.1f}ms; exact rates={rates}; pause and local/OBS isolation', flush=True)
