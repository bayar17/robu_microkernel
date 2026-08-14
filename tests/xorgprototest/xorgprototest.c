#include <stdio.h>
#include <X11/Xproto.h>

int main(void) {
    int ok = 1;

    printf("xorgproto test: X_CreateWindow=%d (expect 1)\n", X_CreateWindow);
    if (X_CreateWindow != 1) {
        ok = 0;
    }

    printf("xorgproto test: sz_xCreateWindowReq=%d (expect 32)\n", sz_xCreateWindowReq);
    if (sz_xCreateWindowReq != 32) {
        ok = 0;
    }

    printf("xorgproto test: sizeof(xCreateWindowReq)=%lu (expect matches sz_xCreateWindowReq)\n",
           (unsigned long)sizeof(xCreateWindowReq));
    if (sizeof(xCreateWindowReq) != (size_t)sz_xCreateWindowReq) {
        ok = 0;
    }

    printf("xorgproto test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
