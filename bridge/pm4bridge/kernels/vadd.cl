kernel void vadd(__global const float *a, __global const float *b, __global float *c,
                 uint n, uint block)
{
    uint gid = __builtin_amdgcn_workgroup_id_x() * block + __builtin_amdgcn_workitem_id_x();
    if (gid < n)
        c[gid] = a[gid] + b[gid];
}
