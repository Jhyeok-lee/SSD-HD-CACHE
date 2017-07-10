#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/wait.h>

//default value
#define DEFAULT_CACHE_SIZE		65536				//65536 blocks -> 256MB (if 4KB per block)
#define DEFAULT_BLOCK_SIZE		8					//8 sectors -> 1 block -> 4KB
#define DEFAULT_CACHE_ASSOC		512					//512 blocks
#define DEFAULT_DISK_ASSOC		512					//512 blocks

//cache state
#define BLOCK_NODATA		0
#define BLOCK_USE			1
#define BLOCK_USELESS		2

//operation mode
#define WRITE_THROUGH		0
#define WRITE_BACK			1

//policy
#define FIFO				0
#define LRU					1

//io case
#define READ_HIT			0
#define READ_MISS			1
#define WRITE_HIT			2
#define WRITE_MISS			3
#define COMPLETE			4

//number of pages for I/O
#define OSCACHE_PAGES		1024

#define bio_barrier(bio)	( (bio)->bi_rw & REQ_FLUSH )

// cache block information
struct cache_block {
	spinlock_t lock;
	sector_t start_sector;
	unsigned int state;
	unsigned int hit_count;
	struct bio_list bios;
};

// target information
struct oscache {
	struct dm_target* ti;

    struct dm_dev* origin_dev;
	struct dm_dev* cache_dev;

	struct cache_block* cache_table;				// cache table in memory
	unsigned int cache_table_count;					// number of cache block

	unsigned int cache_size;						// cache size (in blocks)
	unsigned int block_size;						// block size (in sectors)
	unsigned int block_shift;						// block shift : sectors >> block_shift = blocks, blocks << block_shift = sectors

	unsigned int cache_assoc;						// cache associativity
	unsigned int cache_assoc_shift;					// cache assoc shift
	unsigned int disk_assoc;						// disk associativity
	unsigned int disk_assoc_shift;					// disk assoc shift
	unsigned int cache_set;							// the number of cache sets

	unsigned int operation_mode;
	unsigned int policy;

	struct dm_io_client* io_client;					// client memory handler
};

//////////////////////////////////////////////////////////////////////////////////////////
//                                jobs configuration area                               //
//////////////////////////////////////////////////////////////////////////////////////////

#define JOBS 1024

struct oscache_job {
	struct list_head list;
	struct oscache* cache;
	struct bio* bio;
	struct dm_io_region origin_region;
	struct dm_io_region cache_region;
	struct cache_block* block;
	int io_case;
};

static struct kmem_cache *_job_cache;
static mempool_t *_job_pool;

static DEFINE_SPINLOCK(_job_lock);

static LIST_HEAD(_complete_jobs);
static LIST_HEAD(_io_jobs);

// create mempool to job_pool
static int oscache_jobs_init(void) {

	_job_cache = kmem_cache_create("oscache-jobs", sizeof(struct oscache_job),
									__alignof__(struct oscache_job), 0, NULL);
	if (!_job_cache)
		return -ENOMEM;

	_job_pool = mempool_create(JOBS, mempool_alloc_slab, mempool_free_slab, _job_cache);
	if (!_job_pool) {
		kmem_cache_destroy(_job_cache);
		return -ENOMEM;
	}

	return 0;
}

static void oscache_jobs_exit(void) {

	BUG_ON(!list_empty(&_complete_jobs));
	BUG_ON(!list_empty(&_io_jobs));

	mempool_destroy(_job_pool);
	kmem_cache_destroy(_job_cache);
	_job_pool = NULL;
	_job_cache = NULL;
}

// allocate new job
static struct oscache_job* new_oscache_job(struct oscache *cache, struct bio* bio
											,unsigned int index) {

	struct dm_io_region origin_region;
	struct dm_io_region cache_region;
	struct oscache_job *job;

	origin_region.bdev = cache->origin_dev->bdev;
	origin_region.sector = bio->bi_iter.bi_sector;
	origin_region.count = cache->block_size;

	cache_region.bdev = cache->cache_dev->bdev;
	cache_region.sector = cache->cache_table[index].start_sector;
	cache_region.count = cache->block_size;

	job = mempool_alloc(_job_pool, GFP_NOIO);
	job->cache = cache;
	job->bio = bio;
	job->origin_region = origin_region;
	job->cache_region = cache_region;
	job->block = &cache->cache_table[index];


	return job;
}

static int dm_io_async_bio(unsigned int num_regions, struct dm_io_region* where,
		                    int rw, struct bio* bio, io_notify_fn fn, void* context);
void oscache_io_callback(unsigned long error, void* context);

////////////////////////////////////////////////////////////////////////////////////
//						work queue interface for deferring work					  //
////////////////////////////////////////////////////////////////////////////////////

static struct workqueue_struct* _oscache_thread;	// oscache thread for work
static struct work_struct _oscache_work;			// works

static inline struct oscache_job* pop(struct list_head *jobs)
{
	struct oscache_job *job = NULL;
	unsigned long flags;

	spin_lock_irqsave(&_job_lock, flags);
	if (!list_empty(jobs)) {
		job = list_entry(jobs->next, struct oscache_job, list);
		list_del(&job->list);
	}
	spin_unlock_irqrestore(&_job_lock, flags);

	return job;
}

static inline void push(struct list_head *jobs, struct oscache_job *job)
{
	unsigned long flags;

	spin_lock_irqsave(&_job_lock, flags);
	list_add_tail(&job->list, jobs);
	spin_unlock_irqrestore(&_job_lock, flags);
}

static int do_complete(struct oscache_job* job) {


	int r = 0;
	struct bio *bio = job->bio;


	bio_endio(bio);

	mempool_free(job, _job_pool);


	return r;
}

static int do_io(struct oscache_job* job) {
	

	int r = 0;
	unsigned int io_case;

	io_case = job->io_case;
	job->io_case = COMPLETE;


	if( io_case == READ_HIT )
		do_complete(job);

	if( io_case == READ_MISS )
		dm_io_async_bio(1, &(job->cache_region), WRITE, job->bio, oscache_io_callback, job);

	if( io_case == WRITE_HIT ) {
		if( job->cache->operation_mode == WRITE_THROUGH )
			dm_io_async_bio(1, &(job->cache_region), WRITE, job->bio, oscache_io_callback, job);

		if( job->cache->operation_mode == WRITE_BACK )
			dm_io_async_bio(1, &(job->origin_region), WRITE, job->bio, oscache_io_callback, job);
	}

	if( io_case == WRITE_MISS )
		dm_io_async_bio(1, &(job->cache_region), WRITE, job->bio, oscache_io_callback, job);

	return r;
}

static int process_jobs(struct list_head *jobs, int (*fn)(struct oscache_job*))
{
	struct oscache_job *job;
	int r, count = 0;

	while ((job = pop(jobs))) {
		r = fn(job);

		if (r < 0) {
			/* error this rogue job */
			//DMERR("process_jobs: Job processing error");
		}

		if (r > 0) {
			/*
	 		 * We couldn't service this job ATM, so
			 * push this job back onto the list.
			 */
			push(jobs, job);
			break;
		}

		count++;
	}

	return count;
}

static void oscache_work(struct work_struct* jaehyeok) {

	process_jobs(&_complete_jobs, do_complete);
	process_jobs(&_io_jobs, do_io);
}

/////////////////////////////////////////////////////////////////////////////////////
//							handling I/O functions                                 //
/////////////////////////////////////////////////////////////////////////////////////
static int dm_io_sync_bio(unsigned int num_regions, struct dm_io_region* where,
							int rw, struct bio* bio, io_notify_fn fn, void* context) {

	struct dm_io_request iorq;
	struct oscache_job* job;
	struct oscache* cache;

	job = (struct oscache_job*)context;
	cache = job->cache;

	iorq.bi_rw = (rw | REQ_SYNC);
	iorq.mem.type = DM_IO_BIO;
	iorq.mem.ptr.bio = bio;
	iorq.notify.fn = fn;
	iorq.notify.context = context;
	iorq.client = cache->io_client;

	return dm_io(&iorq, num_regions, where, NULL);
}

static int dm_io_async_bio(unsigned int num_regions, struct dm_io_region* where,
							int rw, struct bio* bio, io_notify_fn fn, void* context) {
	
	struct dm_io_request iorq;
	struct oscache_job* job;
	struct oscache* cache;

	job = (struct oscache_job*)context;
	cache = job->cache;

	iorq.bi_rw = (rw | (1 << REQ_SYNC));
	iorq.mem.type = DM_IO_BIO;
	iorq.mem.ptr.bio = bio;
	iorq.notify.fn = fn;
	iorq.notify.context = context;
	iorq.client = cache->io_client;

	return dm_io(&iorq, num_regions, where, NULL);
}

static inline void wake(void)
{
	queue_work(_oscache_thread, &_oscache_work);
}

void oscache_io_callback(unsigned long error, void* context) {


	struct oscache_job* job = (struct oscache_job*)context;

	if( error )
		return;

	if( job->io_case == COMPLETE )
		push(&_complete_jobs, job);
	else
		push(&_io_jobs, job);


	wake();
}

//find set_number
int find_set_number(struct oscache* cache, sector_t request_sector) {

	unsigned int request_block;
	unsigned int set_number;

	request_block = request_sector >> cache->block_shift;
	set_number = (request_block >> cache->cache_assoc_shift) % cache->cache_set;

	return set_number;
}


//fifo
void oscache_fifo(struct oscache* cache, unsigned int set_number, unsigned int* index) {


	unsigned int i;
	unsigned int first;
	unsigned int last;
	struct cache_block* table;

	table = cache->cache_table;
	first = (set_number-1) << (cache->cache_assoc_shift);
	last = (set_number << (cache->cache_assoc_shift));

	for( i=first; i < last; i++)
		table[i].state = BLOCK_USELESS;

	table[first].state = BLOCK_USE;
	*index = first;


}

/*
//lru
void oscache_lru(struct oscache* cache, unsigned int* index ) {

}
*/

// lookup requested block in cache_table
// requested block exists in cache_table -> return 1,index
// no exists -> return 0, index
static int oscache_lookup(struct oscache* cache, struct bio* bio, unsigned int* index) {


	unsigned int i;
	unsigned int set_number;
	struct cache_block* table;
	sector_t request_sector;

	table = cache->cache_table;
	request_sector = bio->bi_iter.bi_sector;
	set_number = find_set_number(cache, request_sector);

	for( i = set_number << (cache->cache_assoc_shift); i < (set_number+1 << (cache->cache_assoc_shift)) ; i++) {
		if( table[i].state == BLOCK_NODATA || table[i].state == BLOCK_USELESS)
			goto miss;
		if( table[i].start_sector == request_sector ) {
			*index = i;
			return 1; //hit
		}
	}

	goto full_miss;

miss:

	table[i].state = BLOCK_USE;
	*index = i;


	return 0; // miss

full_miss:


	if( cache->policy == FIFO )
		oscache_fifo(cache, set_number, index);

	if( cache->policy == LRU )
		//oscache_lru(cache, request_sector, index);

	return 0; // miss
}

void oscache_read_hit(struct oscache* cache, struct bio* bio, unsigned int* index) {

	struct oscache_job* job;

	job = new_oscache_job(cache, bio, *index);

	job->io_case = READ_HIT;

	dm_io_sync_bio(1, &(job->cache_region), READ, bio, oscache_io_callback, job);
}

void oscache_read_miss(struct oscache* cache, struct bio* bio, unsigned int* index) {

	struct oscache_job* job;

	job = new_oscache_job(cache, bio, *index);

	job->io_case = READ_MISS;

	dm_io_sync_bio(1, &(job->origin_region), READ, bio, oscache_io_callback, job);
}

void oscache_write_hit(struct oscache* cache, struct bio* bio, unsigned int* index) {

	struct oscache_job* job;

	job = new_oscache_job(cache, bio, *index);

	job->io_case = WRITE_HIT;

	if( cache->operation_mode == WRITE_THROUGH )
		dm_io_async_bio(1, &(job->origin_region), WRITE, bio, oscache_io_callback, job);

	if( cache->operation_mode == WRITE_BACK )
		dm_io_async_bio(1, &(job->cache_region), WRITE, bio, oscache_io_callback, job);
}

void oscache_write_miss(struct oscache* cache, struct bio* bio, unsigned int* index) {

	struct oscache_job* job;

	job = new_oscache_job(cache, bio, *index);

	job->io_case = WRITE_MISS;

	dm_io_async_bio(1, &(job->origin_region), WRITE, bio, oscache_io_callback, job);
}

#define VERIFY(x) do { \
	if (unlikely(!(x))) { \
		dump_stack(); \
		panic("VERIFY: assertion (%s) failed at %s (%d)\n", \
		      #x,  __FILE__ , __LINE__);		    \
	} \
} while(0)

static void
flashcache_do_block_checks(struct oscache *dmc, struct bio *bio)
{
	sector_t mask;
	sector_t io_start;
	sector_t io_end;

	VERIFY(to_sector(bio->bi_iter.bi_size) <= dmc->block_size);
	mask = ~((1 << dmc->block_shift) - 1);
	io_start = bio->bi_iter.bi_sector & mask;
	io_end = (bio->bi_iter.bi_sector + (to_sector(bio->bi_iter.bi_size) - 1)) & mask;
	/* The incoming bio must NOT straddle a blocksize boundary */
	VERIFY(io_start == io_end);
}


// map function -> when I/O requested
static int oscache_map(struct dm_target* ti, struct bio* bio) {

    struct oscache* cache = (struct oscache*) ti->private;

	unsigned int index;
	int result;

	flashcache_do_block_checks(cache, bio);

	if(bio_barrier(bio))
		return -EOPNOTSUPP;

	result = oscache_lookup(cache, bio, &index);

    if( bio_data_dir(bio) == READ ) { // read request
		if( result == 1 )
			oscache_read_hit(cache, bio, &index);
		else
			oscache_read_miss(cache, bio, &index);
	}
    else { // write request
		if( result == 1 )
			oscache_write_hit(cache, bio, &index);
		else
			oscache_write_miss(cache, bio, &index);
	}

    return DM_MAPIO_SUBMITTED;
}

// construction function
static int oscache_ctr(struct dm_target* ti, unsigned int argc, char** argv) {

	int result;
	int i;
    struct oscache* cache;

    if( argc < 2 ) {
        printk("Invalid Number of arguments\n");
        ti->error = "Invalid arguments count";
        return -EINVAL;
    }

    cache = kmalloc( sizeof(struct oscache), GFP_KERNEL );

    if( cache == NULL ) {
        printk("cache is null\n");
        ti->error = "oscache : Cannot allocate oscache\n";
        return -ENOMEM;
    }

	// dm_get_device(dm_target, path, mode, device)
	// set origin_dev, cache_dev
    if( dm_get_device(ti, argv[0], FMODE_READ | FMODE_WRITE, &cache->origin_dev) ) {
        ti->error = "oscache : set origin_dev fail\n";
        goto bad;
    }

	if( dm_get_device(ti, argv[1], FMODE_READ | FMODE_WRITE, &cache->cache_dev) ) {
		ti->error = "oscache : set cache_dev fail\n";
		goto bad;
	}

	// set cache size in blocks
	if( argc > 2 ) {
		if( sscanf(argv[2],"%u",&cache->cache_size) != 1 ) {
			ti->error = "oscache : invalid cache size";
			goto bad;
		}
	}
	else 
		cache->cache_size = DEFAULT_CACHE_SIZE;

	// set block size in sectors
	if( argc > 3 ) {
		if( sscanf(argv[3],"%u",&cache->block_size) != 1 ) {
			ti->error = "oscache : invalid cache_block size";
			goto bad;
		}
	}
	else 
		cache->block_size = DEFAULT_BLOCK_SIZE;

	cache->block_shift = ffs(cache->block_size)-1;

	cache->cache_assoc = DEFAULT_CACHE_ASSOC;
	cache->disk_assoc = DEFAULT_DISK_ASSOC;
	cache->cache_assoc_shift = ffs(cache->cache_assoc)-1;
	cache->disk_assoc_shift = ffs(cache->disk_assoc)-1;
	cache->cache_set = cache->cache_size >> cache->cache_assoc_shift;

	cache->io_client = dm_io_client_create();
	if( IS_ERR(cache->io_client) ) {
		result = PTR_ERR(cache->io_client);
		ti->error = "oscache : fail to create io_client";
		goto bad;
	}

	cache->cache_table = (struct cache_block*)vmalloc( cache->cache_size * sizeof(struct cache_block));
	
	cache->operation_mode = WRITE_THROUGH;
	cache->policy = FIFO;
	cache->cache_table_count = 0;

    ti->private = cache;
	cache->ti = ti;

	for(i=0; i < cache->cache_size ; i++) {
		cache->cache_table[i].start_sector = i << cache->block_shift;
		cache->cache_table[i].state = BLOCK_NODATA;
		cache->cache_table[i].hit_count = 0;
	}

    printk("Out function : oscache_ctr\n");

    return 0;

bad:
	dm_put_device(ti, cache->origin_dev);
	dm_put_device(ti, cache->cache_dev);
	dm_io_client_destroy(cache->io_client);
	//kcached_destroy(cache);
    kfree(cache);
    printk("Out function : oscache_ctr with ERROR\n");

	return -ENOMEM;
}


// destruction function
static void oscache_dtr(struct dm_target* ti) {

    struct oscache* cache = (struct oscache*) ti->private;

    dm_put_device(ti, cache->origin_dev);
	dm_put_device(ti, cache->cache_dev);
	dm_io_client_destroy(cache->io_client);
	//kcached_destroy(cache);
    kfree(cache);
}


// file operations
static struct target_type oscache_target = {

    .name = "oscache",
    .version = {1,0,0},
    .module = THIS_MODULE,
    .ctr = oscache_ctr,
    .dtr = oscache_dtr,
    .map = oscache_map,
};


// module functions
static int __init_oscache(void) {

    int result;

	result = oscache_jobs_init();
	if(result)
		return result;

	_oscache_thread = create_singlethread_workqueue("oscache_thread");
	if( !_oscache_thread )
		return -ENOMEM;
	INIT_WORK(&_oscache_work, oscache_work);


    result = dm_register_target(&oscache_target);
    if( result<0 ) {
		printk("Error in registering target\n");
		destroy_workqueue(_oscache_thread);
	}

    return 0;
}

static void __exit_oscache(void) {

    dm_unregister_target(&oscache_target);

	oscache_jobs_exit();

	destroy_workqueue(_oscache_thread);
}

module_init(__init_oscache);
module_exit(__exit_oscache);
MODULE_AUTHOR("Lee Jaehyeok <wogur0310@gmail.com>");
MODULE_LICENSE("GPL");
