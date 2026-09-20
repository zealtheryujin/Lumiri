<p align="center">
  <img src="assets/mascot/lumiri-anime.png" alt="Lumiri mascot" width="280">
</p>

## ENG — English

Stream your PC desktop and games to Nintendo Switch.

### Use at your own risk

Lumiri is provided as is, without any assurance or warranty. You use the application and change streaming or OC settings at your own risk and responsibility. Performance, stability, compatibility, uninterrupted operation, and device safety are not guaranteed.

### Setup and connection

1. Run the `lumiri-host.exe` file.
2. Copy the included `switch/lumiri.nro` to the `switch/` folder on your Switch SD card.
3. Open Lumiri in Homebrew Menu. Your PC and Switch must be on the same local network.
4. Choose **Desktop** or a game from the discovered PC's library and press **A**. If discovery fails, press **X** to enter the PC's IPv4 address.

Your PC needs an NVIDIA or AMD GPU with hardware H.264 encoding and a compatible graphics driver. The encoder is selected automatically. Virtual controller input requires ViGEmBus; the host can start its installer if missing. VB-Cable is optional. Use a homebrew environment compatible with your Switch firmware; application mode provides more memory than album applet mode.

PC Ethernet and stable Switch Wi-Fi help keep streaming smooth. Use only on a trusted local network; the connection does not provide authentication or encryption.

### Controls

| Button / gesture | Action |
|:--|:--|
| A / B | Select or launch / go back |
| Y — home screen | Settings |
| X — home screen | Enter PC IP address |
| − — home screen | Refresh games |
| + — home screen | Exit the app |
| L / R — home screen | Switch between discovered PCs |
| L / R — settings | Switch settings sections |
| Direction buttons | Navigate; use Left/Right to change settings |
| ZL + ZR + Plus | Open/close the session menu |
| Touch drag | Move the mouse |
| Short tap / two-finger tap | Left click / right click |
| Two-finger vertical drag | Scroll |

During streaming, face buttons match Xbox positions: Switch B → Xbox A, A → B, Y → X, and X → Y. When a game asks for Xbox A, press Switch B. Lumiri menus keep Switch A for select and B for back.

Choose **Add a game**, select a PC `.exe`, and enter its name. The session menu provides keyboard entry, Enter/Esc/Alt+Tab/Windows keys, statistics, and disconnect.

### Stream and appearance settings

Press **Y** on the home screen, or open **ZL + ZR + Plus → Stream Settings** during a stream.

- **Picture & sound:** resolution, FPS, bitrate, and audio. Initial values are 720p, 60 FPS, and 8 Mbps. Bitrate range: 2–35 Mbps. Above 10 Mbps, adjust in 5 Mbps steps. Higher bitrate increases network traffic and does not guarantee higher FPS. FPS options: 30, 60, 120, and Unlimited. Unlimited removes the host streaming timer limit; capture and hardware capacity still determine throughput. It does not increase the Switch display refresh rate. Higher FPS at the same bitrate may reduce picture quality. Update both the host and NRO to use these modes.
- **Experience:** performance, H.264 decoder, and mouse sensitivity. **NVDEC** uses Switch hardware decoding with software fallback if unavailable. **Software (CPU)** uses CPU decoding. Video is always H.264; an AMD PC does not change the Switch's NVDEC option.
- **Clock limits:** OC control and CPU/GPU/RAM limits.
- **Appearance & language:** Light/Dark theme and English/Turkish. The app starts in English with the Light theme by default.

Press **B** to save. Theme-only or language-only changes apply during a stream without reconnecting. Stream or OC changes reconnect to the same PC with a brief interruption; the game is not relaunched. Press **X** to discard changes in the connected settings menu.

### Video presentation

This build uses SDL presentation with NVDEC hardware decoding when available. Experimental direct GPU presentation is disabled after a reported startup failure. Existing `directmode=1` settings are ignored in this build; no configuration reset is required.

### Recommended OC profile

**OC support must be installed and configured on your Switch to increase frequencies.** Lumiri NRO cannot raise clocks on its own or install OC support. Its OC settings only request frequencies made available by your OC setup.

| Setting | Value |
|:--|:--|
| Performance | Maximum |
| OC Control | Application |
| CPU Limit | **1785 MHz** |
| GPU Limit | **768 MHz** |
| RAM Limit | **2133 MHz** |

These are requested upper limits; identical frequencies and stability are not guaranteed across devices. RAM may remain at 1600 MHz if the system does not expose 2133 MHz. Balanced mode may request lower CPU/GPU clocks. **System / external** leaves frequency management to the system or external tools. Lower the limits if you experience instability. Higher Switch clocks cannot fix PC or network bottlenecks.

### Switching monitors

Choose **ZL + ZR + Plus → Switch Monitor** to cycle to the next monitor. The monitor must be connected to the capture GPU and enabled in the Windows desktop. Switching capture does not move the game window.

### Updating and saved settings

Replace `SD:/switch/lumiri.nro` with the new NRO. Close the host before updating it. Preferences are saved in `SD:/switch/lumiri.cfg`; an existing `remoteplay.cfg` can also be read on first load. Keep your existing `games.txt` beside the host EXE to preserve custom games. Run `firewall-onar.bat` again if you move the host folder.

### Troubleshooting

| Problem | What to check |
|:--|:--|
| PC not found / connection refused | Check the host is open and ready, verify the IP and local network, and run the firewall helper. |
| No handshake reply | Open with `host-baslat.bat` and check the last host message. Press Esc if an older console has text selected. |
| Host opens and closes | Another host may already be running. Close older hosts and retry. |
| AMD AMF error | Update the AMD driver and check that the GPU supports hardware H.264 encoding. |
| Stuttering | Lower resolution or bitrate; check PC load and the network connection. |
| RAM does not reach 2133 | The system may not expose that frequency; the selected value is an upper limit. |

Host log: `lumiri-host.log` beside the EXE. Switch logs: `SD:/switch/lumiri-boot.log` and `SD:/switch/lumiri-clocks.log`.


---

## TR — Türkçe

PC ekranını ve oyunlarını Nintendo Switch'e aktar.

### Kullanım sorumluluğu

Lumiri olduğu gibi sunulur; herhangi bir güvence veya garanti verilmez. Uygulamanın kullanımı ile yayın ve OC ayarlarında yapılan değişikliklerin riski ve sorumluluğu kullanıcıya aittir. Performans, stabilite, uyumluluk, kesintisiz çalışma veya cihaz güvenliği garanti edilmez.

### Kurulum ve bağlantı

1. `lumiri-host.exe` dosyasını çalıştır.
2. Paketteki `switch/lumiri.nro` dosyasını Switch'in SD kartındaki `switch/` klasörüne kopyala.
3. Lumiri'yi Homebrew Menu'den aç. PC ve Switch aynı yerel ağda olmalı.
4. Bulunan PC'nin kütüphanesinden **Masaüstü** veya bir oyun seçip **A**'ya bas. PC bulunmazsa **X** ile IPv4 adresini gir.

PC'de H.264 kodlamayı destekleyen NVIDIA veya AMD ekran kartı ve uygun sürücüsü gerekir. Kodlayıcı otomatik seçilir. Sanal kontrolcü için ViGEmBus gerekir; host eksikse kurulumunu başlatabilir. VB-Cable isteğe bağlıdır. Switch'te sistem sürümünle uyumlu bir homebrew ortamı kullan; uygulama modu, albüm modundan daha fazla bellek sağlar.

PC'yi Ethernet ile bağlamak ve Switch'te kararlı Wi-Fi kullanmak akıcılığa yardımcı olur. Yalnızca güvendiğin yerel ağda kullan; bağlantı kimlik doğrulaması ve şifreleme içermez.

### Kontroller

| Tuş / hareket | İşlev |
|:--|:--|
| A / B | Seç veya başlat / geri dön |
| Y — ana ekran | Ayarlar |
| X — ana ekran | PC IP adresini gir |
| − — ana ekran | Oyun listesini yenile |
| + — ana ekran | Uygulamadan çık |
| L / R — ana ekran | Birden fazla PC varsa aralarında geçiş yap |
| L / R — ayarlar | Ayar bölümleri arasında geçiş yap |
| Yön tuşları | Gezin; ayarlarda sol/sağ ile değeri değiştir |
| ZL + ZR + Plus | Yayın menüsünü aç/kapat |
| Dokunarak sürükleme | Fareyi hareket ettir |
| Kısa dokunuş / iki parmakla dokunuş | Sol tık / sağ tık |
| İki parmakla dikey sürükleme | Kaydır |

Yayında tuşlar Xbox konumlarına göre eşleşir: Switch B → Xbox A, A → B, Y → X ve X → Y. Oyun Xbox A isterse Switch B'ye bas. Lumiri menülerinde A seçim, B geri olarak kalır.

**Oyun ekle** kartından PC'deki `.exe` dosyasını seçip adını gir. Yayın menüsünden klavyeyi açabilir, Enter/Esc/Alt+Tab/Windows tuşlarını gönderebilir, istatistikleri gösterebilir ve bağlantıyı kesebilirsin.

### Yayın ve görünüm ayarları

Ana ekranda **Y**'ye veya yayın sırasında **ZL + ZR + Plus → Yayın Ayarları**'na gir.

- **Görüntü ve ses:** çözünürlük, FPS, bit hızı ve ses. Başlangıç değerleri 720p, 60 FPS ve 8 Mbps'dir. Bit hızı aralığı: 2–35 Mbps. 10 Mbps üzerinde 5 Mbps adımlarla ayarlanır. Daha yüksek bit hızı ağ trafiğini artırır; daha yüksek FPS garantilemez. FPS seçenekleri: 30, 60, 120 ve Sınırsız. Sınırsız, host yayın zamanlayıcısının sınırını kaldırır; gerçek hız yakalama ve donanım kapasitesine bağlıdır. Switch ekranının yenileme hızını artırmaz. Aynı bit hızında daha yüksek FPS görüntü kalitesini düşürebilir. Bu modlar için hem host hem NRO güncellenmelidir.
- **Deneyim:** performans, H.264 çözücü ve fare hassasiyeti. **NVDEC**, Switch'in donanım çözücüsünü kullanır; açılamazsa yazılıma döner. **Yazılım (CPU)**, CPU ile çözer. Yayın her zaman H.264'tür; PC'nin AMD olması NVDEC seçimini değiştirmez.
- **Frekanslar:** OC kontrolü ve CPU/GPU/RAM sınırları.
- **Görünüm ve dil:** açık/koyu tema ve Türkçe/İngilizce. Uygulama varsayılan olarak İngilizce ve açık temayla açılır.

**B** değişiklikleri kaydeder. Yayındayken yalnızca tema veya dil değişirse yeniden bağlanılmaz. Yayın veya OC ayarları değişirse aynı PC'ye kısa bir kesintiyle yeniden bağlanılır; oyun tekrar başlatılmaz. **X**, yayın ayarlarındaki değişiklikleri iptal eder.

### Görüntü sunumu

Bu derleme, kullanılabildiğinde NVDEC donanım çözmesiyle SDL görüntü sunumunu kullanır. Bildirilen açılış hatası nedeniyle deneysel doğrudan GPU sunumu kapatılmıştır. Mevcut `directmode=1` ayarı bu derlemede yok sayılır; ayar dosyasını sıfırlamak gerekmez.

### Tavsiye edilen OC profili

**Frekansları artırmak için Switch'inde OC desteğinin kurulu ve yapılandırılmış olması gerekir.** Lumiri NRO tek başına frekansları yükseltemez veya OC desteği kuramaz. Uygulamadaki OC ayarları yalnızca mevcut OC kurulumunun kullanıma açtığı frekansları talep eder.

| Ayar | Değer |
|:--|:--|
| Performans | Maksimum |
| OC Kontrolü | Uygulama |
| CPU Tavanı | **1785 MHz** |
| GPU Tavanı | **768 MHz** |
| RAM Tavanı | **2133 MHz** |

Bunlar istenen üst sınırlardır; her cihazda aynı hız veya stabilite garanti edilmez. Sistem 2133 MHz RAM hızını desteklemiyorsa 1600 MHz'de kalabilir. Dengeli mod daha düşük CPU/GPU hızları isteyebilir. **Sistem / harici**, frekans yönetimini sisteme veya harici araçlara bırakır. Kararsızlık yaşarsan sınırları düşür. Switch frekansını artırmak PC veya ağ kaynaklı takılmaları çözmez.

### Monitör değiştirme

**ZL + ZR + Plus → Monitör Değiştir** ile sıradaki monitöre geç. Monitörün yakalama yapılan ekran kartına bağlı ve Windows masaüstünde etkin olması gerekir. Bu işlem oyun penceresini diğer monitöre taşımaz.

### Güncelleme ve kayıtlı ayarlar

Yeni NRO'yu `SD:/switch/lumiri.nro` üzerine kopyala. Host'u güncellemeden önce kapat. Ayarlar `SD:/switch/lumiri.cfg` içinde saklanır; eski `remoteplay.cfg` dosyası da ilk yüklemede okunabilir. Özel oyunlarını korumak için mevcut `games.txt` dosyanı host EXE'sinin yanında tut. Host klasörünü taşırsan `firewall-onar.bat` dosyasını yeniden çalıştır.

### Sorun giderme

| Sorun | Yapılacak işlem |
|:--|:--|
| PC bulunmuyor / connection refused | Host'un açık ve hazır olduğunu, IP adresini ve aynı ağda olduğunu kontrol et; güvenlik duvarı yardımcısını çalıştır. |
| Host açılıp kapanıyor | Başka bir host açık olabilir. Eski host'ları kapatıp yeniden dene. |
| AMD AMF hatası | AMD sürücüsünü güncelle; kartın H.264 donanım kodlamasını desteklediğini kontrol et. |
| Yayın takılıyor | Çözünürlük veya bit hızını düşür; PC yükünü ve ağ bağlantısını kontrol et. |
| RAM 2133 olmuyor | Sistem bu hızı sunmuyor olabilir; seçilen değer bir üst sınırdır. |

Host günlüğü: EXE'nin yanında `lumiri-host.log`. Switch günlükleri: `SD:/switch/lumiri-boot.log` ve `SD:/switch/lumiri-clocks.log`.
