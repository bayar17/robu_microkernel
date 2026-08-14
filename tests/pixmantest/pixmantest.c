#include <stdio.h>
#include <pixman.h>

int main(void) {
    pixman_image_t *img = pixman_image_create_bits(PIXMAN_a8r8g8b8, 4, 4, NULL, 0);
    if (!img) {
        printf("pixman test: pixman_image_create_bits failed\n");
        return 1;
    }

    pixman_color_t color = { 0xffff, 0x0000, 0x0000, 0xffff };
    pixman_image_t *fill = pixman_image_create_solid_fill(&color);
    if (!fill) {
        printf("pixman test: pixman_image_create_solid_fill failed\n");
        return 1;
    }

    pixman_image_composite32(PIXMAN_OP_SRC, fill, NULL, img, 0, 0, 0, 0, 0, 0, 4, 4);

    uint32_t *data = pixman_image_get_data(img);
    uint32_t pixel = data[0];
    printf("pixman test: pixel[0]=0x%08x (expect 0xffff0000, opaque red in ARGB32)\n", pixel);

    int ok = pixel == 0xffff0000u;

    pixman_image_unref(fill);
    pixman_image_unref(img);

    printf("pixman test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
