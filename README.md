# DbxComp

<img src="docs/logo.png" alt="Logo DbxComp" width="128">

Compressor + limiter real-time untuk Android, terinspirasi perangkat dbx.
Mic / line-in -> compressor (RMS, soft knee) -> limiter -> output, dengan Oboe
(audio latensi rendah) dan meter gain reduction.

## Cara build di GitHub (tanpa Android Studio)

1. Buat repository baru di github.com.
2. Unggah isi proyek ini ke repo:
   - **Dari komputer:** ekstrak zip, lalu seret semua isinya (termasuk folder
     `.github`) ke halaman repo -> *Add file -> Upload files*.
   - **Dari HP:** unggah `DbxComp.zip` apa adanya (*Add file -> Upload files*),
     lalu *Add file -> Create new file*, namai `.github/workflows/build.yml`,
     tempel isi file `build.yml`, dan commit. Workflow akan mengekstrak zip sendiri.
3. Buka tab **Actions**. Build jalan otomatis setiap ada commit
   (atau klik *Run workflow*). Butuh sekitar 5-10 menit.
4. Setelah hijau, buka run-nya -> bagian **Artifacts** -> unduh `DbxComp-debug-apk`
   -> ekstrak -> pasang `app-debug.apk` di HP (izinkan "install dari sumber tidak dikenal").

## Catatan pemakaian

- Pakai **headphone** atau **audio interface USB**. Mic HP + speaker HP = feedback.
- Pilih perangkat input/output di dalam aplikasi. Untuk latensi terendah: interface USB
  atau headphone berkabel. Bluetooth biasanya 100 ms lebih.
- Aplikasi harus tetap di layar depan; Android membatasi mic di latar belakang,
  jadi audio berhenti otomatis saat aplikasi ditinggal.
- Mode input mic memakai `Unprocessed` supaya AGC/noise suppression bawaan HP mati.
- Limiter tanpa lookahead: puncak sangat cepat dipotong keras di ceiling sebagai
  pengaman, jadi bukan brickwall transparan setara hardware.

## Tata letak

- **Bilah atas tetap** (tidak ikut tergulung): meter IN / GR / OUT, START, REKAM, dan BYP
  (bypass semua efek). Di bawahnya tab: **Sumber**, **Mic**, **Kompresor**, **Reverb**, **Preset**.
- Kontrol dikelompokkan dalam kotak berbingkai. Tombol **i** membuka/menutup teks panduan
  untuk kelompok atau kontrol itu.
- Tombol ON/OFF: ON menyala hijau, OFF redup. BYP memakai oranye karena artinya efek dimatikan.

## Preset dan rekam

- **Preset** disimpan sebagai file JSON di `Documents/DbxComp/Preset` (Android 10+). Isinya
  memakai nama parameter (bukan urutan angka), plus catatan, versi aplikasi, dan model HP.
  Tombol: *Muat*, *Simpan*, *Hapus*, *Bagikan* (kirim file lewat aplikasi lain), *Impor*.
  **Netral (bawaan)** = semua efek mati dan semua gain 0 dB, dipakai sebagai pembanding.
  Preset lama dari versi sebelumnya dipindahkan otomatis ke file saat pertama dibuka.
- Android hanya mengizinkan aplikasi melihat file yang dibuatnya sendiri. Kalau aplikasi dipasang
  ulang, file lama tetap ada di folder tetapi tidak muncul di daftar; pakai *Impor* untuk
  memasukkannya lagi.
- **Rekam:** tekan START, lalu *● REK*. Yang direkam adalah hasil akhir setelah compressor +
  limiter, format WAV 16-bit, di **Music/DbxComp** (Android 10+). Android 9 ke bawah: folder
  milik aplikasi (`Android/data/com.example.dbxcomp.v3/files/...`).
- Setiap rekaman otomatis dibuatkan **file info** dengan nama sama di
  `Documents/DbxComp/Rekaman/<nama>.json`: setelan saat merekam, perangkat input/output,
  mono/stereo, sample rate, dan statistik tiap kanal (puncak, RMS, RMS 100 ms terendah dan
  persentil ke-10 sebagai perkiraan desis). Untuk menakar mic/preamp, rekam **mono dengan
  Rekam A/B**: kanal kiri = suara asli (setelah input gain), kanan = hasil olahan.
  Input stereo memakai mode Camcorder yang pada sebagian HP bisa menambah pengolahan bawaan.

## Logo

Logo: kurva transfer compressor (garis redup = 1:1, garis terang = setelah ditekan)
dengan titik LED hijau di lutut kurva. Ikon adaptif + versi monokrom (tema ikon Android 13+)
ada di `app/src/main/res`; PNG untuk Android 7.0-7.1 ada di folder `mipmap-*`.
Ikon notifikasi layanan tangkap suara internal: `drawable/ic_stat_dbxcomp.xml`.

## Compressor multiband + Auto (vokal nyanyi, A Mic)

Tab **Kompresor** -> kotak *Multiband + Auto*. Hanya untuk strip A (mic vokal, 1 channel);
strip B/C tetap memakai compressor dBFS biasa.

- **Mode multiband OFF** = compressor dBFS lama (tidak berubah). **ON** = 3 pita: rendah (< crossover
  rendah, 200 Hz), tengah, tinggi (> crossover tinggi, 4,5 kHz; bertindak seperti de-esser).
  Pindah mode dihaluskan 15 ms (tanpa klik), jadi enak untuk A/B langsung sambil bernyanyi.
- Slider **Ratio / Attack / Release / Knee** di atas dipakai sebagai dasar. Pita rendah dibuat
  lebih lembut dan lambat, pita tinggi lebih tegas dan cepat.
- **Auto ON**: threshold tiap pita mengikuti level rata-rata suara (hanya dihitung saat ada suara di
  atas *Noise floor*), dan makeup otomatis menarik hasil ke *Target level* (bawaan -18 dBFS RMS,
  maksimum +18 dB / -6 dB). Saat diam, adaptasi dibekukan: desis tidak ikut naik terus. Slider
  *Makeup gain* tetap berlaku sebagai trim tambahan. **Auto OFF**: threshold manual dari slider Threshold.
- Catatan: saat jeda panjang di antara frasa, gain makeup terakhir dipertahankan. Aktifkan
  *Mode dinamis (gate)* di tab Mic bila desis di sela frasa terdengar.
- Parameter baru ada di preset (`compressor_vokal`); preset lama tetap terbaca.

Tes DSP di komputer (tanpa Android): lihat `tools/dsp_test/README.txt`.
