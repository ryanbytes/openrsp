#include "openrsp/openrsp.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    const openrsp_model *rsp1a = openrsp_model_lookup(0x1df7u, 0x3000u);
    assert(rsp1a != NULL);
    assert(strcmp(rsp1a->model, "SDRplay RSP1A") == 0);
    assert(rsp1a->support == OPENRSP_SUPPORT_DISCOVERY_ONLY);
    const openrsp_model *rspduo = openrsp_model_lookup(0x1df7u, 0x3020u);
    assert(rspduo != NULL);
    assert(strcmp(rspduo->model, "SDRplay RSPduo") == 0);
    assert(openrsp_model_lookup(0x1df7u, 0xffffu) == NULL);
    assert(openrsp_model_lookup(0xffffu, 0x3000u) == NULL);
    assert(strcmp(openrsp_support_name(OPENRSP_SUPPORT_UNKNOWN), "unknown") == 0);
    return 0;
}
