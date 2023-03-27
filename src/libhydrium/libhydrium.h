/*
 * libhydrium/hydrium.h
 * 
 * This is the main libhydrium API entry point.
 */

#ifndef HYDRIUM_H_
#define HYDRIUM_H_

#include <stddef.h>
#include <stdint.h>

#define HYDRIUM_VERSION_INT 0x1000002001
#define HYDRIUM_VERSION_STRING "0.2.1"

#ifdef _WIN32
    #ifdef HYDRIUM_INTERNAL_BUILD
        #define HYDRIUM_EXPORT __declspec(__dllexport)
    #else
        #define HYDRIUM_EXPORT __declspec(__dllimport)
    #endif
#else
    #if defined(__GNUC__) || defined(__CLANG__)
        #define HYDRIUM_EXPORT __attribute__((visibility ("default")))
    #else
        #define HYDRIUM_EXPORT
    #endif /* __GNUC__ || __clang__ */
#endif

typedef enum HYDStatusCode {
    HYD_OK = 0,
    HYD_DEFAULT = -1,
    HYD_ERROR_START = -10,
    HYD_NEED_MORE_OUTPUT = -11,
    HYD_NEED_MORE_INPUT = -12,
    HYD_NOMEM = -13,
    HYD_API_ERROR = -14,
    HYD_INTERNAL_ERROR = -15,
} HYDStatusCode;

typedef struct HYDAllocator {
    void *opaque;
    void *(*alloc_func)(size_t size, void *opaque);
    void (*free_func)(void *ptr, void *opaque);
} HYDAllocator;

typedef struct HYDImageMetadata {
    size_t width;
    size_t height;
    /* flag, false for sRGB input, true for linear input */
    int linear_light;
} HYDImageMetadata;

/* opaque structure */
typedef struct HYDEncoder HYDEncoder;

HYDRIUM_EXPORT HYDEncoder *hyd_encoder_new(const HYDAllocator *allocator);
HYDRIUM_EXPORT HYDStatusCode hyd_encoder_destroy(HYDEncoder *encoder);

HYDRIUM_EXPORT HYDStatusCode hyd_set_metadata(HYDEncoder *encoder, const HYDImageMetadata *metadata);

HYDRIUM_EXPORT HYDStatusCode hyd_provide_output_buffer(HYDEncoder *encoder, uint8_t *buffer, size_t buffer_len);

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile(HYDEncoder *encoder, const uint16_t *const buffer[3],
                                           uint32_t tile_x, uint32_t tile_y,
                                           ptrdiff_t row_stride, ptrdiff_t pixel_stride);

HYDRIUM_EXPORT HYDStatusCode hyd_send_tile8(HYDEncoder *encoder, const uint8_t *const buffer[3],
                                            uint32_t tile_x, uint32_t tile_y,
                                            ptrdiff_t row_stride, ptrdiff_t pixel_stride);

HYDRIUM_EXPORT HYDStatusCode hyd_release_output_buffer(HYDEncoder *encoder, size_t *written);

HYDRIUM_EXPORT HYDStatusCode hyd_flush(HYDEncoder *encoder);

#endif /* HYDRIUM_H_ */
