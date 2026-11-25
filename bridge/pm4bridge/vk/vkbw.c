/* vkbw: GPU memory bandwidth (read / copy / write) through Vulkan compute, on
 * buffers placed in a chosen memory kind. Written because KFD cannot keep a
 * buffer in the VRAM carve-out on this machine (see README "VRAM vs GTT").
 *
 * usage: vkbw <MB> <src kind> <dst kind|none> [iters=64] [reps=5] [passes=4]
 *   kinds: vram   DEVICE_LOCAL | HOST_VISIBLE memory type
 *          gtt    HOST_VISIBLE, not DEVICE_LOCAL, HOST_CACHED
 *          gttwc  HOST_VISIBLE, not DEVICE_LOCAL, not cached (write-combined)
 * Each timed submit holds `passes` identical dispatches, bracketed by GPU
 * timestamps. Where the buffers really live is read from sysfs and DRM fdinfo.
 *
 * NEVER run without VK_ICD_FILENAMES pointing at the patched Mesa (the
 * program refuses to start otherwise): stock RADV hangs this GPU. */
#include <vulkan/vulkan.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("RESULT FAIL %s -> %d\n", #x, r_); exit(r_ == VK_ERROR_DEVICE_LOST ? 2 : 3); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static long long sysfs_ll(const char *n)
{
	char p[128], b[64] = ""; FILE *f;
	snprintf(p, sizeof p, "/sys/class/drm/card0/device/%s", n);
	if (!(f = fopen(p, "r"))) return -1;
	if (!fgets(b, sizeof b, f)) b[0] = 0;
	fclose(f); return atoll(b);
}

static void residency(const char *when)
{
	DIR *d = opendir("/proc/self/fdinfo"); struct dirent *e; char p[300], line[256];
	printf("[mem %s] sysfs vram_used %.2f MiB  gtt_used %.2f MiB\n", when,
	       sysfs_ll("mem_info_vram_used") / 1048576.0, sysfs_ll("mem_info_gtt_used") / 1048576.0);
	while (d && (e = readdir(d))) {
		FILE *f;
		if (e->d_name[0] == 46) continue;
		snprintf(p, sizeof p, "/proc/self/fdinfo/%s", e->d_name);
		if (!(f = fopen(p, "r"))) continue;
		while (fgets(line, sizeof line, f))
			if (!strncmp(line, "drm-memory-vram", 15) || !strncmp(line, "drm-memory-gtt", 14) ||
			    !strncmp(line, "amd-evicted-vram", 16) || !strncmp(line, "amd-requested-vram", 18))
				printf("    fd %s %s", e->d_name, line);
		fclose(f);
	}
	if (d) closedir(d);
}

static uint32_t pick_mem(uint32_t bits, const char *kind)
{
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		int local = !!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), vis = !!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
		    cached = !!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
		if (!(bits & (1u << i)) || !vis) continue;
		if (!strcmp(kind, "vram") && local) return i;
		if (!strcmp(kind, "gtt") && !local && cached) return i;
		if (!strcmp(kind, "gttwc") && !local && !cached) return i;
	}
	printf("RESULT FAIL no memory type for kind %s\n", kind); exit(3);
}

struct buf { VkBuffer b; VkDeviceMemory m; uint32_t *p; VkDeviceSize size; };

static void mkbuf(struct buf *o, VkDeviceSize size, const char *kind)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkMemoryRequirements mr; uint32_t t;
	CK(vkCreateBuffer(dev, &bi, 0, &o->b));
	vkGetBufferMemoryRequirements(dev, o->b, &mr);
	t = pick_mem(mr.memoryTypeBits, kind);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, t };
	CK(vkAllocateMemory(dev, &ai, 0, &o->m));
	CK(vkBindBufferMemory(dev, o->b, o->m, 0));
	CK(vkMapMemory(dev, o->m, 0, size, 0, (void **)&o->p));
	o->size = size;
	printf("  buffer %-5s %4llu MB -> memory type %u (flags %#x, heap %u)\n", kind,
	       (unsigned long long)(size >> 20), t, mp.memoryTypes[t].propertyFlags, mp.memoryTypes[t].heapIndex);
}

static VkPipelineLayout pl; static VkDescriptorSetLayout dl; static VkDescriptorPool dp;

static VkPipeline mkpipe(const char *path)
{
	FILE *f = fopen(path, "rb"); size_t sz; uint32_t *code;
	if (!f) { perror(path); exit(3); }
	fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
	code = malloc(sz); if (fread(code, 1, sz, f) != sz) exit(3); fclose(f);
	VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sz, code };
	VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, 0, &sm));
	VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", 0 }, pl };
	VkPipeline p; CK(vkCreateComputePipelines(dev, 0, 1, &cpi, 0, &p));
	return p;
}

static VkDescriptorSet mkset(struct buf *a, struct buf *b)
{
	VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dl };
	VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dsa, &ds));
	VkDescriptorBufferInfo i0 = { a->b, 0, a->size }, i1 = { b->b, 0, b->size };
	VkWriteDescriptorSet w[2] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &i0 },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &i1 } };
	vkUpdateDescriptorSets(dev, 2, w, 0, 0);
	return ds;
}

static VkQueue q; static VkCommandPool pool; static VkQueryPool qpool; static float ts_period; static int have_ts;

/* one submit of `passes` dispatches; returns CPU seconds, *gpu_s = GPU-timestamp seconds (0 if none) */
static double run(VkPipeline pipe, VkDescriptorSet ds, uint32_t groups, uint32_t iters, uint32_t value, int passes, double *gpu_s)
{
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	uint32_t pc[2] = { iters, value };
	CK(vkBeginCommandBuffer(cb, &cbi));
	if (have_ts) { vkCmdResetQueryPool(cb, qpool, 0, 2); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qpool, 0); }
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, 0);
	vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
	for (int i = 0; i < passes; i++) vkCmdDispatch(cb, groups, 1, 1);
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
	if (have_ts) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qpool, 1);
	CK(vkEndCommandBuffer(cb));
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; CK(vkCreateFence(dev, &fi, 0, &fence));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
	double t0 = now_s();
	CK(vkQueueSubmit(q, 1, &si, fence));
	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 15000000000ull);
	double t = now_s() - t0;
	if (wr != VK_SUCCESS) { printf("RESULT HANG (wait=%d)\n", wr); fflush(stdout); _exit(2); }
	*gpu_s = 0;
	if (have_ts) {
		uint64_t ts[2] = { 0, 0 };
		if (vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
			*gpu_s = (double)(ts[1] - ts[0]) * ts_period * 1e-9;
	}
	vkDestroyFence(dev, fence, 0); vkFreeCommandBuffers(dev, pool, 1, &cb);
	return t;
}

struct st { double sum, best, gsum; int n; };
static void st_add(struct st *s, double t, double g) { if (!s->n || t < s->best) s->best = t; s->sum += t; s->gsum += g; s->n++; }
static void st_print(const char *what, const char *sk, const char *dk, double bytes, uint32_t iters, int passes, struct st *s, uint32_t bad)
{
	double tot = bytes * passes, g = s->gsum / s->n;
	printf("SUMMARY vk %-5s src=%-5s dst=%-5s %3.0f MB x%d iters %3u: mean %6.2f GB/s  best %6.2f GB/s  gpu-timed %6.2f GB/s  (n=%d, mismatches %u)\n",
	       what, sk, dk, bytes / 1048576.0, passes, iters, tot / (s->sum / s->n) / 1e9, tot / s->best / 1e9,
	       g > 0 ? tot / g / 1e9 : 0.0, s->n, bad);
}

int main(int argc, char **argv)
{
	const char *icd = getenv("VK_ICD_FILENAMES");
	if (!icd || !strstr(icd, "mesa-ps4")) { printf("RESULT FAIL refusing to run: VK_ICD_FILENAMES is not the patched Mesa\n"); return 3; }
	if (argc < 4) { printf("usage: vkbw <MB> <src kind> <dst kind|none> [iters] [reps] [passes]\n"); return 3; }
	size_t bytes = (size_t)strtoul(argv[1], 0, 0) << 20;
	const char *sk = argv[2], *dk = argv[3];
	uint32_t iters = argc > 4 ? strtoul(argv[4], 0, 0) : 64;
	int reps = argc > 5 ? atoi(argv[5]) : 5, passes = argc > 6 ? atoi(argv[6]) : 4, pass = 1, have_dst = strcmp(dk, "none") != 0;
	uint32_t ndw = bytes / 4, groups = ndw / (256u * iters), i, bad;
	if (!bytes || !iters || ndw % (256u * iters)) { printf("RESULT FAIL bad size/iters\n"); return 3; }

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkbw", 1, 0, 0, VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
	VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
	uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
	ts_period = pp.limits.timestampPeriod;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	printf("device %s, timestampPeriod %.3f ns\n", pp.deviceName, ts_period);
	for (i = 0; i < mp.memoryHeapCount; i++)
		printf("  heap %u: %llu MiB flags %#x\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
	for (i = 0; i < mp.memoryTypeCount; i++)
		printf("  type %u: heap %u flags %#x%s%s%s%s\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags,
		       mp.memoryTypes[i].propertyFlags & 1 ? " DEVICE_LOCAL" : "", mp.memoryTypes[i].propertyFlags & 2 ? " HOST_VISIBLE" : "",
		       mp.memoryTypes[i].propertyFlags & 4 ? " HOST_COHERENT" : "", mp.memoryTypes[i].propertyFlags & 8 ? " HOST_CACHED" : "");

	uint32_t qn = 8; VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
	int qf = -1;
	for (i = 0; i < qn; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }  /* same choice as vkbisect "any" */
	if (qf < 0) return 3;
	have_ts = qp[qf].timestampValidBits != 0 && !getenv("VKBW_NO_TS");
	printf("queue family %d flags %#x timestampValidBits %u\n", qf, qp[qf].queueFlags, qp[qf].timestampValidBits);
	float pri = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, (uint32_t)qf, 1, &pri };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci };
	CK(vkCreateDevice(pd, &dci, 0, &dev)); vkGetDeviceQueue(dev, qf, 0, &q);
	residency("after device create");

	struct buf S, D = { 0 }, O;
	mkbuf(&S, bytes, sk);
	residency("after src alloc");
	if (have_dst) { mkbuf(&D, bytes, dk); residency("after dst alloc"); }
	mkbuf(&O, (VkDeviceSize)groups * 256 * 4, "gtt");

	uint32_t *ref = malloc(bytes), *chk = malloc(bytes);
	for (i = 0; i < ndw; i++) ref[i] = i * 2654435761u + 12345u;
	memcpy(S.p, ref, bytes);
	if (have_dst) memset(D.p, 0, bytes);
	residency("after CPU fill");

	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, (uint32_t)qf };
	CK(vkCreateCommandPool(dev, &pci, 0, &pool));
	VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0 };
	if (have_ts) CK(vkCreateQueryPool(dev, &qpi, 0, &qpool));
	VkDescriptorSetLayoutBinding bd[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 },
					       { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 } };
	VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, bd };
	CK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dl));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dl, 1, &pcr };
	CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 };
	VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4, 1, &ps };
	CK(vkCreateDescriptorPool(dev, &dpi, 0, &dp));
	VkPipeline pcopy = mkpipe("bw_copy.spv"), psum = mkpipe("bw_sum.spv"), pfill = mkpipe("bw_fill.spv");
	VkDescriptorSet ds_sum = mkset(&S, &O), ds_cp = have_dst ? mkset(&S, &D) : 0;

	struct st st; double t, g; int rep;

	/* submit overhead: one tiny dispatch (1 group) */
	memset(&st, 0, sizeof st);
	for (rep = 0; rep < 5; rep++) { t = run(psum, ds_sum, 1, iters, 0, 1, &g); st_add(&st, t, g); }
	printf("submit overhead (1 group): cpu mean %.1f us best %.1f us, gpu-timed mean %.1f us\n", st.sum / st.n * 1e6, st.best * 1e6, st.gsum / st.n * 1e6);

	/* read only */
	memset(&st, 0, sizeof st);
	for (rep = 0; rep < reps; rep++) {
		memset(O.p, 0, O.size);
		t = run(psum, ds_sum, groups, iters, 0, passes, &g); st_add(&st, t, g);
		printf("read  rep %d: %8.3f ms %6.2f GB/s (gpu-timed %8.3f ms)\n", rep, t * 1e3, (double)bytes * passes / t / 1e9, g * 1e3);
	}
	bad = 0;
	for (uint32_t gi = 0; gi < groups; gi++)
		for (uint32_t l = 0; l < 256; l++) {
			uint32_t idx = gi * 256u * iters + l, sum = 0, k;
			for (k = 0; k < iters; k++, idx += 256) sum += ref[idx];
			if (O.p[gi * 256 + l] != sum && bad++ < 3) printf("  sum [%u,%u] wrong\n", gi, l);
		}
	pass &= !bad;
	st_print("read", sk, "-", bytes, iters, passes, &st, bad);

	if (have_dst) {
		memset(&st, 0, sizeof st); bad = 0;
		for (rep = 0; rep < reps; rep++) {
			memset(D.p, 0, bytes);
			t = run(pcopy, ds_cp, groups, iters, 0, passes, &g); st_add(&st, t, g);
			printf("copy  rep %d: %8.3f ms %6.2f GB/s copied (gpu-timed %8.3f ms)\n", rep, t * 1e3, (double)bytes * passes / t / 1e9, g * 1e3);
			memcpy(chk, D.p, bytes);
			for (i = 0; i < ndw; i++) if (chk[i] != ref[i] && bad++ < 3) printf("  copy [%u] = %#x want %#x\n", i, chk[i], ref[i]);
		}
		pass &= !bad;
		st_print("copy", sk, dk, bytes, iters, passes, &st, bad);

		memset(&st, 0, sizeof st);
		uint32_t value = 0;
		for (rep = 0; rep < reps; rep++) {
			value = 0x1000u * (rep + 1);
			t = run(pfill, ds_cp, groups, iters, value, passes, &g); st_add(&st, t, g);
			printf("write rep %d: %8.3f ms %6.2f GB/s (gpu-timed %8.3f ms)\n", rep, t * 1e3, (double)bytes * passes / t / 1e9, g * 1e3);
		}
		bad = 0; memcpy(chk, D.p, bytes);
		for (i = 0; i < ndw; i++) if (chk[i] != value + i && bad++ < 3) printf("  fill [%u] = %#x want %#x\n", i, chk[i], value + i);
		pass &= !bad;
		st_print("write", "-", dk, bytes, iters, passes, &st, bad);
	}
	memcpy(chk, S.p, bytes);
	printf("source intact after runs: %s\n", memcmp(chk, ref, bytes) ? (pass = 0, "NO") : "yes");
	residency("after GPU runs");
	vkDeviceWaitIdle(dev);
	printf("RESULT %s vkbw %s->%s %zu MB\n", pass ? "PASS" : "FAIL", sk, dk, bytes >> 20);
	fflush(stdout);
	return !pass;
}
