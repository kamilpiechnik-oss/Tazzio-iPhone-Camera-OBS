# Tazzio iPhone Camera for OBS

Samodzielny plugin OBS, który odbiera obraz i opcjonalny dźwięk z Safari na iPhonie przez WebRTC. Produkt nie korzysta z kont ani pokoi Tazzio CoStream.

## Użytkowanie

1. Zainstaluj plugin i uruchom OBS.
2. Otwórz `Doki -> Tazzio iPhone Camera`.
3. Utwórz kod QR i zeskanuj go iPhonem.
4. W Safari wybierz aparat, rozdzielczość i FPS, a następnie uruchom kamerę.

Backend publiczny działa pod `https://iphone-camera.tazzio.pl`. TURN może być współdzielony infrastrukturalnie z innymi usługami, ale sesje, tokeny, backend, instalator i branding są oddzielne.

## Walidacja

- `node --test` w katalogu `service` sprawdza jednorazowe parowanie i routing SDP.
- GitHub Actions buduje osobne `tazzio-iphone-camera.dll`, ZIP i instalator Windows.
- Faktyczny FPS zależy od aparatu, modelu iPhone'a, Safari oraz warunków sieciowych.
