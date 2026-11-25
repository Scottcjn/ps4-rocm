/* Deliberately needs a private segment (scratch). pm4_load must reject it.
 * Built without the audit; never dispatched. */
kernel void scratch(__global uint *out, uint n)
{
    volatile uint tmp[64];
    for (uint i = 0; i < 64; i++)
        tmp[i] = i * n;
    out[__builtin_amdgcn_workitem_id_x()] = tmp[n & 63];
}
