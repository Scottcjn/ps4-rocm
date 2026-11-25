/* counter[gid]++ (read-modify-write across dispatches) and a per-dispatch log entry */
kernel void stress(__global uint *counter, __global uint *log, uint idx, uint block)
{
    uint lid = __builtin_amdgcn_workitem_id_x();
    uint gid = __builtin_amdgcn_workgroup_id_x() * block + lid;
    counter[gid] = counter[gid] + 1;
    if (gid == 0)
        log[idx] = idx + 1;
}
