# jpeg_fullrange_decode

Full-range (JFIF / "PC range") BT.601 YCbCr→RGB JPEG decode. A drop-in for the
IDF `jpeg_decoder_process()` exposing `jpeg_decoder_process_full_range()` with the
identical signature; the engine is still created/destroyed with the stock IDF
`jpeg_new_decoder_engine()` / `jpeg_del_decoder_engine()`.

MJPEG / JFIF content (what `tab5adb-agent` streams) is encoded full-range, but the
IDF P4 decoder bakes the *limited-range* (Y∈[16,235]) BT.601 matrix into the
2D-DMA CSC unit, which under-saturates the image. The device impl replays the IDF
decode flow and overwrites the CSC matrix registers with the full-range
coefficients before the transfer starts.

Shared, target-divergent component (`ESP_PLATFORM` branch in `CMakeLists.txt`):

- **device** — `src/jpeg_fullrange_decode_p4.c`: the register-override driver;
  depends on IDF-internal `esp_driver_jpeg` headers.
- **host (simulator)** — `src/jpeg_fullrange_decode_sim.c`: a passthrough to the
  idf_compat libjpeg shim (`jpeg_decoder_process()`), which already decodes JFIF
  full-range. So app code calls the same `jpeg_decoder_process_full_range()` on
  both targets and the simulator previews the fullrange-fixed device output.
