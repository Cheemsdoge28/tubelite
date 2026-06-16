#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

int main() {
    printf("[probe] Opening /dev/dri/card0...\n");
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("[probe] Failed to open /dev/dri/card0");
        return 1;
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        perror("[probe] Failed to set universal planes cap");
        close(fd);
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
        if (!p) {
            printf("[probe] Failed to get plane %u\n", pres->planes[i]);
            continue;
        }

        printf("\n--- Plane ID: %u ---\n", p->plane_id);
        printf("  Possible CRTCs : 0x%x\n", p->possible_crtcs);
        printf("  Current CRTC ID: %u\n", p->crtc_id);
        printf("  Current FB ID  : %u\n", p->fb_id);
        printf("  Formats count  : %u\n", p->count_formats);
        printf("  Formats        : ");
        for (uint32_t f = 0; f < p->count_formats; f++) {
            char fmt[5] = {};
            fmt[0] = (p->formats[f] >>  0) & 0xFF;
            fmt[1] = (p->formats[f] >>  8) & 0xFF;
            fmt[2] = (p->formats[f] >> 16) & 0xFF;
            fmt[3] = (p->formats[f] >> 24) & 0xFF;
            printf("%s ", fmt);
        }
        printf("\n");

        // Query plane type and other properties
        drmModeObjectProperties* props =
            drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);

        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
                if (!prop) continue;

                if (strcmp(prop->name, "type") == 0) {
                    uint64_t val = props->prop_values[j];
                    const char* type_str =
                        val == DRM_PLANE_TYPE_OVERLAY ? "Overlay" :
                        val == DRM_PLANE_TYPE_PRIMARY ? "Primary" :
                        val == DRM_PLANE_TYPE_CURSOR  ? "Cursor"  : "Unknown";
                    printf("  Type           : %s (%llu)\n", type_str, (unsigned long long)val);
                } else if (strcmp(prop->name, "zpos") == 0) {
                    printf("  zpos           : %llu\n", (unsigned long long)props->prop_values[j]);
                } else if (strcmp(prop->name, "alpha") == 0) {
                    printf("  alpha          : %llu\n", (unsigned long long)props->prop_values[j]);
                } else if (strcmp(prop->name, "pixel blend mode") == 0) {
                    printf("  blend mode     : %llu\n", (unsigned long long)props->prop_values[j]);
                } else {
                    // Print all other property names so we know what's available
                    printf("  prop %-20s = %llu\n", prop->name, (unsigned long long)props->prop_values[j]);
                }

                drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        } else {
            printf("  [!] Could not get properties for plane %u\n", p->plane_id);
        }

        drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(pres);
    close(fd);
    printf("\n[probe] Done.\n");
    return 0;
}