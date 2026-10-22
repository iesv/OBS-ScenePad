"""Run browser UI checks and document benchmarks."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
from threading import Thread
from urllib.parse import urlsplit
from playwright.sync_api import sync_playwright
from editing_checks import check_marks, check_navigation, check_mark_search, check_app_entry, check_format_tools, check_footer, check_no_return, check_smooth, check_speed

root = Path(__file__).resolve().parent.parent
return_only = "--return-only" in sys.argv
focused = return_only or "--focused-editing" in sys.argv

class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.mjs': 'text/javascript'}
    def log_message(self, *_):
        pass

server = ThreadingHTTPServer(('127.0.0.1', 0), partial(Handler, directory=str(root)))
Thread(target=server.serve_forever, daemon=True).start()
try:
    with sync_playwright() as playwright:
        with playwright.chromium.launch(channel='chrome') as browser:
            result = {"ok": True, "metrics": []}
            if not focused:
                page = browser.new_page()
                page.on('pageerror', lambda error: print('Browser error:', error, flush=True))
                page.goto(f'http://127.0.0.1:{server.server_port}/tests/web-stress.html', timeout=180000)
                page.wait_for_function("document.getElementById('result').textContent !== 'Running'", timeout=180000)
                result = json.loads(page.locator('#result').inner_text())
                if result.get('ok'):
                    for name, text in [('paragraph-10k', 'W' * 9998), ('note-200k', '\n'.join(['W' * 9998] * 20)), ('paragraphs-5000', 'alpha beta\n' * 4999 + 'End')]:
                        page.evaluate("async text => {const {render,plainDocument}=await import('/web/document.mjs');render(document.getElementById('editor'),plainDocument(text));window.typingOriginal=text;}", text)
                        peak = 0
                        for depth in (0, 0.5, 1):
                            before = page.evaluate("""depth => {
                                const root=document.getElementById('editor');root.focus();
                                root.scrollTop=(root.scrollHeight-root.clientHeight)*depth;
                                const box=root.getBoundingClientRect(),x=box.left+box.width/2,y=box.top+box.height/2;
                                const caret=document.caretPositionFromPoint(x,y),range=document.createRange();
                                range.setStart(caret.offsetNode,caret.offset);range.collapse(true);
                                const selection=document.getSelection();selection.removeAllRanges();selection.addRange(range);
                                return {scroll:root.scrollTop,top:range.getBoundingClientRect().top};
                            }""", depth)
                            page.keyboard.type('x')
                            for undo in (False, True):
                                if undo:
                                    page.keyboard.press('Meta+z' if sys.platform == 'darwin' else 'Control+z')
                                after = page.evaluate("""() => ({scroll:document.getElementById('editor').scrollTop,
                                    top:document.getSelection().getRangeAt(0).getBoundingClientRect().top})""")
                                jump = max(abs(after['scroll'] - before['scroll']), abs(after['top'] - before['top']))
                                peak = max(peak, jump)
                                assert jump <= 40, f'{name}: typing/undo moved the viewport {jump}px'
                            assert page.evaluate(r"async () => {const {serialize}=await import('/web/document.mjs');return serialize(document.getElementById('editor')).blocks.map(b=>b.runs.map(r=>r.text).join('')).join('\n')===window.typingOriginal;}"), f'{name}: undo changed the content'
                        result['metrics'].append({'name': name, 'typing-undo-jump': peak})
                else:
                    raise SystemExit(json.dumps(result))
            ui = browser.new_page()
            labels = dict(line.split('=', 1) for line in (root/'data/locale/zh-CN.ini').read_text(encoding='utf-8').splitlines() if '=' in line)
            labels = {key: value.strip('"') for key, value in labels.items()}
            def route_ui(route):
                path = urlsplit(route.request.url).path
                if path == '/config.json':
                    route.fulfill(json={'port': 4455, 'labels': labels})
                else:
                    file = root/'web'/('index.html' if path == '/' else path.lstrip('/'))
                    if file.is_file():
                        route.fulfill(path=file, content_type={'.mjs': 'text/javascript', '.css': 'text/css', '.html': 'text/html', '.webmanifest': 'application/manifest+json', '.png': 'image/png', '.svg': 'image/svg+xml'}[file.suffix])
                    else:
                        route.continue_()
            ui.route('**/*', route_ui)
            ui.add_init_script(('window.focusedRun=true;' if focused else '')+"""window.opens=0;window.loads=0;window.largeCode='size';window.saveCode=null;window.hasEdit=false;window.savedDocument=JSON.parse(sessionStorage.getItem('testSavedDocument')||'null');
                const state={ok:true,epoch:'test',session:'browser',scenes:[{id:'large',name:'Large'},{id:'small',name:'Small'}],
                    activeScene:window.savedDocument?'small':(window.focusedRun?'small':'large'),locked:false,editor:'',scrolling:false,speed:.5,position:0};window.testState=state;
                window.WebSocket=class {
                    constructor(){this.readyState=1;window.socket=this;window.opens++;setTimeout(()=>this.reply(0,{authentication:{salt:'salt',challenge:'test'}}),0);}
                    reply(op,d){this.onmessage?.({data:JSON.stringify({op,d})});}
                    send(text){const {op,d}=JSON.parse(text);if(op===1){queueMicrotask(()=>this.reply(2,{}));return;}
                        const input=JSON.parse(d.requestData.requestData.payload);window.lastAccess=input.access;let response=state;
                        if(input.action==='GetDocument'){window.loads++;response=input.scene==='large'?{ok:false,code:window.largeCode}:
                            {ok:true,epoch:'test',revision:1,editable:true,blocks:window.savedDocument?.blocks||[{align:1,runs:[{text:'Short scene'}]}]};}
                        if(input.action==='AcquireEdit'){window.hasEdit=true;response={ok:true,epoch:'test',revision:1,editable:true,blocks:window.savedDocument?.blocks||[{align:1,runs:[{text:'Short scene'}]}]};}
                        if(input.action==='ReleaseEdit')window.hasEdit=false;window.savedDocument=JSON.parse(sessionStorage.getItem('testSavedDocument')||'null');
                        if(input.action==='KeepAlive')response={ok:true,epoch:'test',editing:window.hasEdit,locked:false};
                        if(input.action==='SaveDocument'){response=window.saveCode?{ok:false,code:window.saveCode}:{ok:true,epoch:'test',revision:1};if(!window.saveCode){window.savedDocument=input.document;sessionStorage.setItem('testSavedDocument',JSON.stringify(input.document));}}
                        if(input.action==='Scroll'&&input.command==='speed'){window.lastSpeed=state.speed=input.speed;}
                        if(input.action==='Scroll'&&input.command==='seek'){if(window.seekCode)response={ok:false,code:window.seekCode};else{window.lastSeek=input;state.activeScene=input.scene;state.position=parseInt(input.target)/100;}}
                        if(input.action==='Scroll'&&input.command==='return')throw Error('Obsolete browser return command');
                        if(input.action==='ExportText')response={ok:true,text:'剧本 😀\\nSecond line'};
                        if(input.action==='ExportHtml')response={ok:true,html:'<p><b>剧本 😀</b></p>'};
                        queueMicrotask(()=>this.reply(7,{requestId:d.requestId,requestStatus:{result:true},responseData:{responseData:{payload:JSON.stringify(response)}}}));}
                    close(){this.readyState=3;queueMicrotask(()=>this.onclose?.({code:1000}));}
                };""")
            ui.goto(f'http://127.0.0.1:{server.server_port}/#' + 'a'*22)
            ui.wait_for_function("document.activeElement.id==='password'")
            if not return_only:check_app_entry(ui)
            ui.locator('#password').fill('test-only')
            ui.locator('#login button').click()
            if focused:
                ui.wait_for_function("document.getElementById('document').textContent==='Short scene'")
                if return_only:
                    check_no_return(ui)
                    raise SystemExit(0)
                check_speed(ui)
                check_footer(ui)
                check_format_tools(ui)
                check_no_return(ui)
                check_smooth(ui)
                check_navigation(ui)
                check_mark_search(ui)
                check_marks(ui)
                raise SystemExit(0)
            ui.wait_for_function("document.getElementById('message').textContent.includes('正文过长')")
            ui.wait_for_timeout(1200)
            assert ui.evaluate('window.opens===1&&window.loads===1'), 'Oversized documents cause reconnects or repeated loads'
            assert ui.locator('#connection').inner_text() == labels['RemoteConnected']
            ui.locator('#retry').click()
            ui.wait_for_function('window.loads===2')
            ui.locator('#scenes').select_option('small')
            ui.wait_for_function("document.getElementById('document').textContent==='Short scene'")
            assert ui.locator('#problem').is_hidden()
            for width in (240, 393, 768, 1280):
                ui.set_viewport_size({'width': width, 'height': 844})
                wrapping = ui.evaluate(r"""async()=>{
                    const {render,plainDocument}=await import('/document.mjs'),root=document.getElementById('document'),saved=root.innerHTML;
                    const text=('alpha Bravo charlie delta, echo. (Foxtrot) hotel. 他说：“你好，世界！”（继续阅读）测试。 ').repeat(12);
                    render(root,plainDocument(text));root.scrollTop=0;
                    const node=root.firstChild.firstChild.firstChild,range=document.createRange(),lines=[];
                    for(let i=0;i<text.length;i++){
                        range.setStart(node,i);range.setEnd(node,i+1);const top=Math.round(range.getBoundingClientRect().top);
                        if(!lines.length||lines.at(-1).top!==top)lines.push({top,start:i,text:''});lines.at(-1).text+=text[i];
                    }
                    const words=lines.every(line=>!line.start||!/[A-Za-z]/.test(text[line.start-1])||!/[A-Za-z]/.test(text[line.start]));
                    const punctuation=lines.every(line=>{const part=line.text.trim();return !part||(!'，。！？；：、）》」』】”’'.includes(part[0])&&!'（《「『【“‘'.includes(part.at(-1)));});
                    const unchanged=root.textContent===text,weight=getComputedStyle(root.firstChild,'::before').fontWeight;
                    root.firstChild.dataset.list='-4';root.firstChild.dataset.marker='1.';
                    const listWeight=getComputedStyle(root.firstChild,'::before').fontWeight;
                    render(root,plainDocument('W'.repeat(9998)));const fits=root.scrollWidth<=root.clientWidth;
                    root.innerHTML=saved;return {words,punctuation,unchanged,fits,weight,listWeight,lines:lines.length};
                }""")
                assert wrapping['lines']>1 and all(wrapping[key] for key in ('words','punctuation','unchanged','fits')), f'Natural wrapping failed at {width}px: {wrapping}'
                assert wrapping['weight']==wrapping['listWeight']=='700', f'Line/list numbers are not bold at {width}px: {wrapping}'
            ui.set_viewport_size({'width':1280,'height':844})
            print('PASS: natural word/punctuation wrapping, overlong-word fallback and bold line/list numbers at 240-1280px',flush=True)
            assert ui.evaluate("Math.abs(parseFloat(getComputedStyle(document.getElementById('document')).lineHeight)/parseFloat(getComputedStyle(document.getElementById('document')).fontSize)-1.4)<.001"), 'Default browser line spacing is not 1.4x'
            assert ui.locator('#characters').inner_text() == labels['CharacterCount'].replace('%1', '11')
            assert '199,989' in ui.locator('#characters').get_attribute('aria-label')
            ui.locator('#search').fill('scene')
            ui.wait_for_function("document.getElementById('matches').textContent==='1/1'")
            assert ui.evaluate("CSS.highlights.get('scene-search').size===1&&document.getElementById('document').classList.contains('searching')"), 'Active search result has no persistent highlight'
            assert ui.evaluate("getComputedStyle(document.getElementById('document'),'::selection').backgroundColor==='rgb(255, 242, 0)'"), 'Search selection color is not contrasting'
            for width in (1280, 1000, 768, 600, 490, 430, 393, 390, 360, 320, 280, 240):
                ui.set_viewport_size({'width': width, 'height': 844})
                ui.locator('#scenes').focus()
                ui.evaluate("()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(()=>requestAnimationFrame(done))))")
                assert ui.evaluate("getComputedStyle(document.getElementById('scenes')).outlineWidth==='1px'"), 'Focus outline is thicker than one pixel'
                assert ui.evaluate("document.body.scrollWidth<=innerWidth"), f'Horizontal overflow at {width}px'
                positions = ui.evaluate("""() => {
                    const box = id => document.getElementById(id).getBoundingClientRect(),
                        footer = document.querySelector('footer').getBoundingClientRect(),
                        header = document.querySelector('.header-content').getBoundingClientRect(),
                        owner = box('owner'), target = box('target'), paragraph = box('paragraph'), percent = box('percent'),
                        scene = box('scenes'), search = box('search'), row = document.querySelector('.scene-row').getBoundingClientRect(),
                        iconOffset = Math.max(...Array.from(document.querySelectorAll('.tools button svg,.search button svg'), svg => {
                            const icon=svg.getBoundingClientRect(), button=svg.parentElement.getBoundingClientRect();
                            if(!icon.width)return 0;
                            const bounds=svg.getBBox(), matrix=svg.getScreenCTM(),
                                center=new DOMPoint(bounds.x+bounds.width/2,bounds.y+bounds.height/2).matrixTransform(matrix);
                            return Math.max(Math.abs(center.x-button.x-button.width/2),Math.abs(center.y-button.y-button.height/2));
                        }));
                    return {iconOffset, center: Math.abs(owner.x + owner.width/2 - header.x - header.width/2),
                        right: footer.right-target.right, bottom: footer.bottom-target.bottom,
                        paragraphBottom: paragraph.bottom, targetBottom: target.bottom, percentRight: footer.right-percent.right,
                        sceneRow: Math.abs(scene.y+scene.height/2-search.y-search.height/2),
                        sceneLeft: scene.left-row.left, searchRight: row.right-search.right};
                }""")
                assert positions['center'] < 1, f'Header status is not centered at {width}px: {positions}'
                assert positions['iconOffset'] < 1, f'Toolbar icons are not centered at {width}px: {positions}'
                assert positions['percentRight'] >= 11, f'Scroll percentage is too close to the right edge at {width}px: {positions}'
                assert positions['sceneRow'] < 1 and positions['sceneLeft'] >= 0 and positions['searchRight'] >= 0, f'Scene and search controls are not on one bounded row at {width}px: {positions}'
                if ui.locator('footer').evaluate("node=>node.classList.contains('two-rows')"):
                    assert 0 <= positions['right'] <= 8 and 0 <= positions['bottom'] <= 8, f'Reading target is not at the bottom right at {width}px: {positions}'
                    assert abs(positions['paragraphBottom'] - positions['targetBottom']) < 1, f'Reading target is outside the paragraph row at {width}px'
                ui.locator('#characters').click()
                assert ui.evaluate("getComputedStyle(document.getElementById('characters'),'::after').content.includes('199,989')"), 'Touch/focus does not reveal remaining capacity'
                if width in (490, 393, 320):
                    ui.screenshot(path=f'preview-web-{width}.png')
            ui.set_viewport_size({'width': 393, 'height': 844})
            ui.locator('#characters').evaluate('(button)=>button.blur()')
            ui.screenshot(path='preview-web-features.png')
            check_speed(ui)
            check_footer(ui)
            check_format_tools(ui)
            check_no_return(ui)
            check_smooth(ui)
            ui.set_viewport_size({'width': 1280, 'height': 844})
            ui.locator('#search').fill('target')
            ui.wait_for_function("CSS.highlights.has('scene-search')")
            ui.locator('#clear-search').click()
            assert ui.evaluate("!CSS.highlights.has('scene-search')&&!document.getElementById('document').classList.contains('searching')"), 'Cleared search leaves stale color'
            check_mark_search(ui)
            assert ui.evaluate("()=>{const event=new MouseEvent('contextmenu',{bubbles:true,cancelable:true,button:2});document.getElementById('document').dispatchEvent(event);return !event.defaultPrevented&&!document.querySelector('[role=menu],.popup-menu');}"), 'Native context menu is intercepted'
            ui.locator('#document').click()
            ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
            assert ui.evaluate("getComputedStyle(document.querySelector('#document p'),'::before').fontWeight==='700'"), 'Editing line numbers are not bold'
            assert not ui.locator('#insert-mark').is_disabled()
            check_marks(ui)
            for fallback in (False,True):
                ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument('alpha beta'));root.dispatchEvent(new Event('input',{bubbles:true}));}")
                if fallback:ui.evaluate("Object.defineProperty(document,'caretPositionFromPoint',{configurable:true,value:undefined})")
                point=ui.evaluate("""()=>{const root=document.getElementById('document'),range=document.createRange();range.setStart(root.firstChild.firstChild.firstChild,5);range.collapse(true);const rect=range.getBoundingClientRect();return {x:rect.x+.1,y:rect.y+rect.height/2};}""")
                ui.keyboard.down('Alt');ui.keyboard.down('Shift')
                ui.mouse.click(point['x'],point['y'])
                ui.keyboard.up('Shift');ui.keyboard.up('Alt')
                assert ui.locator('#document').inner_text()=='alpha◆[] beta', 'Alt-Shift-click missed the clicked position'
                assert ui.evaluate("document.activeElement.id==='document'"), 'Modified click lost body focus'
                ui.keyboard.type('x')
                assert ui.locator('#document').inner_text()=='alpha◆[x] beta', 'Modified click lost the typing caret'
                if fallback:ui.evaluate("delete document.caretPositionFromPoint")
            print('PASS: Alt/Option-Shift-click insertion with both caret hit-test APIs',flush=True)
            ui.screenshot(path='preview-web-mark.png')
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument('Short scene'));root.dispatchEvent(new Event('input',{bubbles:true}));}")
            ui.keyboard.press('Escape')
            ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
            assert ui.locator('#insert-mark').is_disabled(), 'Readonly web mark action is enabled'
            ui.locator('#document').click()
            ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
            ui.context.grant_permissions(['clipboard-read','clipboard-write'])
            modifier='Meta' if sys.platform=='darwin' else 'Control'
            ui.locator('#document').focus()
            ui.evaluate("()=>{const root=document.getElementById('document'),range=document.createRange();range.setStart(root.firstChild.firstChild.firstChild,0);range.setEnd(root.firstChild.firstChild.firstChild,5);getSelection().removeAllRanges();getSelection().addRange(range);}")
            ui.keyboard.press(modifier+'+x')
            ui.wait_for_function("document.getElementById('document').textContent.replaceAll(String.fromCharCode(160),' ')===' scene'",timeout=3000)
            assert ui.evaluate("navigator.clipboard.readText()")=='Short', 'Native cut lost clipboard text'
            ui.wait_for_timeout(5100)
            assert ui.locator('#document').get_attribute('contenteditable')=='true', 'Editing turn expired during native clipboard use'
            ui.evaluate("navigator.clipboard.writeText('Hello')")
            ui.keyboard.press(modifier+'+v')
            ui.wait_for_function("document.getElementById('document').textContent.replaceAll(String.fromCharCode(160),' ')==='Hello scene'")
            ui.evaluate("()=>{const root=document.getElementById('document'),range=document.createRange();const walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);let node;while((node=walker.nextNode())&&!node.textContent.includes('Hello')){}range.setStart(node,0);range.setEnd(node,5);getSelection().removeAllRanges();getSelection().addRange(range);}")
            ui.evaluate("navigator.clipboard.writeText('')")
            ui.keyboard.press(modifier+'+v')
            assert ui.locator('#document').inner_text().replace('\u00a0',' ')=='Hello scene', 'Empty paste deleted selected text'
            ui.evaluate("getSelection().getRangeAt(0).collapse(false)")
            ui.evaluate("navigator.clipboard.writeText(' 😀')")
            ui.keyboard.press(modifier+'+v')
            ui.wait_for_function("document.getElementById('document').textContent.replaceAll(String.fromCharCode(160),' ')==='Hello 😀 scene'")
            ui.evaluate("()=>{const root=document.getElementById('document'),range=document.createRange();range.selectNodeContents(root);getSelection().removeAllRanges();getSelection().addRange(range);document.dispatchEvent(new Event('selectionchange'));const color=document.getElementById('color');color.value='#ff0000';color.dispatchEvent(new Event('change'));}")
            ui.wait_for_function("getSelection().toString().replaceAll(String.fromCharCode(160),' ')==='Hello 😀 scene'&&getComputedStyle(document.querySelector('#document span'),'::selection').color==='rgb(255, 0, 0)'")
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument('Short scene'));root.dispatchEvent(new Event('input',{bubbles:true}));}")
            ui.keyboard.press('Escape')
            ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
            print('PASS: native context menu, keyboard cut/paste, empty clipboard and Unicode preservation', flush=True)
            assert ui.locator('#export option').evaluate_all("options=>options.length===2&&options.every(option=>option.value&&option.textContent.trim()&&!option.disabled)"), 'Export menu contains a blank or disabled placeholder'
            assert ui.evaluate("getComputedStyle(document.querySelector('#document p'),'::before').fontWeight==='700'"), 'Readonly line numbers are not bold'
            for kind, expected in [('txt', '剧本 😀\nSecond line'), ('txt', '剧本 😀\nSecond line'), ('html', '<p><b>剧本 😀</b></p>'), ('html', '<p><b>剧本 😀</b></p>')]:
                with ui.expect_download() as pending:
                    ui.locator('#export').select_option(kind)
                download = pending.value
                assert download.suggested_filename == f'ScenePad.{kind}'
                assert Path(download.path()).read_text(encoding='utf-8') == expected
                assert ui.locator('#export').evaluate('select=>select.selectedIndex===-1'), 'Export selection is not reset for repeat downloads'
            print('PASS: two export formats without a placeholder, repeated HTML/TXT downloads',flush=True)
            check_navigation(ui)
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');render(document.getElementById('document'),plainDocument('Reading line\\n'.repeat(500)));document.getElementById('document').scrollTop=0;document.getElementById('speed').value='5';}")
            ui.locator('#play').click()
            ui.wait_for_function("document.getElementById('document').scrollTop>4")
            ui.locator('#document').hover()
            ui.mouse.wheel(0, 300)
            ui.wait_for_timeout(300)
            manual_position=ui.locator('#document').evaluate('(root)=>root.scrollTop')
            assert ui.locator('#play').get_attribute('title')==labels['Pause']
            assert ui.locator('#document').get_attribute('contenteditable')=='false'
            ui.wait_for_timeout(600)
            assert ui.locator('#document').evaluate('(root)=>root.scrollTop')>manual_position, 'Playback did not continue after wheel easing'
            ui.wait_for_function('(position)=>document.getElementById("document").scrollTop>position',arg=manual_position)
            ui.evaluate("()=>{const root=document.getElementById('document');root.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true}));root.scrollTop=100;}")
            ui.wait_for_timeout(1200)
            assert ui.locator('#document').evaluate('(root)=>root.scrollTop')==100, 'Playback fought the pointer drag'
            ui.evaluate("window.dispatchEvent(new PointerEvent('pointerup'))")
            ui.wait_for_timeout(600)
            assert ui.locator('#document').evaluate('(root)=>root.scrollTop')==100, 'Drag release did not start a fresh hold'
            ui.wait_for_function('document.getElementById("document").scrollTop>100')
            ui.locator('#play').click()
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');render(document.getElementById('document'),plainDocument('Short scene'));}")
            print('PASS: wheel easing preserves playback; pointer drag holds and resumes from the chosen position',flush=True)
            ui.locator('#document').click()
            ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
            ui.evaluate("window.saveCode='save'")
            ui.locator('#document').fill('Pending 剧本 😀')
            ui.keyboard.press('Escape')
            ui.wait_for_function("document.getElementById('problem').classList.contains('storage-error')&&!document.getElementById('problem').hidden")
            assert ui.locator('#message').inner_text() == labels['RemoteStorageFailed']
            assert ui.evaluate("localStorage.getItem(`scenepad.draft.${location.host}.small`).includes('Pending 剧本 😀')"), 'Failed save lost its recovery draft'
            ui.evaluate('window.saveCode=null')
            ui.locator('#retry').click()
            ui.wait_for_function("localStorage.getItem(`scenepad.draft.${location.host}.small`)===null")
            assert ui.locator('#document').inner_text() == 'Pending 剧本 😀', 'Recovered save changed the pending text'
            assert ui.evaluate("localStorage.getItem(`scenepad.draft.${location.host}.small`)===null"), 'Recovered save did not clear its draft'
            ui.keyboard.press('Escape')
            ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
            print('PASS: search color/clear, character quota, 1px focus, mobile layout, HTML/TXT downloads and failed-save recovery', flush=True)
            ui.locator('#document').click()
            ui.wait_for_function("document.getElementById('document').contentEditable==='true'")
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument('A'.repeat(10000)+('\\n'+'B'.repeat(9999)).repeat(19)));const r=document.createRange();r.selectNodeContents(root.lastChild);r.collapse(false);getSelection().removeAllRanges();getSelection().addRange(r);}")
            ui.keyboard.type('x')
            assert ui.evaluate("document.getElementById('document').textContent.length===199981"), 'Total cap changed the note'
            assert ui.locator('#message').inner_text() == labels['TextLimit']
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument('A'.repeat(6000)+'\\n'+'B'.repeat(6000)));const r=document.createRange();r.setStart(root.lastChild.firstChild.firstChild,0);r.collapse(true);getSelection().removeAllRanges();getSelection().addRange(r);}")
            ui.keyboard.press('Backspace')
            assert ui.evaluate("document.getElementById('document').children.length===2"), 'Backspace bypasses the paragraph cap'
            ui.evaluate("async()=>{const {render,plainDocument}=await import('/document.mjs');const root=document.getElementById('document');render(root,plainDocument('A'.repeat(9999)));root.dispatchEvent(new CompositionEvent('compositionstart',{bubbles:true}));root.firstChild.textContent='A'.repeat(10001);root.dispatchEvent(new InputEvent('input',{bubbles:true,isComposing:true}));root.dispatchEvent(new CompositionEvent('compositionend',{bubbles:true}));}")
            assert ui.evaluate("document.getElementById('document').textContent==='A'.repeat(9999)"), 'IME rejection loses existing text'
            ui.keyboard.press('Escape')
            ui.wait_for_function("document.getElementById('document').contentEditable==='false'")
            print('PASS: browser total cap, paragraph-merge rejection and non-cancelable IME recovery', flush=True)
            ui.locator('#scenes').select_option('large')
            ui.wait_for_function("!document.getElementById('problem').hidden")
            for width in (1280, 768, 390, 320, 240):
                ui.set_viewport_size({'width': width, 'height': 844})
                geometry = ui.evaluate("""() => {
                    const box=document.getElementById('problem').getBoundingClientRect(),
                        message=document.getElementById('message').getBoundingClientRect(),
                        button=document.getElementById('retry').getBoundingClientRect();
                    return {center:Math.abs(message.y+message.height/2-button.y-button.height/2),
                        gap:box.right-button.right,height:box.height,button:button.height};
                }""")
                assert geometry['center'] < 1 and 0 <= geometry['gap'] <= 10, f'Notice alignment at {width}px: {geometry}'
                assert geometry['height'] <= geometry['button']+16, f'Notice wrapped at {width}px'
            ui.set_viewport_size({'width': 390, 'height': 844})
            ui.screenshot(path='preview-web-notice.png')
            for code in ('read', 'save'):
                previous_loads = ui.evaluate('window.loads')
                ui.evaluate('code => window.largeCode=code', code)
                ui.locator('#retry').click()
                ui.wait_for_function(f'window.loads==={previous_loads+1}')
                ui.wait_for_function("document.getElementById('message').textContent===\"" + labels['RemoteStorageFailed'] + "\"")
                ui.wait_for_timeout(1100)
                assert ui.evaluate(f'window.opens===1&&window.loads==={previous_loads+1}'), 'Storage errors cause reconnects or repeated loads'
            ui.evaluate("sessionStorage.removeItem('testSavedDocument')")
            ui.evaluate("localStorage.setItem(`scenepad.draft.${location.host}.large`,JSON.stringify({epoch:'test',revision:1,document:{blocks:[{runs:[{text:'Kept draft'}]}]}}))")
            ui.reload()
            ui.wait_for_function("document.getElementById('document').textContent==='Kept draft'")
            assert ui.locator('#download').is_visible() and ui.locator('#retry').is_hidden()
            ui.evaluate("localStorage.setItem(`scenepad.draft.${location.host}.large`,JSON.stringify({epoch:'test',revision:1,document:{blocks:[{runs:[{text:'W'.repeat(10001)}]}]}}))")
            ui.reload()
            ui.wait_for_function("document.getElementById('message').textContent===" + json.dumps(labels['TextLimit']))
            assert ui.evaluate("document.getElementById('document').textContent===''"), 'Oversized recovery draft was rendered'
            assert ui.evaluate("JSON.parse(localStorage.getItem(`scenepad.draft.${location.host}.large`)).document.blocks[0].runs[0].text.length===10001"), 'Oversized recovery draft was overwritten'
            ui.once('dialog', lambda dialog: dialog.accept())
            ui.locator('#reload').click()
            ui.wait_for_function("document.getElementById('document').textContent===''")
            ui.locator('#disconnect').click()
            ui.wait_for_function("document.activeElement.id==='password'")
            print('PASS: password focus, document/storage error recovery, explicit retry, scene switching and single-row notices at 240-1280px', flush=True)
        for metric in result.get('metrics', []):
            print('WEB ' + json.dumps(metric), flush=True)
        print('PASS: real-browser long-text and soft-break stress', flush=True)
finally:
    server.shutdown()
    server.server_close()
