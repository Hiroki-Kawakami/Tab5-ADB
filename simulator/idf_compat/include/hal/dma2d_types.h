// Host (simulator) shim for hal/dma2d_types.h — only the slice hal/ppa_types.h
// needs: the 2D-DMA burst-length enum that the PPA burst-length enum aliases.
// The simulator has no 2D-DMA engine, so burst length is a no-op knob here; the
// values are kept identical to ESP-IDF v5.4.3 for source portability.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enumeration of 2D-DMA data burst length options
 */
typedef enum {
    DMA2D_DATA_BURST_LENGTH_8 = 1,      /*!< 2D-DMA block size: 8 bytes */
    DMA2D_DATA_BURST_LENGTH_16,         /*!< 2D-DMA block size: 16 bytes */
    DMA2D_DATA_BURST_LENGTH_32,         /*!< 2D-DMA block size: 32 bytes */
    DMA2D_DATA_BURST_LENGTH_64,         /*!< 2D-DMA block size: 64 bytes */
    DMA2D_DATA_BURST_LENGTH_128,        /*!< 2D-DMA block size: 128 bytes */
    DMA2D_DATA_BURST_LENGTH_INVALID,    /*!< Invalid 2D-DMA block size */
} dma2d_data_burst_length_t;

#ifdef __cplusplus
}
#endif
