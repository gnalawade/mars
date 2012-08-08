// (c) 2011 Thomas Schoebel-Theuer / 1&1 Internet AG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/fs.h>

#include <asm/atomic.h>

#include "brick_mem.h"
#include "brick_say.h"
#include "brick_locks.h"

#define BRICK_DEBUG_MEM 10000
#define USE_KERNEL_PAGES // currently mandatory (vmalloc does not work)
#define ALLOW_DYNAMIC_RAISE 512

#ifndef CONFIG_MARS_DEBUG
#undef BRICK_DEBUG_MEM
#endif

#define MAGIC_BLOCK  (int)0x8B395D7B
#define MAGIC_BEND   (int)0x8B395D7C
#define MAGIC_MEM    (int)0x8B395D7D
#define MAGIC_END    (int)0x8B395D7E
#define MAGIC_STR    (int)0x8B395D7F

#define INT_ACCESS(ptr,offset) (*(int*)(((char*)(ptr)) + (offset)))

#define _BRICK_FMT(_fmt) __BASE_FILE__ " %d %s(): " _fmt, __LINE__, __FUNCTION__
#define _BRICK_MSG(_class, _dump, PREFIX, _fmt, _args...) do { say(_class, PREFIX _BRICK_FMT(_fmt), ##_args); if (_dump) dump_stack(); } while (0)
#define BRICK_ERROR   "MEM_ERROR "
#define BRICK_WARNING "MEM_WARN  "
#define BRICK_INFO    "MEM_INFO  "
#define BRICK_ERR(_fmt, _args...) _BRICK_MSG(1,  true,  BRICK_ERROR,   _fmt, ##_args)
#define BRICK_WRN(_fmt, _args...) _BRICK_MSG(0,  false, BRICK_WARNING, _fmt, ##_args)
#define BRICK_INF(_fmt, _args...) _BRICK_MSG(-1, false, BRICK_INFO,    _fmt, ##_args)

/////////////////////////////////////////////////////////////////////////

// limit handling

#include <linux/swap.h>

long long brick_global_memavail = 0;
EXPORT_SYMBOL_GPL(brick_global_memavail);
long long brick_global_memlimit = 0;
EXPORT_SYMBOL_GPL(brick_global_memlimit);

void get_total_ram(void)
{
	struct sysinfo i = {};
	si_meminfo(&i);
	//si_swapinfo(&i);
	brick_global_memavail = (long long)i.totalram * (PAGE_SIZE / 1024);
	BRICK_INF("total RAM = %lld [KiB]\n", brick_global_memavail);
}

/////////////////////////////////////////////////////////////////////////

// small memory allocation (use this only for len < PAGE_SIZE)

#ifdef BRICK_DEBUG_MEM
static atomic_t mem_count[BRICK_DEBUG_MEM] = {};
static atomic_t mem_free[BRICK_DEBUG_MEM] = {};
static int  mem_len[BRICK_DEBUG_MEM] = {};
#define PLUS_SIZE (4 * sizeof(int))
#else
#define PLUS_SIZE (1 * sizeof(int))
#endif

static inline
void *__brick_mem_alloc(int len)
{
	void *res;
	if (len >= PAGE_SIZE) {
		res = _brick_block_alloc(0, len, 0);
	} else {
#ifdef CONFIG_MARS_MEM_RETRY
		for (;;) {
			res = kmalloc(len, GFP_BRICK);
			if (likely(res))
				break;
			msleep(1000);
		}
#else
		res = kmalloc(len, GFP_BRICK);
#endif
	}
	return res;
}

static inline
void __brick_mem_free(void *data, int len)
{
	if (len >= PAGE_SIZE) {
		_brick_block_free(data, len, 0);
	} else {
		kfree(data);
	}
}

void *_brick_mem_alloc(int len, int line)
{
	void *res;
#ifdef CONFIG_MARS_DEBUG
	might_sleep();
#endif

	res = __brick_mem_alloc(len + PLUS_SIZE);

	if (likely(res)) {
#ifdef BRICK_DEBUG_MEM
		if (unlikely(line < 0))
			line = 0;
		else if (unlikely(line >= BRICK_DEBUG_MEM))
			line = BRICK_DEBUG_MEM - 1;
		INT_ACCESS(res, 0 * sizeof(int)) = MAGIC_MEM;
		INT_ACCESS(res, 1 * sizeof(int)) = len;
		INT_ACCESS(res, 2 * sizeof(int)) = line;
		res += 3 * sizeof(int);
		INT_ACCESS(res, len) = MAGIC_END;
		atomic_inc(&mem_count[line]);
		mem_len[line] = len;
#else
		INT_ACCESS(res, 0 * sizeof(int)) = len;
		res += 1 * sizeof(int);
#endif
	}
	return res;
}
EXPORT_SYMBOL_GPL(_brick_mem_alloc);

void _brick_mem_free(void *data, int cline)
{
	if (data) {
#ifdef BRICK_DEBUG_MEM
		void *test = data - 3 * sizeof(int);
		int magic = INT_ACCESS(test, 0 * sizeof(int));
		int len   = INT_ACCESS(test, 1 * sizeof(int));
		int line  = INT_ACCESS(test, 2 * sizeof(int));
		if (unlikely(magic != MAGIC_MEM)) {
			BRICK_ERR("line %d memory corruption: magix %08x != %08x, len = %d\n", cline, magic, MAGIC_MEM, len);
			return;
		}
		if (unlikely(line < 0 || line >= BRICK_DEBUG_MEM)) {
			BRICK_ERR("line %d memory corruption: alloc line = %d, len = %d\n", cline, line, len);
			return;
		}
		INT_ACCESS(test, 0) = 0xffffffff;
		magic = INT_ACCESS(data, len);
		if (unlikely(magic != MAGIC_END)) {
			BRICK_ERR("line %d memory corruption: magix %08x != %08x, len = %d\n", cline, magic, MAGIC_END, len);
			return;
		}
		INT_ACCESS(data, len) = 0xffffffff;
		atomic_dec(&mem_count[line]);
		atomic_inc(&mem_free[line]);
#else
		void *test = data - 1 * sizeof(int);
		int len   = INT_ACCESS(test, 0 * sizeof(int));
#endif
		data = test;
		__brick_mem_free(data, len + PLUS_SIZE);
	}
}
EXPORT_SYMBOL_GPL(_brick_mem_free);

/////////////////////////////////////////////////////////////////////////

// string memory allocation

#ifdef BRICK_DEBUG_MEM
static atomic_t string_count[BRICK_DEBUG_MEM] = {};
static atomic_t string_free[BRICK_DEBUG_MEM] = {};
#endif

char *_brick_string_alloc(int len, int line)
{
	char *res;

#ifdef CONFIG_MARS_DEBUG
	might_sleep();
#endif

	if (len <= 0) {
		len = BRICK_STRING_LEN;
	}

#ifdef BRICK_DEBUG_MEM
	len += sizeof(int) * 4;
#endif

#ifdef CONFIG_MARS_MEM_RETRY
	for (;;) {
#endif
#ifdef CONFIG_MARS_DEBUG
		res = kzalloc(len + 1024, GFP_BRICK);
#else
		res = kzalloc(len, GFP_BRICK);
#endif
#ifdef CONFIG_MARS_MEM_RETRY
		if (likely(res))
			break;
		msleep(1000);
	}
#endif

#ifdef BRICK_DEBUG_MEM
	if (likely(res)) {
		if (unlikely(line < 0))
			line = 0;
		else if (unlikely(line >= BRICK_DEBUG_MEM))
			line = BRICK_DEBUG_MEM - 1;
		INT_ACCESS(res, 0) = MAGIC_STR;
		INT_ACCESS(res, sizeof(int)) = len;
		INT_ACCESS(res, sizeof(int) * 2) = line;
		INT_ACCESS(res, len - sizeof(int)) = MAGIC_END;
		atomic_inc(&string_count[line]);
		res += sizeof(int) * 3;
	}
#endif
	return res;
}
EXPORT_SYMBOL_GPL(_brick_string_alloc);

void _brick_string_free(const char *data, int cline)
{
	if (data) {
#ifdef BRICK_DEBUG_MEM
		int magic;
		int len;
		int line;

		data -= sizeof(int) * 3;
		magic = INT_ACCESS(data, 0);
		if (unlikely(magic != MAGIC_STR)) {
			BRICK_ERR("cline %d stringmem corruption: magix %08x != %08x\n", cline, magic, MAGIC_STR);
			return;
		}
		len =  INT_ACCESS(data, sizeof(int));
		line = INT_ACCESS(data, sizeof(int) * 2);
		if (unlikely(line < 0 || line >= BRICK_DEBUG_MEM)) {
			BRICK_ERR("cline %d stringmem corruption: line = %d (len = %d)\n", cline, line, len);
			return;
		}
		magic = INT_ACCESS(data, len - sizeof(int));
		if (unlikely(magic != MAGIC_END)) {
			BRICK_ERR("cline %d stringmem corruption: end_magix %08x != %08x, line = %d len = %d\n", cline, magic, MAGIC_END, len, line);
			return;
		}
		INT_ACCESS(data, len - sizeof(int)) = 0xffffffff;
		atomic_dec(&string_count[line]);
		atomic_inc(&string_free[line]);
#endif
		kfree(data);
	}
}
EXPORT_SYMBOL_GPL(_brick_string_free);

/////////////////////////////////////////////////////////////////////////

// block memory allocation

static
int len2order(int len)
{
	int order = 0;

	while ((PAGE_SIZE << order) < len)
		order++;

	if (unlikely(order > BRICK_MAX_ORDER || len <= 0)) {
		BRICK_ERR("trying to allocate %d bytes (max = %d)\n", len, (int)(PAGE_SIZE << order));
		return -1;
	}
	return order;
}

#ifdef BRICK_DEBUG_MEM
// indexed by line
static atomic_t block_count[BRICK_DEBUG_MEM] = {};
static atomic_t block_free[BRICK_DEBUG_MEM] = {};
static int  block_len[BRICK_DEBUG_MEM] = {};
// indexed by order
static atomic_t op_count[BRICK_MAX_ORDER+1] = {};
static atomic_t raw_count[BRICK_MAX_ORDER+1] = {};
static atomic_t alloc_count[BRICK_MAX_ORDER+1] = {};
static int alloc_max[BRICK_MAX_ORDER+1] = {};
static int alloc_line[BRICK_MAX_ORDER+1] = {};
#endif

static inline
void *__brick_block_alloc(gfp_t gfp, int order)
{
	void *res;
#ifdef BRICK_DEBUG_MEM
	atomic_inc(&raw_count[order]);
#endif
#ifdef CONFIG_MARS_MEM_RETRY
	for (;;) {
#endif
#ifdef USE_KERNEL_PAGES
		res = (void*)__get_free_pages(gfp, order);
#else
		res = __vmalloc(PAGE_SIZE << order, gfp, PAGE_KERNEL_IO);
#endif
#ifdef CONFIG_MARS_MEM_RETRY
		if (likely(res))
			break;
		msleep(1000);
	}
#endif
	return res;
}

static inline
void __brick_block_free(void *data, int order)
{
#ifdef USE_KERNEL_PAGES
	__free_pages(virt_to_page((unsigned long)data), order);
#else
	vfree(data);
#endif
#ifdef BRICK_DEBUG_MEM
	atomic_dec(&raw_count[order]);
#endif
}

bool brick_allow_freelist = true;
EXPORT_SYMBOL_GPL(brick_allow_freelist);

#ifdef CONFIG_MARS_MEM_PREALLOC
/* Note: we have no separate lists per CPU.
 * This should not hurt because the freelists are only used
 * for higher-order pages which should be rather low-frequency.
 */
static spinlock_t freelist_lock[BRICK_MAX_ORDER+1];
static void *brick_freelist[BRICK_MAX_ORDER+1] = {};
static atomic_t freelist_count[BRICK_MAX_ORDER+1] = {};
static int freelist_max[BRICK_MAX_ORDER+1] = {};

static
void *_get_free(int order)
{
	void *data;
	unsigned long flags;

	traced_lock(&freelist_lock[order], flags);
	data = brick_freelist[order];
	if (likely(data)) {
		void *next = *(void**)data;
#ifdef BRICK_DEBUG_MEM // check for corruptions
		void *copy = *(((void**)data)+1);
		if (unlikely(next != copy)) { // found a corruption
			// prevent further trouble by leaving a memleak
			brick_freelist[order] = NULL;
			traced_unlock(&freelist_lock[order], flags);
			BRICK_ERR("freelist corruption at %p (next %p != %p, murdered = %d), order = %d\n", data, next, copy, atomic_read(&freelist_count[order]), order);
			return NULL;
		}
#endif
		brick_freelist[order] = next;
		atomic_dec(&freelist_count[order]);
	}
	traced_unlock(&freelist_lock[order], flags);
	return data;
}

static
void _put_free(void *data, int order)
{
	void *next;
	unsigned long flags;

	traced_lock(&freelist_lock[order], flags);
	next = brick_freelist[order];
	*(void**)data = next;
#ifdef BRICK_DEBUG_MEM // insert redundant copy for checking
	*(((void**)data)+1) = next;
#endif
	brick_freelist[order] = data;
	traced_unlock(&freelist_lock[order], flags);
	atomic_inc(&freelist_count[order]);
}

static
void _free_all(void)
{
	int order;
	for (order = BRICK_MAX_ORDER; order >= 0; order--) {
		for (;;) {
			void *data = _get_free(order);
			if (!data)
				break;
			__brick_block_free(data, order);
		}
	}
}

int brick_mem_reserve(struct mem_reservation *r)
{
	int order;
	int status = 0;
	for (order = BRICK_MAX_ORDER; order >= 0; order--) {
		int max = r->amount[order];
		int i;

		freelist_max[order] += max;
		BRICK_INF("preallocating %d at order %d (new maxlevel = %d)\n", max, order, freelist_max[order]);

		max = freelist_max[order] - atomic_read(&freelist_count[order]);
		if (max >= 0) {
			for (i = 0; i < max; i++) {
				void *data = __brick_block_alloc(GFP_KERNEL, order);
				if (likely(data)) {
					_put_free(data, order);
				} else {
					status = -ENOMEM;
				}
			}
		} else {
			for (i = 0; i < -max; i++) {
				void *data = _get_free(order);
				if (likely(data)) {
					__brick_block_free(data, order);
				}
			}
		}
	}
	return status;
}
#else
int brick_mem_reserve(struct mem_reservation *r)
{
	BRICK_INF("preallocation is not compiled in\n");
	return 0;
}
#endif
EXPORT_SYMBOL_GPL(brick_mem_reserve);

void *_brick_block_alloc(loff_t pos, int len, int line)
{
	void *data;
#ifdef BRICK_DEBUG_MEM
	int count;
	const int plus = len <= PAGE_SIZE ? 0 : PAGE_SIZE * 2;
#else
	const int plus = 0;
#endif
	int order = len2order(len + plus);

	if (unlikely(order < 0)) {
		BRICK_ERR("trying to allocate %d bytes (max = %d)\n", len, (int)(PAGE_SIZE << order));
		return NULL;
	}

#ifdef CONFIG_MARS_DEBUG
	might_sleep();
#endif

#ifdef BRICK_DEBUG_MEM
	atomic_inc(&op_count[order]);
	atomic_inc(&alloc_count[order]);
	count = atomic_read(&alloc_count[order]);
	// statistics
	alloc_line[order] = line;
	if (count > alloc_max[order])
		alloc_max[order] = count;

	/* Dynamic increase of limits, in order to reduce
	 * fragmentation on higher-order pages.
	 * This comes on cost of higher memory usage.
	 */
#if defined(ALLOW_DYNAMIC_RAISE) && defined(CONFIG_MARS_MEM_PREALLOC)
	if (order > 0 && count > freelist_max[order] && count <= ALLOW_DYNAMIC_RAISE)
		freelist_max[order] = count;
#endif
#endif

#ifdef CONFIG_MARS_MEM_PREALLOC
	data = _get_free(order);
	if (!data)
#endif
		data = __brick_block_alloc(GFP_BRICK, order);

#ifdef BRICK_DEBUG_MEM
	if (likely(data) && order > 0) {
		if (unlikely(line < 0))
			line = 0;
		else if (unlikely(line >= BRICK_DEBUG_MEM))
			line = BRICK_DEBUG_MEM - 1;
		atomic_inc(&block_count[line]);
		block_len[line] = len;
		INT_ACCESS(data, 0) = MAGIC_BLOCK;
		INT_ACCESS(data, sizeof(int)) = line;
		INT_ACCESS(data, sizeof(int) * 2) = len;
		data += PAGE_SIZE;
		INT_ACCESS(data, len) = MAGIC_BEND;
	}
#endif
	return data;
}
EXPORT_SYMBOL_GPL(_brick_block_alloc);

void _brick_block_free(void *data, int len, int cline)
{
	int order;
#ifdef BRICK_DEBUG_MEM
	const int plus = len <= PAGE_SIZE ? 0 : PAGE_SIZE * 2;
#else
	const int plus = 0;
#endif
	if (!data) {
		return;
	}
	order = len2order(len + plus);
#ifdef BRICK_DEBUG_MEM
	if (order > 0) {
		void *test = data - PAGE_SIZE;
		int magic = INT_ACCESS(test, 0);
		int line = INT_ACCESS(test, sizeof(int));
		int oldlen = INT_ACCESS(test, sizeof(int)*2);
		int magic2;

		if (unlikely(magic != MAGIC_BLOCK)) {
			BRICK_ERR("line %d memory corruption: magix %08x != %08x\n", cline, magic, MAGIC_BLOCK);
			return;
		}
		if (unlikely(line < 0 || line >= BRICK_DEBUG_MEM)) {
			BRICK_ERR("line %d memory corruption: alloc line = %d\n", cline, line);
			return;
		}
		if (unlikely(oldlen != len)) {
			BRICK_ERR("line %d memory corruption: len != oldlen (%d != %d)\n", cline, len, oldlen);
			return;
		}
		magic2 = INT_ACCESS(data, len);
		if (unlikely(magic2 != MAGIC_BEND)) {
			BRICK_ERR("line %d memory corruption: magix %08x != %08x\n", cline, magic, MAGIC_BEND);
			return;
		}
		INT_ACCESS(test, 0) = 0xffffffff;
		INT_ACCESS(data, len) = 0xffffffff;
		atomic_dec(&block_count[line]);
		atomic_inc(&block_free[line]);
		data = test;
	}
#endif
#ifdef CONFIG_MARS_MEM_PREALLOC
	if (order > 0 && brick_allow_freelist && atomic_read(&freelist_count[order]) <= freelist_max[order]) {
		_put_free(data, order);
	} else
#endif
		__brick_block_free(data, order);

#ifdef BRICK_DEBUG_MEM
	atomic_dec(&alloc_count[order]);
#endif
}
EXPORT_SYMBOL_GPL(_brick_block_free);

struct page *brick_iomap(void *data, int *offset, int *len)
{
	int _offset = ((unsigned long)data) & (PAGE_SIZE-1);
	struct page *page;
	*offset = _offset;
	if (*len > PAGE_SIZE - _offset) {
		*len = PAGE_SIZE - _offset;
	}
	if (is_vmalloc_addr(data)) {
		page = vmalloc_to_page(data);
	} else {
		page = virt_to_page(data);
	}
	return page;
}
EXPORT_SYMBOL_GPL(brick_iomap);

/////////////////////////////////////////////////////////////////////////

// module

void brick_mem_statistics(void)
{
#ifdef BRICK_DEBUG_MEM
	int i;
	int count = 0;
	int places = 0;

	BRICK_INF("======== page allocation:\n");
#ifdef CONFIG_MARS_MEM_PREALLOC
	for (i = 0; i <= BRICK_MAX_ORDER; i++) {
		BRICK_INF("pages order = %2d "
			  "operations = %9d "
			  "freelist_count = %4d / %3d "
			  "raw_count = %5d "
			  "alloc_count = %5d "
			  "line = %5d "
			  "max_count = %5d\n",
			  i,
			  atomic_read(&op_count[i]),
			  atomic_read(&freelist_count[i]),
			  freelist_max[i],
			  atomic_read(&raw_count[i]),
			  atomic_read(&alloc_count[i]),
			  alloc_line[i], alloc_max[i]);
	}
#endif
	for (i = 0; i < BRICK_DEBUG_MEM; i++) {
		int val = atomic_read(&block_count[i]);
		if (val) {
			count += val;
			places++;
			BRICK_INF("line %4d: "
				  "%6d allocated "
				  "(last size = %4d, freed = %6d)\n",
				  i,
				  val,
				  block_len[i],
				  atomic_read(&block_free[i]));
		}
	}
	BRICK_INF("======== %d block allocations in %d places\n", count, places);
	count = places = 0;
	for (i = 0; i < BRICK_DEBUG_MEM; i++) {
		int val = atomic_read(&mem_count[i]);
		if (val) {
			count += val;
			places++;
			BRICK_INF("line %4d: "
				  "%6d allocated "
				  "(last size = %4d, freed = %6d)\n",
				  i,
				  val,
				  mem_len[i],
				  atomic_read(&mem_free[i]));
		}
	}
	BRICK_INF("======== %d memory allocations in %d places\n", count, places);
	count = places = 0;
	for (i = 0; i < BRICK_DEBUG_MEM; i++) {
		int val = atomic_read(&string_count[i]);
		if (val) {
			count += val;
			places++;
			BRICK_INF("line %4d: "
				  "%6d allocated "
				  "(freed = %6d)\n",
				  i,
				  val,
				  atomic_read(&string_free[i]));
		}
	}
	BRICK_INF("======== %d string allocations in %d places\n", count, places);
#endif
}
EXPORT_SYMBOL_GPL(brick_mem_statistics);

// module init stuff

int __init init_brick_mem(void)
{
#ifdef CONFIG_MARS_MEM_PREALLOC
	int i;
	for (i = BRICK_MAX_ORDER; i >= 0; i--) {
		spin_lock_init(&freelist_lock[i]);
	}
#endif

	get_total_ram();

	return 0;
}

void __exit exit_brick_mem(void)
{
#ifdef CONFIG_MARS_MEM_PREALLOC
	_free_all();
#endif

	brick_mem_statistics();
}

#ifndef CONFIG_MARS_HAVE_BIGMODULE
MODULE_DESCRIPTION("generic brick infrastructure");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_brick_mem);
module_exit(exit_brick_mem);
#endif
