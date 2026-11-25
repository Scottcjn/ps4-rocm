/* y = A x, one work item per row, A row-major: lanes of a wave read rows that
 * are n*4 bytes apart. */
kernel void matvec(__global const float *A, __global const float *x, __global float *y,
                   uint n, uint block)
{
    uint row = __builtin_amdgcn_workgroup_id_x() * block + __builtin_amdgcn_workitem_id_x();
    if (row >= n)
        return;
    __global const float *a = A + (ulong)row * n;
    float s = 0.0f;
    for (uint j = 0; j < n; j++)
        s += a[j] * x[j];
    y[row] = s;
}

/* Same product with A stored column-major (At[j*n + row] = A[row][j]): the 64
 * lanes of a wave read 64 consecutive floats per iteration. */
kernel void matvec_t(__global const float *At, __global const float *x, __global float *y,
                     uint n, uint block)
{
    uint row = __builtin_amdgcn_workgroup_id_x() * block + __builtin_amdgcn_workitem_id_x();
    if (row >= n)
        return;
    __global const float *a = At + row;
    float s = 0.0f;
    for (uint j = 0; j < n; j++)
        s += a[(ulong)j * n] * x[j];
    y[row] = s;
}
