/* Each work group owns a contiguous region of block*iters dwords; in every
 * loop iteration one wave touches 64 consecutive dwords. No bounds check:
 * the caller sizes the buffers to groups*block*iters dwords. */
kernel void copybw(__global const uint *src, __global uint *dst, uint iters, uint block)
{
    uint lid = __builtin_amdgcn_workitem_id_x();
    uint i = __builtin_amdgcn_workgroup_id_x() * block * iters + lid;
    for (uint k = 0; k < iters; k++, i += block)
        dst[i] = src[i];
}

/* read only: out[group*block + lid] = sum of the dwords this item read */
kernel void sumbw(__global const uint *src, __global uint *out, uint iters, uint block)
{
    uint lid = __builtin_amdgcn_workitem_id_x();
    uint g = __builtin_amdgcn_workgroup_id_x();
    uint i = g * block * iters + lid;
    uint s = 0;
    for (uint k = 0; k < iters; k++, i += block)
        s += src[i];
    out[g * block + lid] = s;
}

/* write only */
kernel void fillbw(__global uint *dst, uint value, uint iters, uint block)
{
    uint lid = __builtin_amdgcn_workitem_id_x();
    uint i = __builtin_amdgcn_workgroup_id_x() * block * iters + lid;
    for (uint k = 0; k < iters; k++, i += block)
        dst[i] = value + i;
}
