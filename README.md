# Tazzio iPhone Camera for OBS

Samodzielny plugin OBS, który odbiera obraz i opcjonalny dźwięk z Safari na iPhonie przez WebRTC. Produkt nie korzysta z kont ani pokoi Tazzio CoStream.

## Użytkowanie

1. Zainstaluj plugin i uruchom OBS.
2. Otwórz `Doki -> Tazzio iPhone Camera`.
3. Utwórz kod QR i zeskanuj go iPhonem.
4. W Safari wybierz tryb `Wi-Fi / LAN` dla bezpośredniego przesyłu w tej samej sieci albo `Sieć komórkowa / Internet` dla wymuszonego przekaźnika TURN na VPS.
5. Wybierz aparat, profil jakości i FPS, a następnie uruchom kamerę.

Domyślny profil `BALANCED` odpowiada sprawdzonej konfiguracji wcześniejszej aplikacji LAN: 1080p, 60 FPS i limit 12 Mb/s. `HIGH QUALITY` używa 1080p, 30 FPS i 16 Mb/s. Safari i sprzęt iPhone'a mogą ograniczyć faktycznie osiąganą rozdzielczość lub liczbę klatek.

Plugin jest publikowany pod `https://tazzio.pl/pluginy/iphone-camera-obs/`, a jego wydzielony backend korzysta z technicznych tras `tazzio.pl`. TURN może być współdzielony infrastrukturalnie z innymi usługami, ale sesje, tokeny, backend, instalator i branding są oddzielne.

## Walidacja

- `node --test` w katalogu `service` sprawdza jednorazowe parowanie i routing SDP.
- GitHub Actions buduje osobne `tazzio-iphone-camera.dll`, ZIP i instalator Windows.
- Faktyczny FPS zależy od aparatu, modelu iPhone'a, Safari oraz warunków sieciowych.
