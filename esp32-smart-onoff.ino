/*
  AKILLI KUYU SİSTEMİ
  Telefon çağrısı, SMS ve Web üzerinden pompa kontrolü
  Donanım: ESP32 + SIM800L + 2x Röle + Güç kaynağı
*/

#include <WiFi.h>
#include <WebServer.h>
#include <SoftwareSerial.h>

// ====== WiFi AYARLARI ======
const char* ssid = "KUYU_SISTEMI";
const char* password = "12345678";

// ====== YETKİLİ NUMARALAR (virgülle ayır) ======
const String yetkiliNumaralar[] = {"+905551234567", "+905551234568"};
const int yetkiliSayisi = 2;

// ====== PIN TANIMLARI ======
#define RELAY_PIN     26   // Pompa rölesi
#define LED_PIN        2   // Durum LED'i
#define SIM_RX_PIN    16   // SIM800L TX -> ESP32 RX
#define SIM_TX_PIN    17   // SIM800L RX -> ESP32 TX
#define SIVI_SEVIYE   34   // (opsiyonel) su seviye sensörü

// ====== DURUM ======
bool pompaDurum = false;
bool manuelKilit = false;
unsigned long sonCalisma = 0;
int hataSayisi = 0;

// ====== NESNELER ======
SoftwareSerial sim800(SIM_RX_PIN, SIM_TX_PIN);
WebServer server(80);

// ====== SMS GÖNDER ======
void smsGonder(String numara, String mesaj) {
  sim800.println("AT+CMGF=1");
  delay(200);
  sim800.print("AT+CMGS=\"");
  sim800.print(numara);
  sim800.println("\"");
  delay(200);
  sim800.print(mesaj);
  delay(100);
  sim800.write(26);
  delay(1000);
}

// ====== POMPA KONTROL ======
void pompaAc() {
  digitalWrite(RELAY_PIN, HIGH);
  pompaDurum = true;
  sonCalisma = millis();
  digitalWrite(LED_PIN, HIGH);
}

void pompaKapat() {
  digitalWrite(RELAY_PIN, LOW);
  pompaDurum = false;
  digitalWrite(LED_PIN, LOW);
}

void pompaToggle() {
  if (pompaDurum) pompaKapat();
  else pompaAc();
}

// ====== SMS KOMUT ÇÖZÜMLE ======
void smsIsle(String metin, String gonderen) {
  metin.toUpperCase();
  metin.trim();

  if (metin.indexOf("AC") >= 0 || metin.indexOf("ON") >= 0) {
    pompaAc();
    smsGonder(gonderen, "Pompa ACIK - Akilli Kuyu Sistemi");
  }
  else if (metin.indexOf("KAPA") >= 0 || metin.indexOf("OFF") >= 0) {
    pompaKapat();
    smsGonder(gonderen, "Pompa KAPALI - Akilli Kuyu Sistemi");
  }
  else if (metin.indexOf("DURUM") >= 0 || metin.indexOf("STATUS") >= 0) {
    String durum = pompaDurum ? "ACIK" : "KAPALI";
    smsGonder(gonderen, "Pompa: " + durum + "\nManuel Kilit: " +
              (manuelKilit ? "AKTIF" : "PASIF"));
  }
  else if (metin.indexOf("KILIT") >= 0) {
    manuelKilit = true;
    smsGonder(gonderen, "Manuel kilit AKTIF. Pompa degistirilemez.");
  }
  else if (metin.indexOf("COZ") >= 0) {
    manuelKilit = false;
    smsGonder(gonderen, "Manuel kilit COZULDU.");
  }
  else {
    smsGonder(gonderen, "Komutlar: AC, KAPA, DURUM, KILIT, COZ");
  }
}

// ====== YETKİ KONTROL ======
bool yetkiliMi(String numara) {
  for (int i = 0; i < yetkiliSayisi; i++) {
    if (numara.indexOf(yetkiliNumaralar[i]) >= 0) return true;
  }
  return false;
}

// ====== GSM ÇAĞRI KONTROL (DTMF ile) ======
void gelenCagriKontrol() {
  if (sim800.available()) {
    String cevap = sim800.readString();

    // Yeni bir çağrı geliyor
    if (cevap.indexOf("RING") >= 0) {
      int idx = cevap.indexOf("+CLIP: \"");
      if (idx >= 0) {
        String numara = cevap.substring(idx + 8);
        numara = numara.substring(0, numara.indexOf("\""));
        if (yetkiliMi(numara)) {
          sim800.println("ATA");  // Çağrıyı cevapla
          delay(2000);
          // Kullanıcıya talimat ver
          sim800.println("AT+DTMF=1");  // Sesli geribildirim (ops)
          delay(500);

          // DTMF dinleme döngüsü
          unsigned long cagriBaslangici = millis();
          while (millis() - cagriBaslangici < 120000) {  // 2 dakika timeout
            server.handleClient();
            if (sim800.available()) {
              String dtmf = sim800.readString();
              if (dtmf.indexOf("+DTMF: 1") >= 0) {
                pompaAc();
                // Onay sesi (biplama)
                sim800.println("AT+DTMF=1");
                delay(3000);
              }
              else if (dtmf.indexOf("+DTMF: 0") >= 0) {
                pompaKapat();
                sim800.println("AT+DTMF=1");
                delay(3000);
              }
              else if (dtmf.indexOf("+DTMF: 3") >= 0 || dtmf.indexOf("NO CARRIER") >= 0) {
                break;  // Çık
              }
            }
            delay(50);
          }
          // Kapa ve SMS gönder
          String durum = pompaDurum ? "ACIK" : "KAPALI";
          smsGonder(numara, "Pompa: " + durum + "\nGorusme sonlandi.");
          sim800.println("ATH");
          delay(1000);
        }
      }
    }
  }
}

// ====== SMS OKU ======
void smsOku() {
  sim800.println("AT+CMGF=1");
  delay(200);
  sim800.println("AT+CMGL=\"REC UNREAD\"");
  delay(500);

  while (sim800.available()) {
    String cevap = sim800.readString();

    // Gönderen numarayı bul
    int idx = cevap.indexOf("+CMGL:");
    if (idx >= 0) {
      int numIdx = cevap.indexOf("\",\"", idx + 6);
      if (numIdx >= 0) {
        String numara = cevap.substring(numIdx + 3);
        numara = numara.substring(0, numara.indexOf("\""));
        numara.replace("\"", "");

        if (yetkiliMi(numara)) {
          // Mesaj metnini bul
          int satirSonu = cevap.lastIndexOf('\n');
          if (satirSonu >= 0) {
            String mesaj = cevap.substring(satirSonu + 1);
            mesaj.trim();
            if (mesaj.length() > 0) {
              smsIsle(mesaj, numara);
            }
          }
        }
      }
    }
  }

  // Tüm SMS'leri okundu olarak işaretle
  sim800.println("AT+CMGD=1,4");
  delay(200);
}

// ====== WEB ARAYÜZÜ (Neon Tema) ======
void webHandle() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Akıllı Kuyu Sistemi</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700;800&display=swap');
body{
  font-family:'Inter',sans-serif;
  background:#0a0a0f;
  min-height:100vh;
  display:flex;align-items:center;justify-content:center;
  padding:16px;
  position:relative;
  overflow-x:hidden
}
body::before{
  content:'';position:fixed;top:-50%;left:-50%;width:200%;height:200%;
  background:radial-gradient(circle at 30% 40%,rgba(0,200,255,.06),transparent 60%),
             radial-gradient(circle at 70% 60%,rgba(0,255,136,.04),transparent 50%);
  z-index:0;pointer-events:none
}
.card{
  position:relative;z-index:1;
  width:100%;max-width:420px;
  background:rgba(18,18,28,.85);
  backdrop-filter:blur(20px);
  -webkit-backdrop-filter:blur(20px);
  border:1px solid rgba(255,255,255,.06);
  border-radius:28px;
  padding:36px 28px;
  text-align:center;
  box-shadow:
    0 0 40px rgba(0,150,255,.06),
    inset 0 1px 0 rgba(255,255,255,.04);
  transition:all .3s ease
}
.card:hover{box-shadow:0 0 60px rgba(0,150,255,.1),inset 0 1px 0 rgba(255,255,255,.06)}
.logo{
  display:flex;align-items:center;justify-content:center;gap:10px;margin-bottom:6px
}
.logo-icon{
  width:40px;height:40px;
  background:linear-gradient(135deg,#00c8ff,#00ff88);
  border-radius:12px;
  display:flex;align-items:center;justify-content:center;
  font-size:20px;color:#0a0a0f;font-weight:800
}
h1{
  font-size:20px;font-weight:700;
  background:linear-gradient(135deg,#e0e0f0,#9090b0);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  letter-spacing:-.3px
}
.subtitle{
  font-size:12px;color:rgba(255,255,255,.3);
  margin-bottom:28px;font-weight:400;
  letter-spacing:1px;text-transform:uppercase
}
/* POMPA DURUM */
.pump-container{
  position:relative;margin:8px auto 28px;
  width:120px;height:120px
}
.pump-ring{
  position:absolute;inset:0;
  border-radius:50%;
  border:3px solid rgba(255,255,255,.05);
  transition:all .6s ease
}
.pump-ring.active{
  border-color:#00ff88;
  box-shadow:0 0 30px rgba(0,255,136,.2),inset 0 0 30px rgba(0,255,136,.04);
  animation:pulse-ring 2s ease-in-out infinite
}
@keyframes pulse-ring{
  0%,100%{box-shadow:0 0 20px rgba(0,255,136,.15),inset 0 0 20px rgba(0,255,136,.02)}
  50%{box-shadow:0 0 40px rgba(0,255,136,.3),inset 0 0 40px rgba(0,255,136,.06)}
}
.pump-ring.off{
  border-color:rgba(255,255,255,.08);
  box-shadow:none
}
.pump-icon{
  position:absolute;inset:12px;
  border-radius:50%;
  background:rgba(255,255,255,.03);
  display:flex;align-items:center;justify-content:center;
  flex-direction:column;
  transition:all .5s ease
}
.pump-icon svg{width:48px;height:48px;transition:all .5s ease}
.pump-icon .pump-blade{transition:all .5s ease;transform-origin:center}
.pump-icon.active .pump-blade{animation:spin 1.5s linear infinite;fill:#00ff88}
.pump-icon.off .pump-blade{fill:rgba(255,255,255,.15)}
@keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
.status-label{
  font-size:14px;font-weight:600;margin-top:4px;
  transition:all .3s ease
}
.status-label.active{color:#00ff88;text-shadow:0 0 20px rgba(0,255,136,.4)}
.status-label.off{color:rgba(255,255,255,.2)}
/* KİLİT BADGE */
.kilit-badge{
  display:inline-flex;align-items:center;gap:6px;
  padding:6px 16px;border-radius:20px;
  font-size:12px;font-weight:600;
  margin-bottom:24px;
  border:1px solid rgba(255,200,0,.2);
  background:rgba(255,200,0,.06);
  color:#fbbf24;
  box-shadow:0 0 20px rgba(255,200,0,.08)
}
.kilit-badge svg{width:14px;height:14px;fill:#fbbf24}
/* BUTONLAR */
.buttons{display:flex;flex-direction:column;gap:12px;margin-bottom:24px}
.btn{
  display:flex;align-items:center;justify-content:center;gap:10px;
  padding:16px 24px;border-radius:16px;
  font-size:16px;font-weight:700;
  border:none;cursor:pointer;text-decoration:none;
  transition:all .25s ease;letter-spacing:.3px;
  position:relative;overflow:hidden
}
.btn-ac{
  background:linear-gradient(135deg,#00e676,#00c853);
  color:#0a0a0f;
  box-shadow:0 0 30px rgba(0,230,118,.15)
}
.btn-ac:hover{
  transform:translateY(-2px) scale(1.01);
  box-shadow:0 0 50px rgba(0,230,118,.3)
}
.btn-ac:active{transform:translateY(0) scale(.98)}
.btn-kapat{
  background:linear-gradient(135deg,#ff1744,#d50000);
  color:#fff;
  box-shadow:0 0 30px rgba(255,23,68,.15)
}
.btn-kapat:hover{
  transform:translateY(-2px) scale(1.01);
  box-shadow:0 0 50px rgba(255,23,68,.3)
}
.btn-kapat:active{transform:translateY(0) scale(.98)}
.btn:disabled{
  background:rgba(255,255,255,.06);
  color:rgba(255,255,255,.15);
  box-shadow:none;cursor:not-allowed;
  transform:none !important
}
.btn .icon{width:20px;height:20px;fill:currentColor}
/* BİLGİLER */
.info-grid{
  display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:20px
}
.info-item{
  background:rgba(255,255,255,.03);
  border-radius:12px;padding:12px;
  border:1px solid rgba(255,255,255,.04)
}
.info-item .label{
  font-size:10px;text-transform:uppercase;
  color:rgba(255,255,255,.25);letter-spacing:.8px;margin-bottom:4px
}
.info-item .value{
  font-size:16px;font-weight:700;color:rgba(255,255,255,.8)
}
.info-item .value.green{color:#00ff88}
.info-item .value.red{color:#ff1744}
/* METOT ETİKETLERİ */
.methods{
  display:flex;gap:8px;justify-content:center;flex-wrap:wrap
}
.method-tag{
  display:inline-flex;align-items:center;gap:5px;
  padding:5px 12px;border-radius:8px;
  font-size:10px;font-weight:500;color:rgba(255,255,255,.3);
  background:rgba(255,255,255,.03);
  border:1px solid rgba(255,255,255,.04)
}
.method-tag svg{width:12px;height:12px;fill:rgba(255,255,255,.25)}
/* LOADING */
.loading{display:none;position:fixed;inset:0;z-index:999;
  background:rgba(10,10,15,.8);backdrop-filter:blur(8px);
  align-items:center;justify-content:center;flex-direction:column;gap:16px}
.loading.show{display:flex}
.spinner{width:40px;height:40px;border:3px solid rgba(255,255,255,.05);
  border-top:3px solid #00ff88;border-radius:50%;animation:spin .8s linear infinite}
.loading span{color:rgba(255,255,255,.4);font-size:14px}
</style>
</head>
<body>

<div class="loading" id="loading">
  <div class="spinner"></div>
  <span>İşlem yapılıyor...</span>
</div>

<div class="card">
  <div class="logo">
    <div class="logo-icon">⬡</div>
    <h1>Akıllı Kuyu</h1>
  </div>
  <div class="subtitle">Pompa Kontrol Sistemi</div>

  <!-- POMPA GÖSTERGESİ -->
  <div class="pump-container">
    <div class="pump-ring" id="pumpRing"></div>
    <div class="pump-icon off" id="pumpIcon">
      <svg viewBox="0 0 48 48" fill="none" xmlns="http://www.w3.org/2000/svg">
        <circle cx="24" cy="24" r="10" fill="rgba(255,255,255,.05)" stroke="rgba(255,255,255,.1)" stroke-width="1.5"/>
        <path class="pump-blade" d="M24 14L26.5 20.5L33 23L26.5 25.5L24 32L21.5 25.5L15 23L21.5 20.5Z" fill="rgba(255,255,255,.15)"/>
        <circle cx="24" cy="23" r="3" fill="rgba(255,255,255,.08)"/>
      </svg>
      <div class="status-label off" id="statusLabel">KAPALI</div>
    </div>
  </div>

  <!-- KİLİT BADGE -->
  <div id="kilitBadge" style="display:none" class="kilit-badge">
    <svg viewBox="0 0 24 24"><path d="M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1s3.1 1.39 3.1 3.1v2z"/></svg>
    Manuel Kilit Aktif
  </div>

  <!-- BUTONLAR -->
  <div class="buttons">
    <a href="/ac" class="btn btn-ac" id="btnAc" style="display:none" onclick="showLoading()">
      <svg class="icon" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
      POMPAYI ÇALIŞTIR
    </a>
    <a href="/kapat" class="btn btn-kapat" id="btnKapat" style="display:none" onclick="showLoading()">
      <svg class="icon" viewBox="0 0 24 24"><path d="M6 6h12v2H6zm0 5h12v2H6zm0 5h12v2H6z"/></svg>
      POMPAYI DURDUR
    </a>
    <button class="btn" id="btnKilitli" disabled style="display:none">🔒 Kilitli</button>
  </div>

  <!-- İSTATİSTİK -->
  <div class="info-grid">
    <div class="info-item">
      <div class="label">Pompa</div>
      <div class="value" id="pompaStatus">KAPALI</div>
    </div>
    <div class="info-item">
      <div class="label">Çalışma Süresi</div>
      <div class="value" id="calismaSuresi">0s</div>
    </div>
  </div>

  <!-- KONTROL YÖNTEMLERİ -->
  <div class="methods">
    <span class="method-tag">
      <svg viewBox="0 0 24 24"><path d="M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z"/></svg>
      Çağrı
    </span>
    <span class="method-tag">
      <svg viewBox="0 0 24 24"><path d="M20 2H4c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zm0 18H4V4h16v16zM6 12h2v5H6zm4-3h2v8h-2zm4 5h2v3h-2zm0-10h2v2h-2z"/></svg>
      SMS
    </span>
    <span class="method-tag">
      <svg viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/></svg>
      Web
    </span>
  </div>
</div>

<script>
let pompa = false, kilit = false;

function showLoading(){document.getElementById('loading').classList.add('show')}
function hideLoading(){document.getElementById('loading').classList.remove('show')}

function updateUI(){
  const pumpRing=document.getElementById('pumpRing');
  const pumpIcon=document.getElementById('pumpIcon');
  const statusLabel=document.getElementById('statusLabel');
  const btnAc=document.getElementById('btnAc');
  const btnKapat=document.getElementById('btnKapat');
  const btnKilitli=document.getElementById('btnKilitli');
  const kilitBadge=document.getElementById('kilitBadge');
  const pompaStatus=document.getElementById('pompaStatus');
  const calismaSuresi=document.getElementById('calismaSuresi');

  if(pompa){
    pumpRing.className='pump-ring active';
    pumpIcon.className='pump-icon active';
    statusLabel.className='status-label active';
    statusLabel.textContent='ÇALIŞIYOR';
    pompaStatus.textContent='ÇALIŞIYOR';
    pompaStatus.className='value green';
  }else{
    pumpRing.className='pump-ring off';
    pumpIcon.className='pump-icon off';
    statusLabel.className='status-label off';
    statusLabel.textContent='KAPALI';
    pompaStatus.textContent='KAPALI';
    pompaStatus.className='value red';
  }

  if(kilit){
    kilitBadge.style.display='inline-flex';
    btnAc.style.display='none';
    btnKapat.style.display='none';
    btnKilitli.style.display='flex';
  }else{
    kilitBadge.style.display='none';
    btnKilitli.style.display='none';
    if(pompa){
      btnAc.style.display='none';
      btnKapat.style.display='flex';
    }else{
      btnAc.style.display='flex';
      btnKapat.style.display='none';
    }
  }

  calismaSuresi.textContent='-';
  hideLoading();
}

async function fetchDurum(){
  try{
    const r=await fetch('/durum');
    const d=await r.json();
    pompa=d.pompa;
    kilit=d.kilit;
    updateUI();
  }catch(e){console.log('Bağlantı hatası',e)}
  setTimeout(fetchDurum,2000);
}

// Sayfa yüklenince ve buton tıklanınca güncelle
document.querySelectorAll('.btn-ac,.btn-kapat').forEach(b=>{
  b.addEventListener('click',function(e){
    e.preventDefault();
    showLoading();
    fetch(this.href).then(()=>{fetchDurum()}).catch(()=>{hideLoading()});
  });
});

fetchDurum();
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void webAc() {
  if (!manuelKilit) { pompaAc(); }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void webKapat() {
  if (!manuelKilit) { pompaKapat(); }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void webJson() {
  String json = "{";
  json += "\"pompa\":" + String(pompaDurum ? "true" : "false") + ",";
  json += "\"kilit\":" + String(manuelKilit ? "true" : "false") + ",";
  json += "\"calisma_suresi\":" + String(millis() / 1000);
  json += "}";
  server.send(200, "application/json", json);
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SIVI_SEVIYE, INPUT);
  digitalWrite(RELAY_PIN, LOW);

  // GSM başlat
  sim800.begin(9600);
  delay(3000);
  sim800.println("AT+CLIP=1");  // Arayan numara göster
  delay(500);
  sim800.println("AT+DDET=1");  // DTMF algılama
  delay(500);

  // WiFi AP modu (internet yoksa direkt bağlan)
  WiFi.softAP(ssid, password);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(ip);

  // Web sunucu
  server.on("/", webHandle);
  server.on("/ac", webAc);
  server.on("/kapat", webKapat);
  server.on("/durum", webJson);
  server.begin();

  Serial.println("Akilli Kuyu Sistemi Hazir");
}

// ====== LOOP ======
void loop() {
  server.handleClient();
  gelenCagriKontrol();
  smsOku();

  // Otomatik kapatma (1 saat)
  if (pompaDurum && (millis() - sonCalisma > 3600000)) {
    pompaKapat();
  }

  delay(100);
}
