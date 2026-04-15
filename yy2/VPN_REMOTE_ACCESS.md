# Scout (yy2) — Remote Access via VPN

## Problem

S3 Vision Hub ve Scout yalnızca yerel 2.4G ağda (`localhost-ofc-dev0`,
internetsiz). Kullanıcı evden çıkınca kamerayı göremez. Çözüm: kalıcı bir
VPN tüneli — kullanıcının telefonundaki NordVPN + evde bir "home endpoint"
(C5 veya laptop) tüneli tutarak erişim sağlar.

## NordVPN + C5 neden doğrudan çalışmaz

NordVPN bir **client servisi** — telefonun ISP trafiğini NordVPN
sunucusuna yönlendirir. Ama telefon NordVPN üzerindeyken ev ağındaki C5'e
direkt ulaşamaz — çünkü NordVPN ev ağına bağlı değil. NordVPN "Meshnet"
özelliği mevcut (P2P mesh, ev cihazlarını dahil eder) ama ESP32 istemcisi
yok.

## Pratik çözümler (tercih sırasıyla)

### A. Tailscale / Headscale mesh (EN KOLAY, önerilir)

- Ev laptop'una Tailscale yükle (zaten NordVPN yanında çalışır)
- Telefona Tailscale yükle
- Laptop `subnet router` modunda çalışsın → 2.4G IoT ağını (192.168.39.0/24)
  Tailscale ağına sunar
- Telefon Tailscale'e bağlı: `http://192.168.39.197` direkt erişilebilir
- C5'te hiç değişiklik gerekmez

```
Phone (Tailscale) ──► ev laptop (Tailscale subnet router)
                           │
                           └─► 2.4G LAN → C5 (192.168.39.197)
                                        → S3 (192.168.39.157)
```

**Kurulum (laptop):**
```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up --advertise-routes=192.168.39.0/24 --accept-routes
```
Admin panelden subnet onayla. Telefona Tailscale kur, giriş yap.

### B. WireGuard VPS — full control

Bir VPS'te (Hetzner, DigitalOcean ~3 €/ay) WireGuard server kur.
- C5 outgoing WG client olarak bağlansın
- Telefon WG client olarak aynı server'a bağlansın
- C5'in WG IP'si (10.0.0.2) üzerinden phone erişir

C5'te `esp_wireguard` managed component var. Kconfig ile user'ın server
public key + endpoint girebileceği alanlar eklendi (aşağıda). Şimdilik
bu feature **disabled by default** — önce Tailscale dene, gerekirse WG.

### C. Cloudflare Tunnel

Cloudflare Tunnel ücretsiz, ancak ESP32 native client yok — laptop'ta
cloudflared çalıştırmak gerekir (laptop hep açık değilse kullanılamaz).

## Önerilen yol

**Tailscale (A)** — 5 dakikada kurulur, C5 değiştirmek gerekmez, NordVPN
ile paralel çalışır, ESP32'ye ekstra yük yok. Telefonda iki VPN aynı anda
(NordVPN uç çıkış için, Tailscale ev erişimi için) olmaz — ama modern iOS
16+ ve Android 12+ ikisini de destekler (split-tunneling).

## Güvenlik notları

- Tailscale: E2E WireGuard şifreli, kendi key altyapısı
- Admin panel: şu an Basic Auth — HTTPS + session cookie eklenebilir
- Telegram: HTTPS ile (mbedTLS + cert bundle), güvenli
