// SPDX-License-Identifier: MIT
// 仮想SCSI tab of the Web UI (served at "/vscsi.js", used by update_page.h):
// vhd68's SD card browser brought over - SCSI ID rows, the SDカード card
// (image volume creation, file rows with mount buttons and a "…" menu, drop
// zone uploads, イメージ作成, LAN連携 with remote browse), the image browser
// (/api/img/* like vhd68: Human68k and ISO9660 contents, upload/copy into,
// move/rename/delete, text view and edit) and SD-level text view/edit.
// Human68k names are Shift-JIS: JSON is parsed byte-exact (latin1), names
// decoded for display, and sent back percent-encoded byte by byte.
// Loaded after the page's own script, so $, st, esc, msg and `found` exist.
#pragma once
static const char s_VscsiJs[] = R"JS(
let vsDir="/",vsIds=[],vsFiles=[],vsAll=[];
const num=n=>Number(n).toLocaleString("en-US");
const vsJoin=n=>(vsDir==="/"?"":vsDir)+"/"+n;
const isImg=n=>/\.(hds|hdf|img|iso)$/i.test(n),isIso=n=>/\.iso$/i.test(n),isXdf=n=>/\.(xdf|zip|dim|vfb)$/i.test(n);
const textish=n=>/\.(txt|bat|sys|doc|cfg|ini|log)$/i.test(n);
const sleep=ms=>new Promise(x=>setTimeout(x,ms));
const bstr=buf=>{let s="";for(const b of new Uint8Array(buf))s+=String.fromCharCode(b);return s};
const sjis=s=>{try{return new TextDecoder("shift_jis").decode(new Uint8Array([...s].map(c=>c.charCodeAt(0)&0xFF)))}catch(e){return s}};
const dosT=ms=>{const d=new Date(ms);const y=d.getFullYear();if(y<1980)return 0;return (((y-1980)<<9|(d.getMonth()+1)<<5|d.getDate())<<16|(d.getHours()<<11|d.getMinutes()<<5|(d.getSeconds()>>1)))>>>0};
const rawEnc=s=>{let o="";for(const c of s){const v=c.charCodeAt(0);o+=(v>0x7E||v<=0x20||'%#?&+=/\\"\''.indexOf(c)>=0)?"%"+(v&0xFF).toString(16).padStart(2,"0").toUpperCase():c}return o};
let sjisMap=null;
function sjisEnc(str){if(!sjisMap){sjisMap={};const td=new TextDecoder("shift_jis");for(let a=0xA1;a<=0xDF;a++){try{const ch=td.decode(new Uint8Array([a]));if(ch.length===1&&ch!=="�")sjisMap[ch]=[a]}catch(e){}}
  for(let hi=0x81;hi<=0xEF;hi++){if(hi>0x9F&&hi<0xE0)continue;for(let lo=0x40;lo<=0xFC;lo++){if(lo===0x7F)continue;try{const ch=td.decode(new Uint8Array([hi,lo]));if(ch.length===1&&ch!=="�"&&sjisMap[ch]===undefined)sjisMap[ch]=[hi,lo]}catch(e){}}}}
 const out=[];let miss=0;for(const ch of str){const cc=ch.codePointAt(0);if(cc<0x80)out.push(cc);else if(sjisMap[ch])out.push(...sjisMap[ch]);else{out.push(0x3F);miss++}}return {bytes:new Uint8Array(out),miss}}
const rawOf=b=>{let r="";b.forEach(x=>{r+=String.fromCharCode(x)});return r};
function h68kSafeName(raw){raw=raw.replace(/ /g,"").replace(/[?*:;"<>|\\\/+,=\[\]]/g,"_");const dot=raw.lastIndexOf(".");let b=dot>0?raw.slice(0,dot):raw,x=dot>0?raw.slice(dot+1):"";b=b.slice(0,18);x=x.slice(0,3);if(!b)b="FILE";return x?b+"."+x:b}
async function api(m,u){try{return await(await fetch(u,{method:m})).json()}catch(e){return {ok:false,error:String(e)}}}
function vsTab(t){document.querySelectorAll('.tabs button').forEach(b=>b.classList.toggle('on',b.dataset.t===t));$("#tab-cpu").style.display=t==="cpu"?"":"none";$("#tab-scsi").style.display=t==="scsi"?"":"none";try{localStorage.setItem("vmpu68tab",t)}catch(e){}if(t==="scsi")vsRefresh()}
async function vsRefresh(){await vsStatus();try{const s=await(await fetch("/api/sd",{cache:"no-store"})).json();vsSd(s)}catch(e){}vsLs()}
async function vsStatus(){try{const j=await st();vsIds=j.ids||[];vsDev()}catch(e){}}
// ---- SCSI IDs ----
const mountedIdOf=p=>vsIds.findIndex(s=>s.mounted&&(s.path||"").toLowerCase()===p.toLowerCase());
function vsDev(){$("#vsdev").innerHTML=vsIds.map((s,i)=>`<div class="row"><span><span class="idlabel">ID ${i}</span>${s.mounted?`${esc(s.name)} <small>${num(s.mb)} MB${s.cd?" · CD-ROM":""}${s.wp?" · 書込み禁止":""}${s.direct===false?" · FatFs 経由(断片化)":""}</small>`:s.empty?'<small>イジェクト済み(リセットまで「媒体なし」と答えます)</small>':'<small>空き</small>'}</span><span>${s.mounted?`<button onclick="vsEjectAsk(${i})">イジェクト</button>`:""}</span></div>`).join("")}
function vsEjectAsk(i){const s=vsIds[i];if(confirm(`${s.name} をイジェクトしますか?\n(X68000がアクセス中でないことを確認してください)`))vsEject(i)}
async function vsEject(i){const j=await api("POST",`/api/eject?slot=${i}`);msg(j.ok?`ID ${i} をイジェクトしました`:"失敗");vsRefresh()}
// ---- SD card ----
function vsSd(s){$("#vssd").innerHTML=`<span>カード ${num(s.card_mb)} MB · 起動用 ${num(s.boot_mb)} MB · イメージ用 ${s.img?`${num(s.img_total_mb)} MB(空き ${num(s.img_free_mb)} MB)`:"未作成"}</span>`+(s.expandable?`<button onclick="vsExpand(${s.spare_mb})" style="margin:0">残りの ${num(s.spare_mb)} MB をイメージ用に初期化</button>`:s.img?"":`<small>イメージ用の領域を作れません(起動用の後ろに 256 MB 以上の空きが必要)。いまは起動用の /vscsi に置いています</small>`)}
async function vsExpand(m){if(!confirm(`SD カードの残り ${num(m)} MB にイメージ用の領域(FAT32)を作ります。起動用の領域と中身はそのままです。/vscsi にあるイメージは新しい領域へ移します。よろしいですか？`))return;msg("初期化しています...");const j=await api("POST","/api/sd/expand");msg(j.ok?"イメージ用の領域を作りました":"失敗: "+(j.error||""));vsRefresh()}
// ---- SD file list ----
const fsort=a=>(a||[]).sort((x,y)=>(y.d||0)-(x.d||0)||x.n.toLowerCase().localeCompare(y.n.toLowerCase()));
async function vsLs(){let j;try{j=await(await fetch("/api/ls?path="+encodeURIComponent(vsDir),{cache:"no-store"})).json()}catch(e){return}
 if(j.ok===false){$("#vsfiles").innerHTML=`<div class="row"><span class="fn">${esc(j.error||"読めません")}</span><span></span></div>`;return}
 vsAll=fsort(j.files);const showall=$("#vsshowall").checked;vsFiles=vsAll.filter(e=>showall||!e.n.startsWith("."));
 let h="";if(vsDir!=="/")h+='<div class="row"><span class="fn dir" onclick="vsUp()">../</span><span></span></div>';
 vsFiles.forEach((e,i)=>{if(e.d){h+=`<div class="row"><span class="fn dir" onclick="vsGo(vsJoin('${esc(e.n)}'))">${esc(e.n)}/</span><span><button onclick="vsMenu(event,${i})">…</button></span></div>`;return}
  const p=vsJoin(e.n);let btn="",kind="file";
  if(isImg(e.n)){kind="img";const mid=mountedIdOf(p);btn+=mid>=0?`<button class="on" onclick="vsEjectAsk(${mid})">マウント中 (ID ${mid})</button>`:`<button onclick="vsPick(${i})">${isIso(e.n)?"CDマウント":"マウント"}</button>`}else if(isXdf(e.n))kind="xdf";
  btn+=`<button onclick="vsMenu(event,${i})">…</button>`;
  const nm=textish(e.n)?`<span class="fn txt" title="内容を表示" onclick="sdView(${i})">${esc(e.n)}</span>`:kind==="img"?`<span class="fn txt" title="ブラウズ" onclick="imgOpen('${esc(p)}')">${esc(e.n)}</span>`:`<span class="fn">${esc(e.n)}</span>`;
  const badge=kind==="img"?` <small style="color:#8fb98f">[${isIso(e.n)?"CD":/\.hdf$/i.test(e.n)?"SASI":"SCSI"}]</small>`:"";
  h+=`<div class="row"><span class="fn" style="padding:0">${nm}${badge}</span><span><small>${num(e.s)}</small><span class="btns">${btn}</span></span></div>`});
 $("#vspath").textContent=vsDir;$("#vsfiles").innerHTML=h||'<div class="row"><span class="fn"><small>ファイルがありません</small></span><span></span></div>'}
function vsGo(d){vsDir=d||"/";vsLs()}
function vsUp(){vsDir=vsDir.replace(/\/[^/]+$/,"")||"/";vsLs()}
function vsMenu(ev,i){ev.stopPropagation();const f=vsFiles[i],m=$("#ctxmenu");let h="";
 if(!f.d){if(isImg(f.n))h+=`<button onclick="imgOpen('${esc(vsJoin(f.n))}')">ブラウズ</button><button onclick="vsXfer(${i})">転送してマウント (LAN)</button>`;
  else{if(textish(f.n))h+=`<button onclick="sdView(${i})">表示</button>`;h+=`<button onclick="inject('${esc(vsJoin(f.n))}')">ブラウズ中のフォルダにコピー</button>`;if(isXdf(f.n))h+=`<button onclick="vsXfer(${i})">転送してマウント (LAN)</button>`}
  h+=`<a class="btn" style="display:block;margin:1px 0;padding:5px 10px" href="/api/file?path=${encodeURIComponent(vsJoin(f.n))}" download="${esc(f.n)}">ダウンロード</a>`}
 h+=`<button onclick="vsRename(${i})">名前の変更</button><button class="warn" onclick="vsRm(${i})">削除</button>`;
 m.innerHTML=h;m.style.display="block";m.style.left=Math.min(ev.clientX,innerWidth-200)+"px";m.style.top=Math.min(ev.clientY,innerHeight-180)+"px"}
document.addEventListener("click",()=>{const m=$("#ctxmenu");if(m)m.style.display="none"});
document.addEventListener("keydown",e=>{if(e.key!=="Escape")return;const cm=$("#ctxmenu");if(cm.style.display==="block"){cm.style.display="none";return}if($("#vsmodal").style.display==="flex"){vsPkHide();return}if($("#imgmodal").style.display==="flex"){closeImg();return}for(const id of["#namemodal","#locmodal"]){const m=$(id);if(m&&m.style.display==="flex")m.style.display="none"}});
// a click on the dimmed backdrop closes the dialog (not the progress dialog: nothing to cancel there)
$("#vsmodal").addEventListener("click",e=>{if(e.target===$("#vsmodal"))vsPkHide()});
$("#imgmodal").addEventListener("click",e=>{if(e.target===$("#imgmodal"))closeImg()});
for(const id of["#namemodal","#locmodal"]){const m=$(id);if(m)m.addEventListener("click",e=>{if(e.target===m)m.style.display="none"})}
// ---- dialogs (pickbox / viewbox) ----
function vsPkHide(){$("#vsmodal").style.display="none";if($("#imgmodal").style.display!=="flex")document.body.style.overflow=""}
function vsPkShow(ttl,body,wide){const m=$("#vsmodal");m.innerHTML=`<div class="${wide?"viewbox":"pickbox"}"><div style="display:flex;justify-content:space-between;align-items:center"><h2 style="font-size:14px;margin:0;color:#9fd49f">${ttl}</h2><button class="x" onclick="vsPkHide()">&times;</button></div><div id="vspkbody" style="margin:8px 0 0;${wide?"overflow:auto;flex:1;display:flex;flex-direction:column":"max-height:60vh;overflow:auto"}">${body}</div></div>`;m.style.display="flex";document.body.style.overflow="hidden"}
function vsPick(fi){const f=vsFiles[fi],p=vsJoin(f.n),iso=isIso(f.n);let h=`<div class="meta">${iso?"CD-ROMとして":""}マウントするファイル:</div><div class="name" style="margin-bottom:8px">${esc(f.n)}</div><div class="meta">マウント先の SCSI ID を選択:</div>`;
 vsIds.forEach((s,i)=>{h+=s.mounted?`<button class="idbtn" disabled>ID ${i}&nbsp;&nbsp;<small>使用中: ${esc(s.name)}</small></button>`:`<button class="idbtn" onclick="vsMount(${i},'${esc(p)}')">ID ${i}&nbsp;&nbsp;<small>空き</small></button>`});
 h+=`<label class="chk" style="display:block;margin:8px 0 0"><input type="checkbox" id="vsro"${iso?" checked disabled":""}> 書込み禁止でマウントする</label><p style="margin-bottom:0"><button onclick="vsPkHide()">キャンセル</button></p>`;vsPkShow("マウント",h)}
async function vsMount(id,p){const ro=$("#vsro")&&$("#vsro").checked?1:0;vsPkHide();const j=await api("POST",`/api/mount?slot=${id}&path=${encodeURIComponent(p)}&ro=${ro}`);msg(j.ok?`ID ${id} に ${p.split("/").pop()} をマウントしました`:"失敗: "+(j.error||""));vsRefresh()}
async function vsRm(i){const f=vsFiles[i];if(!confirm(`${f.n} を削除しますか?`))return;const j=await api("POST","/api/file/rm?path="+encodeURIComponent(vsJoin(f.n)));msg(j.ok?"削除しました":"失敗: "+(j.error||""));vsLs()}
async function vsRename(i){const f=vsFiles[i];const n=prompt("新しい名前",f.n);if(!n||n===f.n)return;const j=await api("POST",`/api/file/rename?path=${encodeURIComponent(vsJoin(f.n))}&to=${encodeURIComponent(vsJoin(n))}`);msg(j.ok?"名前を変更しました":"失敗: "+(j.error||""));vsLs()}
async function vsMkdir(){const n=prompt("フォルダ名");if(!n)return;const j=await api("POST","/api/file/mkdir?path="+encodeURIComponent(vsJoin(n)));msg(j.ok?"作りました":"失敗: "+(j.error||""));vsLs()}
// ---- progress dialog ----
function progShow(name,title){$("#vsprogttl").textContent=title||"インポート";$("#vsprogname").textContent=name;$("#vsprogfill").style.width="0";$("#vsprogtext").textContent=(title||"インポート")+"を開始しています...";$("#vsprog").style.display="flex"}
function progUpd(b,t,verb){const v=verb||"インポート中";if(t){const p=Math.floor(b/t*100);$("#vsprogfill").style.width=p+"%";$("#vsprogtext").textContent=`${v}: ${num(b)} / ${num(t)} (${p}%)`}else $("#vsprogtext").textContent=`${v}: ${num(b)}`}
const progText=t=>$("#vsprogtext").textContent=t;
function progHide(){$("#vsprog").style.display="none"}
// ---- uploads (multipart, 512 KB parts) ----
async function putFile(path,f,label,t){progShow(label,"アップロード");const q="path="+encodeURIComponent(path)+(t?"&t="+t:""),CH=512*1024;let off=0,r={ok:false,error:"no data"};
 while(off<f.size||off===0){const n=Math.min(CH,f.size-off),last=off+n>=f.size?1:0;const fd=new FormData();fd.append("data",f.slice(off,off+n),"c");
  for(let k=0;;k++){try{r=await(await fetch(`/api/file/put?${q}&off=${off}&last=${last}`,{method:"POST",body:fd})).json();if(r.ok)break}catch(e){r={ok:false,error:String(e)}}if(k>=10){progHide();return r}await sleep(500)}
  off+=n;progUpd(off,f.size,"アップロード中");if(last)break}
 progHide();if(r.ok&&r.size!==f.size)r={ok:false,error:`大きさが合いません(${r.size} / ${f.size})`};return r}
async function uploadFile(f){if(vsAll.some(x=>x.n.toLowerCase()===f.name.toLowerCase())&&!confirm(`${f.name} は既にあります。上書きしますか？`))return false;
 const j=await putFile(vsJoin(f.name),f,f.name,dosT(f.lastModified));msg(j.ok?"アップロード完了: "+f.name:"失敗: "+(j.error||""));return j.ok}
async function vsUpload(){const f=$("#vsup").files[0];if(!f){msg("ファイルを選んでください");return}await uploadFile(f);$("#vsup").value="";vsLs()}
(()=>{const dz=$("#vsdrop");if(!dz)return;dz.addEventListener("dragover",e=>{e.preventDefault();dz.classList.add("drop")});dz.addEventListener("dragleave",()=>dz.classList.remove("drop"));
 dz.addEventListener("drop",async e=>{e.preventDefault();dz.classList.remove("drop");for(const f of e.dataTransfer.files)await uploadFile(f);vsLs()});
 window.addEventListener("dragover",e=>e.preventDefault());window.addEventListener("drop",e=>e.preventDefault())})();
// ---- SD text view / edit ----
async function sdView(i){const f=vsFiles[i],p=vsJoin(f.n);const r=await fetch("/api/file?path="+encodeURIComponent(p));if(!r.ok){msg("読み込みに失敗しました");return}
 let t=await r.text();if(t.length>262144){msg("ファイルが大きすぎます(256KBまで)");return}
 window._edEof=/\x1A\s*$/.test(t);window._edCrlf=/\r\n/.test(t);window._edTxt=t.replace(/\x1A[\s\x1A]*$/,"");
 vsPkShow(esc(f.n),`<pre id="edpre"></pre><p style="margin:0"><button onclick="sdEdit('${esc(p)}','${esc(f.n)}')">編集</button></p>`,true);$("#edpre").textContent=window._edTxt}
function sdEdit(p,n){vsPkShow(esc(n),`<textarea id="edta"></textarea><p style="margin:0"><button onclick="sdSave('${esc(p)}','${esc(n)}')">保存</button> <button onclick="vsPkHide()">キャンセル</button></p>`,true);$("#edta").value=window._edTxt}
async function sdSave(p,n){let body=$("#edta").value;if(window._edCrlf)body=body.replace(/\r?\n/g,"\r\n");if(window._edEof&&!/\x1A$/.test(body))body+="\x1A";
 const j=await putFile(p,new Blob([body]),n,dosT(Date.now()));msg(j.ok?"保存しました: "+n:"失敗: "+(j.error||""));if(j.ok)vsPkHide();vsLs()}
// ---- image creation ----
function vsNewDlg(){vsPkShow("イメージ作成",`<div class="meta">空のディスクイメージを作成します。作成後、X68000 側の FORMAT.X で装置初期化・フォーマットしてください。</div>
<div class="segrow"><span class="lbl">種類</span><label class="chk" style="margin:0"><input type="radio" name="nimgtype" value="scsi" checked onchange="newImgType()"> SCSI (.hds)</label><label class="chk"><input type="radio" name="nimgtype" value="sasi" onchange="newImgType()"> SASI (.hdf)</label></div>
<div class="segrow"><span class="lbl">ファイル名</span><input type="text" id="nimgname" value="NEW" style="flex:1"></div>
<div class="segrow" id="nimgscsi"><span class="lbl">容量 (MB)</span><input type="text" id="nimgmb" value="100" style="width:6em"> <small>1〜4090</small></div>
<div class="segrow" id="nimgsasi" style="display:none"><span class="lbl">容量</span><label class="chk" style="margin:0"><input type="radio" name="nsasimb" value="10441728" checked> 10 MB</label><label class="chk"><input type="radio" name="nsasimb" value="20748288"> 20 MB</label><label class="chk"><input type="radio" name="nsasimb" value="41496576"> 40 MB</label></div>
<div class="err" id="nimgerr"></div><p style="margin-bottom:0"><button onclick="newImgGo()">作成</button><button onclick="vsPkHide()">キャンセル</button></p>`)}
function newImgType(){const sasi=document.querySelector("input[name=nimgtype]:checked").value==="sasi";$("#nimgscsi").style.display=sasi?"none":"";$("#nimgsasi").style.display=sasi?"":"none"}
async function newImgGo(){const sasi=document.querySelector("input[name=nimgtype]:checked").value==="sasi";let name=($("#nimgname").value||"").trim().replace(/[\\\/:*?"<>|]/g,"");const err=t=>$("#nimgerr").textContent=t;
 if(!name){err("名前を入れてください");return}const ext=sasi?".hdf":".hds";if(!name.toLowerCase().endsWith(ext))name+=ext;let bytes;
 if(sasi)bytes=+document.querySelector("input[name=nsasimb]:checked").value;else{const mb=+$("#nimgmb").value;if(!(mb>=1&&mb<=4090)){err("容量は 1〜4090 MB で指定してください");return}bytes=mb*1048576}
 if(vsAll.some(x=>x.n.toLowerCase()===name.toLowerCase())){err("そのファイル名のイメージがすでに存在します");return}
 msg("作成中: "+name+" ...");const j=await api("POST",`/api/img/new?path=${encodeURIComponent(vsJoin(name))}&bytes=${bytes}`);if(!j.ok){err("エラー: "+(j.error||""));return}vsPkHide();msg("作成しました: "+name);vsLs()}
// ---- image browser (Human68k / ISO contents) ----
let curImg=null,curStack=[],imgFiles=[];
const curClu=()=>curStack.length?curStack[curStack.length-1].clu:0,curSz=()=>curStack.length?curStack[curStack.length-1].sz:0;
const imgGetPath=raw=>`/api/img/get?path=${encodeURIComponent(curImg)}&clu=${curClu()}&sz=${curSz()}&name=${rawEnc(raw)}`;
function imgOpen(p){curImg=p;curStack=[];imgRender()}
function imgDown(i){const e=imgFiles[i];curStack.push({name:sjis(e.n),clu:e.c,sz:e.s||0});imgRender()}
function imgUp(idx){curStack=curStack.slice(0,idx);imgRender()}
function closeImg(){curImg=null;$("#imgmodal").style.display="none";if($("#vsmodal").style.display!=="flex")document.body.style.overflow=""}
async function imgRender(){let j;try{const buf=await(await fetch(`/api/img/ls?path=${encodeURIComponent(curImg)}&clu=${curClu()}&sz=${curSz()}`,{cache:"no-store"})).arrayBuffer();j=JSON.parse(bstr(buf))}catch(e){msg("エラー: 一覧の解析に失敗");return}
 if(j.ok===false){msg("エラー: "+j.error);closeImg();return}
 imgFiles=fsort(j.files);const v=$("#imgview");let bc=esc(curImg.split("/").pop());curStack.forEach(s=>{bc+=" / "+esc(s.name)});
 let h=`<h2 style="display:flex;justify-content:space-between;align-items:center">イメージブラウザ <button class="x" onclick="closeImg()" title="閉じる">×</button></h2><div class="meta">${bc}${mountedIdOf(curImg)>=0?" <span style=\"color:#8fb98f\">(マウント中: 読み出しのみ)</span>":""}</div>`;
 if(curStack.length)h+=`<div class="row"><span class="fn dir" onclick="imgUp(${curStack.length-1})">../</span><span></span></div>`;
 imgFiles.forEach((e,i)=>{const disp=esc(sjis(e.n));
  if(e.d)h+=`<div class="row"><span class="fn dir" onclick="imgDown(${i})">${disp}/</span><span><button onclick="imgMenu(event,${i})">…</button></span></div>`;
  else{const nm=textish(disp)?`<span class="fn txt" title="内容を表示" onclick="imgViewText(${i})">${disp}</span>`:`<span class="fn">${disp}</span>`;
   h+=`<div class="row">${nm}<span><small>${num(e.s)}</small> <a class="btn" href="${imgGetPath(e.n)}" download="${disp}" title="ダウンロード">⤓</a> <button onclick="imgMenu(event,${i})">…</button></span></div>`}});
 h+=`<div style="display:flex;gap:10px;align-items:center;margin-top:10px"><div id="imgdrop" style="flex:1;border:1px dashed #4a5568;border-radius:6px;padding:10px 12px" ondragover="event.preventDefault();this.classList.add('drop')" ondragleave="this.classList.remove('drop')" ondrop="imgDrop(event)"><input type="file" id="imgup"> <button onclick="imgUpload()">アップロード</button><div class="meta" style="margin:6px 0 0">ここにファイルをドロップしても取り込めます</div></div><span style="white-space:nowrap"><button onclick="imgMkdir()" style="margin:0 6px 0 0">フォルダ作成</button><button onclick="vsLan()" style="margin:0">LAN連携</button></span></div>`;
 v.innerHTML=h;$("#imgmodal").style.display="flex";document.body.style.overflow="hidden"}
function imgMenu(ev,i){ev.stopPropagation();const e=imgFiles[i],dn=sjis(e.n),m=$("#ctxmenu");let h="";
 if(e.d){h+=`<button onclick="imgRename(${i})">名前の変更</button><button class="warn" onclick="imgRm(${i})">削除</button>`}
 else{if(textish(dn))h+=`<button onclick="imgViewText(${i})">内容を表示</button>`;if(isXdf(dn))h+=`<button onclick="vsXferUrl(imgGetPath(imgFiles[${i}].n),'${esc(dn)}')">転送してマウント (LAN)</button>`;h+=`<button onclick="imgMoveDlg(${i})">移動</button><button onclick="imgRename(${i})">名前の変更</button><button class="warn" onclick="imgRm(${i})">削除</button>`}
 m.innerHTML=h;m.style.display="block";m.style.left=Math.min(ev.clientX,innerWidth-200)+"px";m.style.top=Math.min(ev.clientY,innerHeight-120)+"px"}
// Runs fn() with the open image unmounted: asks first, ejects, always remounts afterwards.
async function imgUnmounted(fn,what){const img=curImg,id=mountedIdOf(img);if(id<0)return await fn();
 if(!confirm(`このイメージはマウント中のため、${what||"ファイルアップロード"}前にいったんイジェクトします。よろしいですか？`))return false;
 const j=await api("POST","/api/eject?slot="+id);if(!j.ok)return false;await vsStatus();
 try{return await fn()}finally{const m=await api("POST",`/api/mount?slot=${id}&path=${encodeURIComponent(img)}`);await vsStatus();if(m.ok)msg(`ID ${id} に再マウントしました`)}}
async function imgViewText(i){const e=imgFiles[i],dn=sjis(e.n);if(e.s>262144){msg("ファイルが大きすぎます(256KBまで)");return}
 const buf=await(await fetch(imgGetPath(e.n))).arrayBuffer();let t=sjis(bstr(buf));window._edEof=/\x1A\s*$/.test(t);window._edTxt=t.replace(/\x1A[\s\x1A]*$/,"");
 vsPkShow(esc(dn),`<pre id="edpre"></pre><p style="margin:0"><button onclick="imgEditMode(${i})">編集</button></p>`,true);$("#edpre").textContent=window._edTxt}
function imgEditMode(i){const dn=sjis(imgFiles[i].n);vsPkShow(esc(dn),`<textarea id="edta"></textarea><p style="margin:0"><button onclick="imgSave(${i})">保存</button> <button onclick="vsPkHide()">キャンセル</button></p>`,true);$("#edta").value=window._edTxt}
async function imgSave(i){await imgUnmounted(()=>imgSaveDo(i),"ファイル保存")}
async function imgSaveDo(i){const e=imgFiles[i],dn=sjis(e.n);let txt=$("#edta").value.replace(/\r?\n/g,"\r\n");if(window._edEof&&!/\x1A$/.test(txt))txt+="\x1A";
 const enc=sjisEnc(txt);if(enc.miss&&!confirm(`Shift-JISに変換できない文字が ${enc.miss} 個あり「?」に置き換わります。続けますか?`))return;
 msg("書き込み中...");const tmp="/.__edit.tmp";let j=await putFile(tmp,new Blob([enc.bytes]),dn,dosT(Date.now()));if(!j.ok){msg("一時保存に失敗: "+(j.error||""));return}
 j=await api("POST",`/api/img/inject?img=${encodeURIComponent(curImg)}&src=${encodeURIComponent(tmp)}&name=${rawEnc(e.n)}&dir=${curClu()}&ow=1`);await api("POST","/api/file/rm?path="+encodeURIComponent(tmp));
 if(j.ok){msg("保存しました: "+dn);vsPkHide();imgRender()}else msg("失敗: "+(j.error||""))}
async function imgMkdir(){const n=prompt("フォルダ名(18文字まで)");if(!n)return;const enc=sjisEnc(n);if(enc.miss){msg("フォルダ名に使えない文字が含まれています");return}
 await imgUnmounted(async()=>{const j=await api("POST",`/api/img/mkdir?img=${encodeURIComponent(curImg)}&name=${rawEnc(rawOf(enc.bytes))}&dir=${curClu()}&t=${dosT(Date.now())}`);if(j.ok){msg("フォルダを作成しました: "+n);await imgRender()}else msg("失敗: "+(j.error||""))},"フォルダ作成")}
async function imgRename(i){const e=imgFiles[i],dn=sjis(e.n);const n=prompt("新しい名前",dn);if(!n||n===dn)return;const enc=sjisEnc(h68kSafeName(n));if(enc.miss){msg("名前に使えない文字が含まれています");return}
 await imgUnmounted(async()=>{const j=await api("POST",`/api/img/rename?img=${encodeURIComponent(curImg)}&name=${rawEnc(e.n)}&to=${rawEnc(rawOf(enc.bytes))}&dir=${curClu()}`);if(j.ok){msg("名前を変更しました");await imgRender()}else msg("失敗: "+(j.error||""))},"名前の変更")}
function imgMoveDlg(i){const dn=sjis(imgFiles[i].n),dirs=imgFiles.map((e,j)=>({e,j})).filter(x=>x.e.d),hasParent=curStack.length>0;if(!dirs.length&&!hasParent){msg("移動先になるフォルダがありません");return}
 let h=`<div class="meta">移動するファイル:</div><div class="name" style="margin-bottom:8px">${esc(dn)}</div><div class="meta">移動先を選択:</div>`;
 if(hasParent){const pclu=curStack.length>1?curStack[curStack.length-2].clu:0;h+=`<button class="idbtn" onclick="imgMove(${i},${pclu})">../(親フォルダ)</button>`}
 dirs.forEach(x=>{h+=`<button class="idbtn" onclick="imgMove(${i},${x.e.c})">${esc(sjis(x.e.n))}/</button>`});h+=`<p style="margin-bottom:0"><button onclick="vsPkHide()">キャンセル</button></p>`;vsPkShow("移動",h)}
async function imgMove(i,toClu){vsPkHide();const e=imgFiles[i],dn=sjis(e.n);await imgUnmounted(async()=>{const j=await api("POST",`/api/img/move?img=${encodeURIComponent(curImg)}&name=${rawEnc(e.n)}&from=${curClu()}&to=${toClu}`);if(j.ok){msg("移動しました: "+dn);await imgRender()}else msg("失敗: "+(j.error||""))},"移動")}
async function imgRm(i){const e=imgFiles[i],dn=sjis(e.n);if(!confirm(dn+" をイメージから削除しますか?"))return;await imgUnmounted(async()=>{const j=await api("POST",`/api/img/rm?img=${encodeURIComponent(curImg)}&name=${rawEnc(e.n)}&dir=${curClu()}`);if(j.ok){msg("削除しました: "+dn);await imgRender()}else msg("失敗: "+(j.error||""))},"削除")}
// SD file -> the folder open in the image browser
async function inject(src){const img=curImg,dir=curClu();if(!img){msg("先にコピー先のイメージをブラウズで開いてください");return}await imgUnmounted(()=>injectDo(src,img,dir),"コピー")}
async function injectDo(src,img,dir){const raw=h68kSafeName(rawOf(sjisEnc(src.split("/").pop().toUpperCase()).bytes)),disp=sjis(raw);
 const base=`/api/img/inject?img=${encodeURIComponent(img)}&src=${encodeURIComponent(src)}&name=${rawEnc(raw)}&dir=${dir}`;msg("コピー中: "+disp);let j=await api("POST",base);
 if(!j.ok&&j.error==="already exists"){if(!confirm(disp+" は既にあります。上書きしますか?"))return;msg("上書きコピー中: "+disp);j=await api("POST",base+"&ow=1")}
 if(j.ok){msg("コピーしました: "+disp);imgRender()}else msg("エラー: "+(j.error||""))}
async function imgUpload(){const f=$("#imgup").files[0];if(!f){msg("ファイルを選んでください");return}await imgUnmounted(()=>imgUploadFile(f))}
async function imgDrop(e){e.preventDefault();e.currentTarget.classList.remove("drop");const files=[...e.dataTransfer.files];await imgUnmounted(async()=>{for(const f of files)if(!await imgUploadFile(f))break})}
async function imgUploadFile(f){if(mountedIdOf(curImg)>=0){msg("マウント中のイメージには書き込めません。先にイジェクトしてください");return false}
 const safe=h68kSafeName(f.name),enc=sjisEnc(safe);if(enc.miss){msg("このファイル名はイメージ内で使えません: "+f.name);return false}const raw=rawOf(enc.bytes);
 if((imgFiles||[]).some(e=>!e.d&&sjis(e.n).toLowerCase()===safe.toLowerCase())&&!confirm(safe+" は既にあります。上書きしますか?"))return true;
 const tmp="/.__upload.tmp";let j=await putFile(tmp,f,safe,dosT(f.lastModified));if(!j.ok){msg("一時保存に失敗: "+(j.error||""));return false}
 msg("イメージへ取り込み中...");j=await api("POST",`/api/img/inject?img=${encodeURIComponent(curImg)}&src=${encodeURIComponent(tmp)}&name=${rawEnc(raw)}&dir=${curClu()}&ow=1&t=${dosT(f.lastModified)}`);await api("POST","/api/file/rm?path="+encodeURIComponent(tmp));
 if(j.ok){msg("取り込みました: "+safe);await imgRender()}else msg("取り込みに失敗: "+(j.error||""));return !!j.ok}
// ---- LAN: peers, remote browse (SD and inside images), import to SD or into the open image folder, transfer to a peer ----
const peersOf=()=>Object.values(typeof found==="object"?found:{}).filter(p=>/^v[hfm]/.test(p.dev)).sort((a,b)=>+a.ip.split(".")[3]-+b.ip.split(".")[3]);
const pUrl=p=>`http://${p.ip}${p.port&&p.port!=80?":"+p.port:""}`;
const peerLabel=p=>(p.name?p.name+"(":"")+p.dev+(p.ver?" v"+p.ver:"")+" @"+p.ip+(p.name?")":"");
function rbDest(){if($("#imgmodal").style.display==="flex"&&curImg&&mountedIdOf(curImg)<0)return {img:curImg,dir:curClu()};return {img:null}}
function vsLan(){const ps=peersOf(),d=rbDest();vsPkShow("LAN連携",`<div class="meta">同じネットワーク内の vhd68 / vfd68 / vmpu68 からファイルを取り込めます</div><div class="meta">取り込み先: ${d.img?"イメージ "+esc(d.img.split("/").pop())+" の開いているフォルダ":"SDフォルダ "+esc(vsDir)}</div>`+
 (ps.length?ps.map(p=>`<div class="peer"><span class="pl" onclick="rbOpen('${p.ip}')" title="クリックでブラウズ"><span class="tag ${p.dev}">${p.dev}</span>${esc(p.name||"")} <span class="meta">${p.ver?"v"+esc(p.ver):""} @${p.ip}</span></span><span><button onclick="rbOpen('${p.ip}')">ブラウズ</button><button onclick="window.open('${pUrl(p)}/')">Web UI</button></span></div>`).join(""):'<div class="meta">vhd68 / vfd68 / vmpu68 が見つかりません(同じLANで起動していますか？)</div>'))}
let rb={};
const rbClu=()=>rb.stack.length?rb.stack[rb.stack.length-1].clu:0,rbSz=()=>rb.stack.length?rb.stack[rb.stack.length-1].sz:0;
function rbOpen(ip){const p=found[ip];if(!p)return;rb={base:pUrl(p),ip:p.ip,port:p.port||80,dev:p.dev,dir:"/",img:null,stack:[]};rbRender()}
async function rbRender(){let body="";
 try{if(!rb.img){const j=await(await fetch(rb.base+"/api/ls?dir="+encodeURIComponent(rb.dir)+"&path="+encodeURIComponent(rb.dir),{cache:"no-store",signal:AbortSignal.timeout(6000)})).json();
   if(rb.dir!=="/")body+='<div class="row"><span class="fn dir" onclick="rbUp()">../</span><span></span></div>';
   for(const e of fsort(j.files)){if(e.n.startsWith("."))continue;
    if(e.d)body+=`<div class="row"><span class="fn dir" onclick="rbCd('${esc(e.n)}')">${esc(e.n)}/</span><span></span></div>`;
    else{const img=rb.dev!=="vfd68"&&isImg(e.n);const nm=img?`<span class="fn txt" title="中を見る" onclick="rbImg('${esc(e.n)}')">${esc(e.n)}</span>`:`<span class="fn">${esc(e.n)}</span>`;
     body+=`<div class="row">${nm}<span><small>${num(e.s)}</small>${img?`<button style="margin-left:12px" onclick="rbImg('${esc(e.n)}')">中を見る</button>`:""}<button style="margin-left:${img?4:12}px" onclick="rbFetchSd('${esc(e.n)}',${e.s||0})">インポート</button></span></div>`}}}
  else{const buf=await(await fetch(rb.base+`/api/img/ls?path=${encodeURIComponent(rb.img)}&clu=${rbClu()}&sz=${rbSz()}`,{cache:"no-store",signal:AbortSignal.timeout(6000)})).arrayBuffer();const j=JSON.parse(bstr(buf));
   if(j.ok===false){msg("エラー: "+j.error);rb.img=null;rbRender();return}rb.files=fsort(j.files);
   body+='<div class="row"><span class="fn dir" onclick="rbImgUp()">../</span><span></span></div>';
   rb.files.forEach((e,i)=>{const disp=esc(sjis(e.n));if(e.d)body+=`<div class="row"><span class="fn dir" onclick="rbImgDown(${i})">${disp}/</span><span></span></div>`;
    else body+=`<div class="row"><span class="fn">${disp}</span><span><small>${num(e.s)}</small><button style="margin-left:12px" onclick="rbFetchImg(${i})">インポート</button></span></div>`})}}
 catch(e){body='<div class="meta">相手に接続できませんでした</div>'}
 const W=s=>`<span style="color:#e8e8e8">${s}</span>`;let bc=W(esc(rb.ip))+(rb.img?" : "+W(esc(rb.img.split("/").pop())):" "+W("SDカード"));
 if(!rb.img&&rb.dir!=="/")rb.dir.split("/").filter(Boolean).forEach(p=>{bc+=" / "+W(esc(p))});rb.stack.forEach(s=>{bc+=" / "+W(esc(s.name))});
 vsPkShow("リモートブラウズ",`<div class="meta">${bc}</div><div id="rbview" style="overflow:auto;flex:1">${body}</div><p style="margin:8px 0 0"><button onclick="vsLan()">機器一覧へ</button></p>`,true)}
function rbCd(n){rb.dir=(rb.dir==="/"?"":rb.dir)+"/"+n;rbRender()}
function rbUp(){rb.dir=rb.dir.replace(/\/[^/]+$/,"")||"/";rbRender()}
function rbImg(n){rb.img=(rb.dir==="/"?"":rb.dir)+"/"+n;rb.stack=[];rbRender()}
function rbImgDown(i){const e=rb.files[i];rb.stack.push({name:sjis(e.n),clu:e.c,sz:e.s});rbRender()}
function rbImgUp(){if(rb.stack.length)rb.stack.pop();else rb.img=null;rbRender()}
function rbSizeOk(n,sz){if(sz>32*1048576){const min=Math.ceil(sz/102400/60);const t=min>=60?Math.round(min/60*10)/10+"時間":min+"分";return confirm(`${n} は ${num(sz)} バイトあります。\n機器間転送では約${t}かかる見込みです。続行しますか?`)}return true}
function rbFetchSd(n,sz){if(!rbSizeOk(n,sz))return;if(!rbDest().img&&vsAll.some(x=>x.n.toLowerCase()===n.toLowerCase())&&!confirm(n+" はSDカードに既にあります。上書きしますか?"))return;rbFetch("/api/file?path="+encodeURIComponent((rb.dir==="/"?"":rb.dir)+"/"+n),n)}
function rbFetchImg(i){const e=rb.files[i],dn=sjis(e.n);if(!rbSizeOk(dn,e.s||0))return;rbFetch(`/api/img/get?path=${encodeURIComponent(rb.img)}&clu=${rbClu()}&sz=${rbSz()}&name=${rawEnc(e.n)}`,dn.replace(/[\\\/:*?"<>|]/g,"_"))}
async function rbFetch(rpath,name){const d=rbDest();let q;
 if(d.img){const safe=h68kSafeName(name),enc=sjisEnc(safe);if(enc.miss){msg("この名前はイメージ内で使えません: "+name);return}
  if((imgFiles||[]).some(e=>!e.d&&sjis(e.n).toLowerCase()===safe.toLowerCase())&&!confirm(safe+" はインポート先に既にあります。上書きしますか?"))return;
  q=`/api/fetch?ip=${rb.ip}&port=${rb.port}&path=${encodeURIComponent(rpath)}&img=${encodeURIComponent(d.img)}&dir=${d.dir}&name=${rawEnc(rawOf(enc.bytes))}&t=${dosT(Date.now())}`;name=safe}
 else q=`/api/fetch?ip=${rb.ip}&port=${rb.port}&path=${encodeURIComponent(rpath)}&dst=${encodeURIComponent(vsJoin(name))}`;
 const j=await api("POST",q);if(!j.ok){msg("失敗: "+(j.error||""));return}vsPkHide();progShow(name);
 for(;;){await sleep(500);let s;try{s=await(await fetch("/api/fetch",{cache:"no-store"})).json()}catch(e){continue}
  if(s.state==="running")progUpd(s.bytes,s.total);else if(s.state==="done"){progHide();msg("インポート完了: "+name);vsLs();if(rbDest().img)imgRender();break}else if(s.state==="error"){progHide();msg("インポート失敗: "+s.error);break}else{progHide();break}}}
// send a file (ours) to a peer: vfd68 mounts via fetch&slot, vhd68/vmpu68 via /api/mount after the pull
function vsXfer(i){const f=vsFiles[i];vsXferUrl("/api/file?path="+encodeURIComponent(vsJoin(f.n)),f.n)}
async function vsXferUrl(rurl,name){const ps=peersOf();if(!ps.length){msg("転送先 (vhd68 / vfd68 / vmpu68) が見つかりません");return}
 vsPkShow(name+" の転送先",'<div class="meta">確認中...</div>');
 const infos=await Promise.all(ps.map(p=>Promise.race([fetch(pUrl(p)+"/api/status",{cache:"no-store"}).then(r=>r.json()),new Promise(r=>setTimeout(()=>r(null),1500))]).catch(()=>null)));
 let h=`<div class="meta">転送してマウントするファイル:</div><div class="name" style="margin-bottom:8px">${esc(name)}</div><div class="meta">転送先を選択:</div>`;
 ps.forEach((p,i)=>{const j=infos[i]||{};let btns="";
  if(p.dev==="vfd68"){const d=j.xbar===false?["2","3"]:["0","1"];btns=`<button onclick="vsPkHide();sendToPeer('${p.ip}','${rurl}','${esc(name)}',0,'ドライブ ${d[0]}')">ドライブ ${d[0]}</button><button onclick="vsPkHide();sendToPeer('${p.ip}','${rurl}','${esc(name)}',1,'ドライブ ${d[1]}')">ドライブ ${d[1]}</button>`}
  else{const sl=j.ids||[];btns=`<select id="xsl-${p.ip.replace(/\./g,"_")}"><option value="">転送のみ</option>${sl.map((s,k)=>`<option value="${k}">ID ${k}${s.mounted?" ("+esc(s.name||"")+")":""}</option>`).join("")}</select> <button onclick="vsPkHide();sendToPeer('${p.ip}','${rurl}','${esc(name)}',(document.getElementById('xsl-${p.ip.replace(/\./g,"_")}')||{}).value,'')">転送</button>`}
  h+=`<div style="display:flex;justify-content:space-between;align-items:center;padding:4px 0"><span><span class="tag ${p.dev}">${p.dev}</span>${esc(peerLabel(p))}</span><span style="flex-shrink:0;margin-left:10px">${btns}</span></div>`});
 h+=`<p style="margin-bottom:0"><button onclick="vsPkHide()">キャンセル</button></p>`;vsPkShow(name+" の転送先",h)}
async function sendToPeer(ip,rurl,name,slot,disp){const p=found[ip];if(!p)return;const base=pUrl(p),dst=p.dev==="vfd68"?"/.lan."+name:"/"+name,hasSlot=slot!==""&&slot!==undefined&&slot!==null;
 const u=base+`/api/fetch?ip=${location.hostname}&port=${location.port||80}&path=${encodeURIComponent(rurl)}&dst=${encodeURIComponent(dst)}`+(p.dev==="vfd68"&&hasSlot?`&slot=${slot}&from=${encodeURIComponent("vmpu68 @"+location.hostname)}`:"");
 try{const j=await(await fetch(u,{method:"POST"})).json();if(!j.ok){msg("失敗: "+(j.error||""));return}}catch(e){msg(p.dev+" に接続できません");return}
 progShow(name+" → "+peerLabel(p),"転送");let fails=0;
 for(;;){await sleep(500);let s;try{s=await(await fetch(base+"/api/fetch",{cache:"no-store"})).json();fails=0}catch(e){if(++fails>=30){progHide();msg(p.dev+" と通信できません");return}continue}
  if(s.state==="running"||s.state==="run"){progUpd(s.bytes??s.done??0,s.total,"転送中");continue}progHide();
  if(s.state==="done"){if(p.dev!=="vfd68"&&hasSlot){const m=await api("POST",base+`/api/mount?slot=${slot}&path=${encodeURIComponent(dst)}`);msg(m.ok?`転送完了(${p.dev} の ID ${slot} にマウント)`:"転送しましたがマウントできません: "+(m.error||""))}else msg("転送完了"+(disp?"("+disp+"にマウント)":""))}
  else if(s.state==="error")msg("転送失敗: "+s.error);else msg("転送状態が確認できません。"+p.dev+" のWeb UIで確認してください");break}}
(()=>{let t="cpu";try{t=localStorage.getItem("vmpu68tab")||"cpu"}catch(e){}vsTab(t)})();
)JS";
