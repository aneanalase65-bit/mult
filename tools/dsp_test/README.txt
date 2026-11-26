Tes DSP multiband di host (butuh g++). Jalankan dari folder ini:

  g++ -O2 -std=c++17 -o test_mb test_mb.cpp && ./test_mb
  g++ -O2 -std=c++17 -o test_chain test_chain.cpp && ./test_chain
  # cek sintaks audio_engine.cpp tanpa NDK (pakai stub oboe/jni):
  g++ -std=c++17 -fsyntax-only -Istubs -I../../app/src/main/cpp ../../app/src/main/cpp/audio_engine.cpp

Yang dicek: respons rata (crossover), auto level ke target, desis beku saat diam,
efek kontrol sibilance, stabilitas (NaN) dan crossfade antar mode.
