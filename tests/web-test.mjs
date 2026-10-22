import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {test} from 'node:test';
import {sha256,Connection} from '../web/protocol.mjs';
import {plainDocument,updateParagraphs,withinTextLimits,characterCount} from '../web/document.mjs';
const authenticate=(password,salt,challenge)=>createHash('sha256').update(createHash('sha256').update(password+salt).digest('base64')+challenge).digest('base64');

test('text imports normalize line endings and retain empty paragraphs and Unicode',()=>{
  const data=plainDocument('\ufeffFirst\r\n\r\n剧本 😀\rLast');
  assert.equal(data.blocks.length,4);assert.deepEqual(data.blocks[1].runs,[]);
  assert.equal(data.blocks[2].runs[0].text,'剧本 😀');assert.equal(data.blocks[3].runs[0].text,'Last');
  assert.throws(()=>plainDocument('x'.repeat(10001)),/limit/);
});
test('scene and paragraph limits accept their exact boundaries without truncation',()=>{
  const boundary='x'.repeat(10000)+('\n'+'y'.repeat(9999)).repeat(19);
  assert.equal(boundary.length,200000);assert.ok(withinTextLimits(plainDocument(boundary)));
  assert.throws(()=>plainDocument(boundary+'x'),/limit/);
  assert.throws(()=>plainDocument('x'.repeat(10001)),/limit/);
  assert.equal(plainDocument('\ufeff'+boundary.replaceAll('\n','\r\n')).blocks.length,20);
  assert.ok(!withinTextLimits({blocks:[{runs:[{text:'x'.repeat(5000)},{text:'y'.repeat(5001)}]}]}));
  assert.equal(plainDocument('\n'.repeat(19999)).blocks.length,20000);
  assert.throws(()=>plainDocument('\n'.repeat(20000)),/limit/);
  assert.ok(!withinTextLimits({blocks:Array.from({length:20001},()=>({runs:[]}))}));
});
test('character counts match UTF-16 limits, including paragraphs and soft breaks',()=>{
  assert.equal(characterCount({blocks:[]}),0);
  assert.equal(characterCount(plainDocument('')),0);
  assert.equal(characterCount(plainDocument('剧本 😀\nB\u2028C')),9);
  assert.equal(characterCount(plainDocument('x'.repeat(10000)+('\n'+'y'.repeat(9999)).repeat(19))),200000);
});
test('paragraph tools change selected paragraphs and keep unrelated lists separate',()=>{
  const data=plainDocument('One\nTwo\nThree');const before=JSON.stringify(data.blocks.map(block=>block.runs));
  updateParagraphs(data,[0,1],'list',-4);const group=data.blocks[0].listGroup;
  assert.equal(data.blocks[1].listGroup,group);assert.equal(data.blocks[2].list,0);
  updateParagraphs(data,[2],'list',-4);assert.notEqual(data.blocks[2].listGroup,group);
  updateParagraphs(data,[1],'align',8);assert.equal(data.blocks[1].align,8);assert.equal(data.blocks[0].align,1);
  updateParagraphs(data,[0],'indent',100);assert.equal(data.blocks[0].listIndent,12);
  updateParagraphs(data,[0],'indent',-100);assert.equal(data.blocks[0].listIndent,1);
  updateParagraphs(data,[0],'list',0);assert.equal(data.blocks[0].listGroup,0);
  assert.equal(JSON.stringify(data.blocks.map(block=>block.runs)),before);
});

test('SHA-256 matches the native cryptographic implementation',()=>{
  const digest=text=>createHash('sha256').update(text).digest();
  for(const value of ['', 'abc', 'a'.repeat(55), 'a'.repeat(64), 'a'.repeat(100000), '场景剧本 😀 🔒'])
    assert.deepEqual(Buffer.from(sha256(value)),digest(value));
});

class Socket {
  static current;
  constructor(url){this.url=url;this.readyState=1;this.sent=[];Socket.current=this;}
  send(packet){this.sent.push(JSON.parse(packet));}
  close(code=1000){this.readyState=3;this.onclose?.({code});}
  receive(op,d){this.onmessage({data:JSON.stringify({op,d})});}
}
globalThis.WebSocket=Socket;
test('OBS handshake, vendor envelope, event decoding and disconnect recovery',async()=>{
  let event,closed=false;
  const connection=new Connection(value=>{event=value;},()=>{closed=true;});
  const promise=connection.connect('ws://127.0.0.1:4455','密码🌍');const socket=Socket.current;
  socket.receive(0,{authentication:{salt:'salt',challenge:'challenge'}});
  assert.equal(socket.sent[0].d.authentication,authenticate('密码🌍','salt','challenge'));
  assert.equal(socket.sent[0].d.eventSubscriptions,512);
  socket.receive(2,{});await promise;
  const request=connection.request({action:'Open',access:'capability'});
  const sent=socket.sent.at(-1);
  assert.equal(sent.d.requestData.vendorName,'ScenePad');
  assert.equal(JSON.parse(sent.d.requestData.requestData.payload).action,'Open');
  socket.receive(7,{requestId:sent.d.requestId,requestStatus:{result:true},responseData:{responseData:{payload:'{"ok":true,"session":"session"}'}}});
  assert.equal((await request).session,'session');
  socket.receive(5,{eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:'Changed',eventData:{payload:'{"epoch":"new"}'}}});
  assert.equal(event.epoch,'new');
  const pending=connection.request({action:'SaveDocument'});socket.close();
  await assert.rejects(pending,/disconnected/);assert.ok(closed);assert.equal(connection.pending.size,0);
});
test('authentication-disabled servers are rejected',async()=>{
  const connection=new Connection(()=>{},()=>{});const promise=connection.connect('ws://localhost','');
  Socket.current.receive(0,{});await assert.rejects(promise,/password-required/);
});

test('compact scroll events stay distinct from document changes',async()=>{
  const events=[];
  const connection=new Connection((data,type)=>events.push({data,type}),()=>{});
  const promise=connection.connect('ws://localhost','secret'),socket=Socket.current;
  socket.receive(0,{authentication:{salt:'salt',challenge:'challenge'}});socket.receive(2,{});await promise;
  const payload={epoch:'collection',scene:'current',position:.75};
  const event=type=>({eventType:'VendorEvent',eventData:{vendorName:'ScenePad',eventType:type,eventData:{payload:JSON.stringify(payload)}}});
  socket.receive(5,event('Scroll'));socket.receive(5,event('Unrelated'));
  assert.deepEqual(events,[{data:payload,type:'Scroll'}]);
  assert.equal(connection.pending.size,0);assert.equal(socket.sent.length,1);
  connection.close();
});

test('remembered credentials authenticate a new challenge without the original password',async()=>{
  const first=new Connection(()=>{},()=>{});const login=first.connect('ws://localhost','secret');
  Socket.current.receive(0,{authentication:{salt:'salt',challenge:'first'}});Socket.current.receive(2,{});await login;
  const remembered=JSON.parse(JSON.stringify(first.credential));first.close();
  assert.equal(remembered.secret,createHash('sha256').update('secretsalt').digest('base64'));
  const resumed=new Connection(()=>{},()=>{});const reconnect=resumed.connect('ws://localhost','',remembered);
  Socket.current.receive(0,{authentication:{salt:'salt',challenge:'next'}});
  assert.equal(Socket.current.sent[0].d.authentication,authenticate('secret','salt','next'));
  Socket.current.receive(2,{});await reconnect;resumed.close();
});

test('a new server salt rejects remembered credentials and requests a fresh login',async()=>{
  const connection=new Connection(()=>{},()=>{});
  const login=connection.connect('ws://localhost','',{salt:'old',secret:'cached'});
  Socket.current.receive(0,{authentication:{salt:'new',challenge:'challenge'}});
  await assert.rejects(login,/authentication/);assert.equal(Socket.current.sent.length,0);
});
test('authentication failures and OBS session revocations retain their close codes',async()=>{
  const connection=new Connection(()=>{},()=>{});const failure=connection.connect('ws://localhost','wrong');
  Socket.current.close(4009);await assert.rejects(failure,/authentication/);
  let code=0;
  const revoked=new Connection(()=>{},event=>{code=event.code;});const promise=revoked.connect('ws://localhost','secret');
  Socket.current.receive(0,{authentication:{salt:'salt',challenge:'challenge'}});Socket.current.receive(2,{});await promise;
  Socket.current.close(4011);assert.equal(code,4011);
});
