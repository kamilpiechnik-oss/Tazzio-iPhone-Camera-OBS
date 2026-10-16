# Tazzio iPhone Camera for OBS

Samodzielny plugin OBS, który odbiera obraz i opcjonalny dźwięk z Safari na iPhonie przez WebRTC. Produkt nie korzysta z kont ani pokoi Tazzio CoStream.

## Użytkowanie

1. Zainstaluj plugin i uruchom OBS.
2. Otwórz `Doki -> Tazzio iPhone Camera`.
3. W Docku wybierz `Wi-Fi / LAN` dla bezpośredniego przesyłu w tej samej sieci albo `Sieć komórkowa / Internet` dla wymuszonego przekaźnika TURN na VPS.
4. Utwórz kod QR i zeskanuj go iPhonem.
5. Wybierz aparat, profil jakości i FPS, a następnie uruchom kamerę.
6. Po uruchomieniu używaj sekcji `Sterowanie kamerą iPhone` w Docku. Pokazuje ona tylko funkcje rzeczywiście udostępnione przez dany aparat i Safari.

W obu trybach plugin tworzy źródło Przeglądarka w OBS, dzięki czemu odbiór i dekodowanie korzystają z tego samego toru Chromium/WebRTC co wcześniejsza aplikacja LAN. W trybie LAN VPS obsługuje tylko jednorazowe parowanie, a media płyną bezpośrednio z iPhone'a do komputera. Domyślny profil `BALANCED` to 1080p, 60 FPS i limit 12 Mb/s. `HIGH QUALITY` używa 1080p, 30 FPS i 16 Mb/s.

Dock pozwala zmieniać FPS, obiektyw, zoom, latarkę, focus, ekspozycję, jasność, ISO i balans bieli, o ile Safari zwróci daną funkcję w możliwościach aktywnej kamery. Zmiana FPS ustawia zarówno kamerę, jak i ograniczenie nadajnika WebRTC, a Dock pokazuje faktycznie raportowany FPS. Obrót telefonu automatycznie aktualizuje wymiary źródła Browser Source, więc źródło przełącza proporcje między poziomymi i pionowymi. Safari i sprzęt iPhone'a mogą ograniczyć faktycznie osiąganą rozdzielczość, liczbę klatek lub dostępność ręcznych ustawień.

Dock automatycznie sprawdza wersję po uruchomieniu i pozwala ponowić sprawdzanie ręcznie. Jeśli dostępna jest nowsza wersja, pokazuje przycisk prowadzący do oficjalnej strony pobierania instalatora. Aktualizacja nie uruchamia się bez kliknięcia użytkownika; przed instalacją należy zamknąć OBS.

Plugin jest publikowany pod `https://tazzio.pl/pluginy/iphone-camera-obs/`, a jego wydzielony backend korzysta z technicznych tras `tazzio.pl`. TURN może być współdzielony infrastrukturalnie z innymi usługami, ale sesje, tokeny, backend, instalator i branding są oddzielne.

## Walidacja

- `node --test` w katalogu `service` sprawdza jednorazowe parowanie, routing SDP oraz odseparowany kanał sterowania Dock ↔ iPhone.
- GitHub Actions buduje osobne `tazzio-iphone-camera.dll`, ZIP i instalator Windows.
- Faktyczny FPS zależy od aparatu, modelu iPhone'a, Safari oraz warunków sieciowych.
