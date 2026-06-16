#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>

int main() {
    printf("[probe] Opening /dev/dri/card0...\n");
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("[probe] Failed to open /dev/dri/card0");
        return 1;
    }

    drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
    if (!pres) {
        perror("[probe] Failed to get plane resources");
        close(fd);
        return 1;
    }

    printf("[probe] Planes found: %u\n", pres->count_planes);

    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane* p = drmModeGetPlane(fd, pres->planes[i]);
        if (p) {
            printf("\n--- Plane ID: %u ---\n", p->plane_id);
            printf("  Possible CRTCs : 0x%x\n", p->possible_crtcs);
            printf("  Current CRTC ID: %u\n", p->crtc_id);
            printf("  Current FB ID  : %u\n", p->fb_id);
            printf("  Formats count  : %u\n", p->count_formats);
            printf("  Formats        : ");
            for (uint32_t f = 0; f < p->count_formats; f++) {
                char format_str[5] = {0};
                format_str[0] = p->formats[f] & 0xFF;
                format_str[1] = (p->formats[f] >> 8) & 0xFF;
                format_str[2] = (p->formats[f] >> 16) & 0xFF;
                format_str[3] = (p->formats[f] >> 24) & 0xFF;
                printf("%s ", format_str);
            }
            printf("\n");
            
            drmModeFreePlane(p);
        } else {
            printf("[probe] Failed to get plane %u\n", pres->planes[i]);
        }
    }

    drmModeFreePlaneResources(pres);
    close(fd);
    printf("\n[probe] Completed.\n");
    return 0;
}
