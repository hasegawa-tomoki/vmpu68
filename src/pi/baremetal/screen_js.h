// SPDX-License-Identifier: MIT
// Screen viewer for the Web UI (served at "/screen.js", used by the 画面 card
// in update_page.h).  Nothing is read until the user presses 更新: every
// read borrows the X68000 bus (~0.4 us per word), so no automatic refresh.
// The key box below the canvas types into Human68k (/api/key) and re-reads
// the screen 2 s later.
// Reads the video registers (/api/screen/regs), the visible rows of the
// VRAM and the sprite controller (/api/bus/dump), then composes graphic
// pages, the text screen and the sprite/BG screen in the browser: 512x512
// 16/256/65536 colours and 1024x1024 16 colours, 128 sprites with priority
// against two BG layers, 8x8 or 16x16 BG tiles; layer order from VC R1,
// colour 0 transparent for text, sprites and BG.
// Not reproduced: translucency, raster-timed register changes.
// While a floppy transfer is running the board refuses the read (JSON
// error fd_busy); the message is shown under the canvas.
static const char s_ScreenJs[] = R"JS((()=>{
const $=s=>document.querySelector(s);
const cv=$("#scr"),cx=cv.getContext("2d"),inf=$("#scrinfo"),btn=$("#scrbtn"),dl=$("#scrdl");
function ph(t){cv.width=768;cv.height=512;cx.fillStyle="#000";cx.fillRect(0,0,768,512);cx.fillStyle="#9aa";cx.font="22px system-ui,sans-serif";cx.textAlign="center";cx.fillText(t,384,262)}
ph("右上の更新ボタンを押すと X68000 の画面を読み取ります");
let words=0;
// the board answers a screen read with JSON {ok:false,error,message} instead of
// data when it must not borrow the bus (fd_busy: a floppy transfer is running)
async function checkErr(r){if((r.headers.get("content-type")||"").startsWith("application/json")){const e=await r.json();if(e&&e.ok===false)throw e.message||e.error}}
async function dump(addr,n){const out=new Uint16Array(n);let o=0;
 while(o<n){const k=Math.min(n-o,32768);
  const r=await fetch(`/api/bus/dump?addr=0x${(addr+2*o).toString(16)}&words=${k}`,{cache:"no-store",signal:AbortSignal.timeout(20000)});
  await checkErr(r);const b=new Uint8Array(await r.arrayBuffer());if(b.length!==2*k)throw"VRAM の読み取りが途中で切れました";
  for(let i=0;i<k;i++)out[o+i]=(b[2*i]<<8)|b[2*i+1];o+=k}
 words+=n;return out}
// rows [sy,sy+h) of a plane of `total` rows x `rw` words, wrapping at the bottom
async function rows(base,rw,total,sy,h){sy&=total-1;const out=new Uint16Array(h*rw),n1=Math.min(h,total-sy);
 out.set(await dump(base+sy*rw*2,n1*rw));if(n1<h)out.set(await dump(base,(h-n1)*rw),n1*rw);return out}
const rgb=v=>((((v>>6)&31)*255/31|0)<<16)|((((v>>11)&31)*255/31|0)<<8)|(((v>>1)&31)*255/31|0);
let busyS=false;
async function shot(){if(busyS)return;busyS=true;btn.disabled=true;words=0;const t0=performance.now();inf.textContent="";
 try{
  const rr=await fetch("/api/screen/regs",{cache:"no-store",signal:AbortSignal.timeout(10000)});await checkErr(rr);const rb=new Uint8Array(await rr.arrayBuffer());
  const R=new Uint16Array(rb.length/2);for(let i=0;i<R.length;i++)R[i]=(rb[2*i]<<8)|rb[2*i+1];
  const crtc=R.subarray(0,24),vc0=R[24],vc1=R[25],vc2=R[26];
  const gpal=Array.from(R.subarray(27,283),rgb),spal=Array.from(R.subarray(283,539),rgb),tpal=spal.slice(0,16);
  const r20=crtc[20],W=[256,512,768,768][r20&3],H=(r20&4)?512:256,cmode=vc0&3,big=(vc0>>2)&1;
  const txOnly=false,grOn=!txOnly&&(big?(vc2&0x10):(vc2&15)),txOn=(vc2&0x20)!==0;
  const g=new Int32Array(W*H).fill(-1),t=new Uint8Array(W*H);
  let desc=`${W}×${H}`;
  if(grOn){
   if(big){desc+=" / 1024×1024 16色";const sx=crtc[12]&1023,d=await rows(0xC00000,1024,1024,crtc[13],H);
    for(let y=0;y<H;y++)for(let x=0;x<W;x++){const v=d[y*1024+((x+sx)&1023)]&15;if(v)g[y*W+x]=gpal[v]}}
   else if(cmode===3){desc+=" / 65536色";const sx=crtc[12]&511,d=await rows(0xC00000,512,512,crtc[13],H);
    for(let y=0;y<H;y++)for(let x=0;x<W;x++)g[y*W+x]=rgb(d[y*512+((x+sx)&511)])}
   else{const n=cmode===1?2:4,mask=cmode===1?255:15;desc+=cmode===1?" / 256色":" / 16色";
    // VC R1 bits 1:0, 3:2, 5:4, 7:6 = page number at the front .. back
    const order=[];for(let i=0;i<4;i++){const p=(vc1>>(2*i))&3;if(p<n&&!order.includes(p))order.push(p)}for(let p=0;p<n;p++)if(!order.includes(p))order.push(p);
    const en=cmode===1?[vc2&3,vc2&12]:[vc2&1,vc2&2,vc2&4,vc2&8];
    for(const p of order){if(!en[p])continue;const sx=crtc[12+2*p]&511,d=await rows(0xC00000+p*0x80000,512,512,crtc[13+2*p],H);
     for(let y=0;y<H;y++)for(let x=0;x<W;x++){const i=y*W+x;if(g[i]>=0)continue;const v=d[y*512+((x+sx)&511)]&mask;if(v)g[i]=gpal[v]}}}}
  if(txOn){const tsx=crtc[10]&1023,pl=[];for(let p=0;p<4;p++)pl.push(await rows(0xE00000+p*0x20000,64,1024,crtc[11],H));
   for(let y=0;y<H;y++)for(let x=0;x<W;x++){const rx=(x+tsx)&1023,o=y*64+(rx>>4),s=15-(rx&15);
    t[y*W+x]=((pl[0][o]>>s)&1)|(((pl[1][o]>>s)&1)<<1)|(((pl[2][o]>>s)&1)<<2)|(((pl[3][o]>>s)&1)<<3)}}
  // sprites/BG: 128 sprite registers, BG control, the 32 KB PCG area (BG maps
  // included); drawn back to front: PRW=1 sprites, BG1, PRW=2, BG0, PRW=3,
  // lower sprite numbers in front.  Not shown in 768-dot modes (no sprites).
  const s=new Int32Array(W*H).fill(-1);let spOn=!txOnly&&(vc2&0x40)&&W!==768;
  if(spOn){const sr=await dump(0xEB0000,512),bg=await dump(0xEB0800,9),pcg=await dump(0xEB8000,16384),ctl=bg[4],t16=(bg[8]&3)!==0;
   spOn=(ctl&0x200)!==0;
   if(spOn){desc+=" + スプライト/BG";
    // 16x16 pattern n = four 8x8 blocks (top-left, bottom-left, top-right, bottom-right), 8x8 pattern n = block n&3 of 16x16 pattern n>>2.
    // A BG map word is the same in both tile sizes: bits 7:0 pattern, 11:8 palette block (verified against a real starfield; a 10-bit pattern number reads the map area itself and shows stripes)
    const p16=(n,x,y)=>(pcg[n*64+((x>>3)*2+(y>>3))*16+(y&7)*2+((x&7)>>2)]>>(12-4*(x&3)))&15;
    const p8=(n,x,y)=>(pcg[n*16+y*2+(x>>2)]>>(12-4*(x&3)))&15;
    const spr=prw=>{for(let i=127;i>=0;i--){if((sr[4*i+3]&3)!==prw)continue;const x0=(sr[4*i]&1023)-16,y0=(sr[4*i+1]&1023)-16,c=sr[4*i+2],n=c&255,pb=((c>>8)&15)*16;
     for(let py=0;py<16;py++){const sy=y0+py;if(sy<0||sy>=H)continue;for(let px=0;px<16;px++){const sx=x0+px;if(sx<0||sx>=W)continue;
      const v=p16(n,c&0x4000?15-px:px,c&0x8000?15-py:py);if(v)s[sy*W+sx]=spal[pb+v]}}}};
    const bgl=(on,area,X,Y)=>{if(!on)return;const ts=t16?16:8,m=64*ts-1,mb=0x2000+(area&1)*0x1000;
     for(let y=0;y<H;y++){const my=(y+Y)&m,ty=(my/ts|0)*64,tpy=my%ts;for(let x=0;x<W;x++){const mx=(x+X)&m,tile=pcg[mb+ty+(mx/ts|0)];
      let tx=mx%ts,ty2=tpy;if(tile&0x4000)tx=ts-1-tx;if(tile&0x8000)ty2=ts-1-ty2;
      const v=t16?p16(tile&255,tx,ty2):p8(tile&255,tx,ty2);if(v)s[y*W+x]=spal[((tile>>8)&15)*16+v]}}};
    spr(1);bgl(ctl&8,ctl>>4,bg[2]&1023,bg[3]&1023);spr(2);bgl(ctl&1,ctl>>1,bg[0]&1023,bg[1]&1023);spr(3)}}
  // VC R1 bits 13:12 sprites, 11:10 text, 9:8 graphics: lower = in front
  const ord=[[(vc1>>12)&3,2],[(vc1>>10)&3,1],[(vc1>>8)&3,0]].sort((a,b)=>a[0]-b[0]).map(a=>a[1]),back=grOn?gpal[0]:0;
  const img=cx.createImageData(W,H),px=img.data;
  for(let i=0,j=0;i<W*H;i++,j+=4){const tv=t[i],gv=g[i],sv=s[i];let c=-1;
   for(const L of ord){if(L===2){if(sv>=0){c=sv;break}}else if(L===1){if(tv){c=tpal[tv];break}}else if(gv>=0){c=gv;break}}
   if(c<0)c=back;
   px[j]=c>>16;px[j+1]=(c>>8)&255;px[j+2]=c&255;px[j+3]=255}
  cv.width=W;cv.height=H;cx.putImageData(img,0,0);
  inf.textContent="";                     // the read summary line is gone; the div still carries errors and key-send notes
  dl.disabled=false;
 }catch(e){inf.innerHTML='<span class="err">読み取りに失敗しました: '+String(e).replace(/</g,"&lt;")+"</span>"}
 busyS=false;btn.disabled=false}
btn.onclick=shot;
// keyboard shortcuts, plain keys like Gmail/GitHub so nothing collides with the
// browser (Ctrl+R/S/D/K are reload/save/bookmark/search there): R = read the
// screen, D = download the PNG, K or / = focus the key box, Esc = leave it.
// Ignored while typing in a text field or with a modifier held.
document.addEventListener("keydown",e=>{const inField=/^(INPUT|TEXTAREA|SELECT)$/.test((e.target||{}).tagName||"");
 if(inField){if(e.key==="Escape"){e.preventDefault();e.target.blur()}return}
 if(e.ctrlKey||e.metaKey||e.altKey||e.shiftKey)return;const k=e.key.toLowerCase();
 if(k==="r"){e.preventDefault();shot()}
 else if(k==="d"){e.preventDefault();if(!dl.disabled)dl.onclick()}
 else if(k==="k"||k==="/"){e.preventDefault();const b=$("#scrkey");b.focus();b.select()}});
// save the last read screen as PNG: vmpu68-<name>-YYYYMMDD-HHMMSS.png
dl.onclick=()=>{const d=new Date(),p=n=>String(n).padStart(2,"0"),nm=(document.title.split(" - ")[1]||"screen").replace(/[^\w\-]+/g,"_");
 const fn=`vmpu68-${nm}-${d.getFullYear()}${p(d.getMonth()+1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}.png`;
 cv.toBlob(b=>{const u=URL.createObjectURL(b),a=document.createElement("a");a.href=u;a.download=fn;document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(u),2000)},"image/png")};
// key box: the text goes into Human68k's keyboard buffer.  With "3秒後に画面更新"
// checked the screen is re-read 3 s later; uncheck it when starting a benchmark
// from here (a screen read stops the X68000 for up to 20 x 25 ms).
const kb=$("#scrkey"),ks=$("#scrsend");
async function send(){const t=kb.value;if(!t&&!$("#scrent").checked)return;ks.disabled=true;
 try{const r=await(await fetch(`/api/key?text=${encodeURIComponent(t)}&enter=${$("#scrent").checked?1:0}`,{cache:"no-store",signal:AbortSignal.timeout(20000)})).json();
  kb.value="";inf.textContent=r.dropped?`${r.sent} キーを送りました(${r.dropped} 文字は送れません: 半角のみ)`:"";
  if($("#scrauto").checked)setTimeout(shot,3000)}
 catch(e){inf.innerHTML='<span class="err">送信に失敗しました: '+String(e).replace(/</g,"&lt;")+"</span>"}
 ks.disabled=false}
ks.onclick=send;kb.onkeydown=e=>{if(e.key==="Enter"){e.preventDefault();send()}};
})();
)JS";
