// SPDX-License-Identifier: MIT
// Browser front end (served at "/"), styled after the vhd68/vfd68 Web UI:
// 情報 / システム / 画面 / ファームウェア / 近くの機器 cards.  Self-contained
// except for the screen viewer (/screen.js, screen_js.h): uploads the .vpk
// in 8 KB hex chunks through /api/put, starts the install with /api/update
// and follows /api/status until the board is back.
// Must stay well below the response buffer (MAX_CONTENT_SIZE, 64 KB).
static const char s_UpdatePage[] = R"HTML(<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vmpu68</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#14161a;color:#e8e8e8}
header{padding:10px 16px;background:#20242c;display:flex;justify-content:space-between;align-items:baseline}
header h1{font-size:18px;margin:0;color:#d8a8ff}
#mock,small{font-size:12px;color:#9aa}
main{max-width:720px;margin:0 auto;padding:12px}
.card{background:#20242c;border-radius:8px;padding:12px 14px;margin:10px 0}
.card h2{font-size:14px;margin:0 0 8px;color:#9fd49f}
.meta{font-size:12px;color:#9aa;margin:4px 0}
button{background:#31405a;color:#e8e8e8;border:0;border-radius:5px;padding:5px 10px;margin:2px 4px 2px 0;cursor:pointer;font-size:13px}
button:hover{background:#3f5378}button.warn{background:#5a3131}button:disabled{opacity:.4;cursor:default}button:disabled:hover{background:#31405a}
button.x{background:none;border:0;color:#9aa;font-size:22px;line-height:1;padding:0 4px;margin:0}
input[type=file]{font-size:12px}
input[type=text]{background:#14161a;color:#e8e8e8;border:1px solid #2a2f38;border-radius:5px;padding:5px 8px;font-size:13px;font-family:ui-monospace,monospace}
input[type=file]::file-selector-button{background:#31405a;color:#e8e8e8;border:0;border-radius:5px;padding:5px 10px;margin-right:8px;cursor:pointer;font-size:13px}
label.chk{font-size:12px;color:#9aa;margin-left:8px}
button.ico{margin:0;padding:5px 7px;line-height:0;display:inline-flex;align-items:center}button.ico:disabled{opacity:.4;cursor:default}
.seg{display:inline-flex;border:1px solid #3f5378;border-radius:6px;overflow:hidden;vertical-align:middle;margin:2px 8px 2px 0}.seg button{margin:0;border-radius:0;white-space:nowrap;background:#20242c;color:#9aa;padding:5px 12px;border-right:1px solid #3f5378}.seg button:last-child{border-right:0}.seg button:hover{background:#2a3446;color:#e8e8e8}.seg button.on{background:#31405a;color:#fff;font-weight:600}.segrow{display:flex;align-items:center;gap:6px;margin:6px 0;font-size:13px}.segrow .lbl{color:#9aa;min-width:6em}
#info{display:grid;grid-template-columns:max-content 1fr;gap:2px 16px;font-size:13px}#info span:nth-child(odd){color:#9aa}
#peers .peer{display:flex;justify-content:space-between;align-items:center;padding:3px 0}
#peers .tag{display:inline-block;font-size:11px;font-weight:700;border-radius:4px;padding:1px 6px;margin-right:8px;min-width:44px;text-align:center}
.tag.vhd68{background:#274a2b;color:#9fd49f}.tag.vfd68{background:#27394a;color:#7ec8ff}.tag.vmpu68{background:#3d2f4a;color:#d8a8ff}.tag.x{background:#4a3a27;color:#d4b88f}
#progmodal{position:fixed;inset:0;background:#000a;display:none;align-items:center;justify-content:center;z-index:24}
.pickbox{background:#20242c;border-radius:8px;padding:14px;width:min(420px,90vw)}
#progbar{height:10px;background:#14161a;border:1px solid #2a2f38;border-radius:5px;overflow:hidden;margin:10px 0 6px}
#progfill{height:100%;width:0;background:#7ec8ff;transition:width .3s}
#proglog{font:12px/1.5 ui-monospace,monospace;color:#9aa;white-space:pre-wrap;max-height:160px;overflow:auto}
.err{color:#ff7b72}.ok{color:#7ee787}
#msg{position:fixed;bottom:12px;left:50%;transform:translateX(-50%);background:#333c;padding:8px 16px;border-radius:6px;display:none;z-index:30}
</style></head><body>
<header><h1 id="ttl">vmpu68</h1><span id="mock"></span></header>
<main>
<div class="card"><h2>情報</h2><div id="info"></div></div>
<div style="display:flex;gap:10px;align-items:stretch;flex-wrap:wrap">
<div class="card" style="flex:1 1 400px;margin:10px 0 0"><h2>MPU 設定</h2>
<div class="segrow"><span class="lbl">速度</span><span class="seg" id="segspd"><button data-v="10" onclick="cfgSet('mhz',10)">10 MHz 相当</button><button data-v="16" onclick="cfgSet('mhz',16)">16 MHz 相当</button><button data-v="24" onclick="cfgSet('mhz',24)">24 MHz 相当</button><button data-v="0" id="segmax" onclick="cfgSet('mhz',0)">Max</button></span></div>
<div class="segrow"><span class="lbl">JIT</span><span class="seg" id="segjit"><button data-v="1" onclick="cfgSet('jit',1)">ON</button><button data-v="0" onclick="cfgSet('jit',0)">OFF</button></span></div>
<div class="segrow"><span class="lbl">メイン RAM</span><span class="seg" id="segwb"><button data-v="1" onclick="cfgSet('wb',1)">ライトバック</button><button data-v="0" onclick="cfgSet('wb',0)">ライトスルー</button></span></div>
<div class="segrow" id="rowsmi" style="display:none;align-items:flex-start"><span class="lbl" style="padding-top:6px">SMI 転送</span><span><span class="seg" id="segsmi"><button data-v="1" onclick="cfgSet('smi',1)">ON</button><button data-v="0" onclick="cfgSet('smi',0)">OFF</button></span><div class="meta" style="margin:0 0 4px">Raspberry Pi と FPGA の間の転送に SMI(SoC の並列バス機能)を使います(V2.0 以降の基板)</div></span></div>
<div class="meta" id="cfginfo"></div></div>
<div class="card" style="flex:1 1 200px;margin:10px 0 0"><h2>システム設定</h2>
<div class="segrow" style="align-items:flex-start"><span class="lbl" style="padding-top:6px">起動画面</span><span><span class="seg" id="segsb"><button data-v="1" onclick="cfgSet('sramboot',1)">ON</button><button data-v="0" onclick="cfgSet('sramboot',0)">OFF</button></span><div class="meta" style="margin:0 0 4px">ONにすると起動時にマシン情報を表示します</div></span></div>
<button onclick="nameDlg()" title="Web UI や近くの機器一覧に表示される名前を設定します" style="display:block;width:100%;margin:10px 0 2px;white-space:nowrap">ホスト名を設定</button>
<button onclick="locDlg()" title="VMPU68 CORE の LED を白く点滅させて、どの基板かを示します" style="display:block;width:100%;margin:6px 0 2px;white-space:nowrap">この機器を探す</button>
<button onclick="doReboot()" title="VMPU68 と X68000 を再起動します" style="display:block;width:100%;margin:6px 0 2px;white-space:nowrap">VMPU68 と X68000 を再起動</button>
</div>
</div>
<div class="card"><h2 style="display:flex;justify-content:space-between;align-items:center">画面 <span style="display:inline-flex;gap:6px"><button id="scrbtn" class="ico" title="画面を読み取って更新 (R キー)" aria-label="更新"><svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 12a8 8 0 1 1-2.34-5.66"/><polyline points="20 3 20 9 14 9"/></svg></button><button id="scrdl" class="ico" title="画像 (PNG) をダウンロード (D キー)" aria-label="画像を保存" disabled><svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v11"/><polyline points="7 10 12 15 17 10"/><path d="M4 17v2a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-2"/></svg></button></span></h2>
<canvas id="scr" style="width:100%;display:block;background:#000;border-radius:4px;image-rendering:pixelated"></canvas>
<div class="meta" id="scrinfo"></div>
<div style="display:flex;gap:6px;align-items:center;margin-top:18px"><input type="text" id="scrkey" placeholder="キーを送信" style="flex:1;min-width:0"><label class="chk" style="margin:0;white-space:nowrap"><input type="checkbox" id="scrent" checked> 最後に Enter を追加する</label><label class="chk" style="margin:0;white-space:nowrap"><input type="checkbox" id="scrauto" checked> 3秒後に画面更新</label><button id="scrsend" style="margin:0">送信</button></div>
<div class="meta" style="margin-top:4px">^C や ^[ で制御文字を送信できます。[k] でこの欄にフォーカスを当て、[esc] でフォーカスを外せます。</div></div>
<div class="card"><h2>ファームウェア</h2>
<div class="meta" id="fwinfo">パッケージ (.vpk) を選んでください</div>
<p><input type="file" id="fwfile" accept=".vpk" onchange="fwPick()">
<button class="warn" id="fwapply" onclick="fwApply()" disabled>適用+再起動</button><br>
<label class="chk" style="display:block;margin:6px 0 0"><input type="checkbox" id="force"> 上書き更新 (同じバージョンでも書き換える)</label>
<label class="chk" style="display:block;margin:4px 0 0"><input type="checkbox" id="trymode" checked> お試し起動 (問題なく20秒動けば確定、そうでなければ前のバージョンに戻す)</label></p>
</div>
<div class="card"><h2 style="display:flex;justify-content:space-between;align-items:center">近くの機器 <button onclick="scan()" id="sc" class="ico" title="再スキャン (同じ /24 を HTTP で探す)" aria-label="再スキャン"><svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 12a8 8 0 1 1-2.34-5.66"/><polyline points="20 3 20 9 14 9"/></svg></button></h2>
<div id="peers"><span class="meta">...</span></div></div>
</main>
<div id="msg"></div>
<div id="namemodal" style="position:fixed;inset:0;background:#000a;display:none;align-items:center;justify-content:center;z-index:25"><div class="pickbox"><div style="display:flex;justify-content:space-between;align-items:center"><h2 style="font-size:14px;margin:0;color:#9fd49f">ホスト名を設定</h2><button class="x" onclick="nameHide()">&times;</button></div>
<div class="meta" style="margin:8px 0 4px">Web UI のヘッダと、近くの機器(vhd68/vfd68)の一覧に表示される名前です。31 バイトまで(日本語は 1 文字 3 バイト)。</div>
<input type="text" id="namein" maxlength="31" placeholder="X68000" style="width:100%;box-sizing:border-box" onkeydown="if(event.key==='Enter')nameSave();if(event.key==='Escape')nameHide()">
<div style="display:flex;gap:8px;justify-content:flex-end;margin-top:10px"><button onclick="nameHide()">キャンセル</button><button id="nameok" onclick="nameSave()">設定</button></div></div></div>
<div id="locmodal" style="position:fixed;inset:0;background:#000a;display:none;align-items:center;justify-content:center;z-index:25"><div class="pickbox"><div style="display:flex;justify-content:space-between;align-items:center"><h2 style="font-size:14px;margin:0;color:#9fd49f">この機器を探す</h2><button class="x" onclick="locHide()">&times;</button></div>
<div style="margin:12px 0 4px">VMPU68 CORE の LED を白に点滅させます(10 秒間)。</div>
<div style="display:flex;gap:8px;justify-content:flex-end;margin-top:10px"><button onclick="locHide()">キャンセル</button><button onclick="locGo()">OK</button></div></div></div>
<div id="progmodal"><div class="pickbox"><div style="display:flex;justify-content:space-between;align-items:center"><h2 style="font-size:14px;margin:0;color:#9fd49f">ファームウェア更新</h2><button class="x" id="progx" onclick="progHide()" disabled>&times;</button></div><div id="progbar"><div id="progfill"></div></div><div id="proglog"></div></div></div>
<script>
const $=s=>document.querySelector(s);

function setSeg(j){try{if(j.mhz_limit!==undefined){const v=[10,16,24].includes(j.mhz_limit)?j.mhz_limit:0;document.querySelectorAll('#segspd button').forEach(b=>b.classList.toggle('on',+b.dataset.v===v))}if(j.wb!==undefined){document.querySelectorAll('#segwb button').forEach(b=>b.classList.toggle('on',+b.dataset.v===(j.wb?1:0)))}if(j.jit!==undefined){document.querySelectorAll('#segjit button').forEach(b=>b.classList.toggle('on',+b.dataset.v===(j.jit?1:0)))}if(j.sramboot!==undefined){document.querySelectorAll('#segsb button').forEach(b=>b.classList.toggle('on',+b.dataset.v===(j.sramboot?1:0)))}if(j.board!==undefined){$("#rowsmi").style.display=j.board===2?"":"none"}if(j.smi!==undefined){document.querySelectorAll('#segsmi button').forEach(b=>b.classList.toggle('on',+b.dataset.v===(j.smi?1:0)))}}catch(e){}}
async function cfgSet(k,v){try{const j=await(await fetch(`/api/config?${k}=${v}`,{cache:"no-store"})).json();if(j.ok){setSeg(j);$("#cfginfo").textContent=`速度 ${j.mhz_limit?j.mhz_limit+" MHz 相当":"Max"}、JIT ${j.jit?"ON":"OFF"}、起動画面 ${j.sramboot?"ON":"OFF"}、メイン RAM ${j.wb?"ライトバック":"ライトスルー"}${j.board===2?"、SMI "+(j.smi?"ON":"OFF"):""}(保存済み)`}else msg("設定に失敗しました"+(j.error?": "+j.error:""))}catch(e){msg("設定に失敗しました: "+e)}}
function msg(t){const m=$("#msg");m.textContent=t;m.style.display="block";setTimeout(()=>m.style.display="none",2500)}
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const sum=a=>{let s=0;for(const b of a)s=(Math.imul(s,31)+b)>>>0;return s};
const hex8=v=>v.toString(16).toUpperCase().padStart(8,"0");
const fmtUp=s=>s>=3600?Math.floor(s/3600)+"h"+Math.floor(s%3600/60)+"m":Math.floor(s/60)+"m"+s%60+"s";
const log=(t,c)=>{const d=$("#proglog");d.innerHTML+=(c?`<span class="${c}">${t}</span>`:t)+"\n";d.scrollTop=1e9};
const bar=p=>$("#progfill").style.width=p+"%";
async function st(){const r=await fetch("/api/status",{cache:"no-store",signal:AbortSignal.timeout(5000)});return r.json()}
function info(j){setSeg(j);curName=j.name||"";if(j.mock)$("#mock").textContent="モック(FPGA なし)";if(j.name&&j.name!=="vmpu68"){$("#ttl").textContent=j.name;document.title="vmpu68 - "+j.name}
 $("#info").innerHTML=`<span>ファームウェア</span><span>${esc(j.version)} <small>(Pi ${esc(j.pi_version||j.version)}, FPGA ${esc(j.fpga_version||"不明")})</small></span><span>基板</span><span>V${esc(j.hw||"?")}</span><span>ホスト名</span><span>${esc(j.name||"—")}</span><span>メイン RAM</span><span>${j.ram_mb?j.ram_mb+" MB":"—"}</span><span>バスクロック</span><span>${j.bus_mhz?j.bus_mhz+" MHz":"—"}</span><span>Wi-Fi MACアドレス</span><span>${esc(j.mac||"—")}</span><span>IPアドレス</span><span>${esc(location.hostname)}</span><span>稼働時間</span><span>${fmtUp(j.uptime||0)}</span>`}
st().then(info).catch(()=>{});setInterval(()=>{if(!busy)st().then(info).catch(()=>{})},10000);
function locDlg(){$("#locmodal").style.display="flex"}
function locHide(){$("#locmodal").style.display="none"}
async function locGo(){locHide();try{const j=await(await fetch("/api/locate?ms=10000",{cache:"no-store"})).json();msg(j.ok?"LED を白く点滅させています(10 秒)":"失敗しました")}catch(e){msg("失敗しました: "+e)}}
let curName="";
function nameDlg(){$("#namein").value=curName;$("#namemodal").style.display="flex";setTimeout(()=>$("#namein").focus(),50)}
function nameHide(){$("#namemodal").style.display="none"}
async function nameSave(){const v=$("#namein").value.trim();if(new TextEncoder().encode(v).length>31){msg("ホスト名が長すぎます(31 バイトまで)");return}
 try{const j=await(await fetch(`/api/config?name=${encodeURIComponent(v)}`,{cache:"no-store"})).json();if(j.ok){nameHide();msg(`ホスト名を「${j.name||"vmpu68"}」にしました`);st().then(info).catch(()=>{})}else msg("設定に失敗しました")}catch(e){msg("設定に失敗しました: "+e)}}
async function doReboot(){if(!confirm("VMPU68とX68000を再起動します。よろしいですか？"))return;msg("再起動しています...");try{await fetch("/api/reboot")}catch(e){}}
// ---- firmware ----
let file=null,hdr=null,busy=false;
async function fwPick(){file=$("#fwfile").files[0];hdr=null;$("#fwapply").disabled=true;if(!file)return;
 const a=new Uint8Array(await file.arrayBuffer());const dv=new DataView(a.buffer);
 if(a.length<64||String.fromCharCode(...a.slice(0,4))!=="VPK1"||dv.getUint32(4,true)!==64||sum(a.slice(0,60))!==dv.getUint32(60,true)){$("#fwinfo").innerHTML='<span class="err">vpk 形式ではありません</span>';return}
 hdr={label:String.fromCharCode(...a.slice(8,32)).replace(/\0.*$/,""),koff:dv.getUint32(32,true),klen:dv.getUint32(36,true),ksum:dv.getUint32(40,true),foff:dv.getUint32(44,true),flen:dv.getUint32(48,true),fsum:dv.getUint32(52,true)};
 const kok=sum(a.subarray(hdr.koff,hdr.koff+hdr.klen))===hdr.ksum,fok=!hdr.flen||sum(a.subarray(hdr.foff,hdr.foff+hdr.flen))===hdr.fsum;
 $("#fwinfo").innerHTML=`${esc(hdr.label)}: RPi カーネル ${hdr.klen} bytes ${kok?"ok":'<span class="err">checksum NG</span>'} / FPGA ${hdr.flen?hdr.flen+" bytes sum "+hex8(hdr.fsum)+(fok?" ok":' <span class="err">checksum NG</span>'):"なし"}`;
 $("#fwapply").disabled=!(kok&&fok)}
async function put(a){const q="path="+encodeURIComponent("SD:/update.vpk");let off=0,r;
 while(off<a.length){const n=Math.min(8000,a.length-off);let body="data=";for(let i=0;i<n;i++)body+=a[off+i].toString(16).padStart(2,"0");
  for(let t=0;;t++){try{r=await(await fetch(`/api/put?${q}&off=${off}&last=${off+n>=a.length?1:0}`,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body})).json();if(r.ok)break}catch(e){}
   if(t>=20)throw"アップロードに失敗しました";await new Promise(x=>setTimeout(x,500))}
  off+=n;bar(off/a.length*60)}
 if(r.sum!==hex8(sum(a))||r.size!==a.length)throw"転送後の検証に失敗しました";}
function progHide(){$("#progmodal").style.display="none"}
const act=v=>v==="same"?"同じ(省略)":v==="none"?"なし":"書換え";
async function fwApply(){if(!file||!hdr)return;
 if(!confirm("ファームウェアを更新して再起動します。X68000 が動作中の場合は停止します。よろしいですか？"))return;
 busy=true;stopScan=true;$("#fwapply").disabled=true;$("#progx").disabled=true;$("#proglog").innerHTML="";bar(0);$("#progmodal").style.display="flex";
 try{const a=new Uint8Array(await file.arrayBuffer());log(`${hdr.label} をアップロード中...`);await put(a);log("アップロード完了、インストール開始","ok");
  const r=await(await fetch(`/api/update?mode=${$("#trymode").checked?"try":"stable"}&force=${$("#force").checked?1:0}`)).json();
  if(!r.ok)throw r.error==="already installed"?"同じファームウェアが既にインストールされています(「上書き更新」で書き換えます)":r.error||"update refused";
  log(`RPi カーネル: ${act(r.kernel_action)} / FPGA: ${act(r.fpga_action)}`);
  let last="",down=0,rebooting=false;
  for(;;){await new Promise(x=>setTimeout(x,1000));let j;
   try{j=await st()}catch(e){down++;if(!rebooting){if(down<5)continue;throw"接続が切れました"}if(down>180)throw"再起動後に応答がありません";continue}
   if(rebooting){if(!down&&(j.update||{}).phase==="reboot")continue;log(`復帰: firmware ${j.version} (${j.build||""})`,"ok");bar(100);info(j);msg($("#trymode").checked?"お試し起動で復帰しました。20秒間問題なく動けば自動で確定します":"更新完了");break}
   down=0;const u=j.update||{};if(u.phase!==last){last=u.phase;log(`[${u.phase}] ${u.msg||""}`,u.phase==="error"?"err":"")}
   if(u.phase==="error")throw u.msg;
   bar(60+(u.total?u.done/u.total:0)*40);
   if(u.phase==="reboot"){rebooting=true;log("再起動中... 復帰を待っています")}}
 }catch(e){log("エラー: "+e,"err")}
 busy=false;stopScan=false;$("#progx").disabled=false;$("#fwapply").disabled=false}
// ---- LAN peers: the board hears the vhd68/vfd68 UDP beacons (/api/peers);
// 再スキャン additionally probes the whole /24 over HTTP ----
const found={};let scanning=false,stopScan=false;
const show=()=>{const l=Object.values(found).sort((a,b)=>+a.ip.split(".")[3]-+b.ip.split(".")[3]);
 $("#peers").innerHTML=l.map(p=>`<div class="peer"><span><span class="tag ${/^v[hfm]/.test(p.dev)?p.dev:"x"}">${p.dev}</span>${p.name?`<span style="color:#e8e8e8">${esc(p.name)}</span> `:""}<span class="meta">${[p.ver?"v"+esc(p.ver):"",p.mac?esc(p.mac):"",p.hw?"HW "+esc(p.hw):""].filter(Boolean).join(" · ")}</span> <span class="meta">@${p.ip}</span></span><span><button onclick="window.open('http://${p.ip}${p.port&&p.port!=80?":"+p.port:""}/')">Web UI</button></span></div>`).join("")||'<span class="meta">vhd68 / vfd68 が見つかりません（同じLANで起動していますか？）</span>'};
// the beacon carries no MAC / hardware revision: ask each peer's /api/status once
async function enrich(ip){const p=found[ip];if(!p||p.mac!==undefined)return;p.mac="";
 try{const j=await(await fetch(`http://${ip}/api/status`,{signal:AbortSignal.timeout(3000),cache:"no-store"})).json();p.mac=j.mac||"";p.hw=j.hw||"";if(j.name)p.name=j.name;show()}catch(e){}}
async function beacons(){try{const j=await(await fetch("/api/peers",{cache:"no-store",signal:AbortSignal.timeout(3000)})).json();
 for(const p of j.peers||[]){const o=found[p.ip]||{};found[p.ip]={ip:p.ip,dev:p.device,ver:p.version,name:p.name||o.name,port:p.port,mac:o.mac,hw:o.hw}}show();for(const ip in found)enrich(ip)}catch(e){}}
const SC_ICON=$("#sc").innerHTML;
beacons();setInterval(()=>{if(!scanning&&!stopScan)beacons()},3000);
async function scan(){if(scanning)return;const base=location.hostname.split(".").slice(0,3).join(".");
 if(!/^\d+\.\d+\.\d+$/.test(base)){msg("IP アドレスでアクセスしたときだけ探せます");return}
 scanning=true;const me=location.hostname;let n=0,i=1;
 const w=async()=>{while(i<=254&&!stopScan){const ip=base+"."+i++;if(ip===me)continue;
  try{const r=await fetch(`http://${ip}/api/status`,{signal:AbortSignal.timeout(1200),cache:"no-store"});const j=await r.json();
   if(j&&j.version!==undefined){const k=j.device==="vmpu68"?"vmpu68":Array.isArray(j.ids)?"vhd68":(j.hw!==undefined||j.wdboot!==undefined)?"vfd68":"?";
    found[ip]={ip,dev:k,ver:j.version,name:j.name||(found[ip]||{}).name,mac:j.mac||"",hw:j.hw||""};show()}}catch(e){}
  n++;$("#sc").textContent=`スキャン中 ${n}/253`}};
 await Promise.all(Array.from({length:24},w));scanning=false;$("#sc").innerHTML=SC_ICON}
</script><script>// screen.js is fetched with a fresh query string so a browser never keeps an old copy across firmware updates
(()=>{const s=document.createElement("script");s.src="/screen.js?"+Date.now();document.body.appendChild(s)})();</script></body></html>
)HTML";
