/* Experimental: 16-byte (uint4) accesses, to tell a bytes/s limit from a
 * requests/s limit. Each work item reads `iters` uint4 values. */
kernel void sumbw4(__global const uint4 *src, __global uint *out, uint iters, uint block)
{
    uint lid = __builtin_amdgcn_workitem_id_x();
    uint g = __builtin_amdgcn_workgroup_id_x();
    uint i = g * block * iters + lid;
    uint4 s = (uint4)(0);
    for (uint k = 0; k < iters; k++, i += block)
        s += src[i];
    out[g * block + lid] = s.x + s.y + s.z + s.w;
}

kernel void copybw4(__global const uint4 *src, __global uint4 *dst, uint iters, uint block)
{
    uint lid = __builtin_amdgcn_workitem_id_x();
    uint i = __builtin_amdgcn_workgroup_id_x() * block * iters + lid;
    for (uint k = 0; k < iters; k++, i += block)
        dst[i] = src[i];
}
