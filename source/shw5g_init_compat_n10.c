/* Scoped adapter for CAM_GEAC_GEAC_A_20260801_2.1 libyuv_cam.so.
 * Use only with CAM-3 V6/V7 on this N10. No vendor binary is modified.
 * The original writer remembers a failed slave address across link switches.
 * Omit serializer transactions for absent V5 while retaining shared DES setup.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return -1 unless the exact reviewed 488-byte factory preset is supplied. */
int filter_shw5g(const uint8_t *in, int size, uint8_t *out) {
    uint64_t hash=UINT64_C(14695981039346656037);
    if (size!=488) return -1;
    for (int i=0;i<size;i++) hash=(hash^in[i])*UINT64_C(1099511628211);
    if (hash!=UINT64_C(0x8683cb9dc7852fff)) return -1;
    int used=0, mask=0, omitted=0, selects=0, final_mask=0;
    for (int i=0;i<size;) {
        int n=in[i]==0?2:in[i]+1;
        if (i+n>size || (n!=2 && n!=5)) return -1;
        int drop=0;
        if (n==5) {
            int addr=in[i+1], reg=(in[i+2]<<8)|in[i+3], val=in[i+4];
            if (addr==0x2d && reg==6) {
                mask=val;
                if (val==0xf1) {drop=1;selects++;}
            }
            if (addr==0x80 && mask==0xf1) {drop=1;omitted++;}
        }
        if (!drop) {
            memcpy(out+used,in+i,(size_t)n);
            /* The last link selection enables only the two connected links. */
            if (n==5 && in[i+1]==0x2d && in[i+2]==0 && in[i+3]==6 &&
                in[i+4]==0xff && i>size/2) {out[used+4]=0xf6;final_mask++;}
            used+=n;
        }
        i+=n;
    }
    return omitted==9 && selects==1 && final_mask==1 && used==438 ? used : -1;
}

typedef int (*writer_fn)(int,int,int,int,uint8_t *,int);
int write_register(int bus,int des,int serial,int sensor,uint8_t *table,int size) {
    static writer_fn original;
    if (!original) {
        const char *library=getenv("SHW5G_SENSOR_LIBRARY");
        if (!library || library[0]!='/') {
            fprintf(stderr,"Set SHW5G_SENSOR_LIBRARY to the verified vendor library absolute path\n"); return -1;
        }
        void *handle=dlopen(library,RTLD_NOW|RTLD_NOLOAD);
        if (handle) original=(writer_fn)dlsym(handle,"write_register");
        if (!original || original==write_register) {
            fprintf(stderr,"SHW5G adapter cannot resolve vendor writer\n");return -1;
        }
    }
    if (bus==7 && des==0x49 && size==488) {
        uint8_t filtered[488];
        int n=filter_shw5g(table,size,filtered);
        if (n<0) {fprintf(stderr,"SHW5G adapter refuses unrecognized preset\n");return -1;}
        fprintf(stderr,"SHW5G V6/V7 adapter: exact preset verified; skipped absent V5, enabled links 1/2\n");
        return original(bus,des,serial,sensor,filtered,n);
    }
    return original(bus,des,serial,sensor,table,size);
}
