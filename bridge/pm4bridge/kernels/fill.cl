/* gid = group * block + local id. The last group may be partial: check n. */
kernel void fill(__global uint *out, uint value, uint n, uint block)
{
    uint gid = __builtin_amdgcn_workgroup_id_x() * block + __builtin_amdgcn_workitem_id_x();
    if (gid < n)
        out[gid] = value + gid;
}

/* read-modify-write: proves the GPU sees what the CPU (or an earlier dispatch) wrote */
kernel void addk(__global uint *buf, uint k, uint n, uint block)
{
    uint gid = __builtin_amdgcn_workgroup_id_x() * block + __builtin_amdgcn_workitem_id_x();
    if (gid < n)
        buf[gid] = buf[gid] + k;
}
