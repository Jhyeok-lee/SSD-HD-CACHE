#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>

/* This is a structure which will store  information about the underlying device 
 * Param:
 * dev : underlying device
 * start:  Starting sector number of the device
 */

struct oscache_dm_target {
       struct dm_dev* dev;
       sector_t start;
};

/* This is map function of basic target. This function gets called whenever you get a new bio
 * request.The working of map function is to map a particular bio request to the underlying device. 
 * The request that we receive is submitted to out device so  bio->bi_bdev points to our device.
 * We should point to the bio-> bi_dev field to bdev of underlying device. Here in this function,
 * we can have other processing like changing sector number of bio request, splitting bio etc. 
 *
 *  Param : 
 *  ti : It is the dm_target structure representing our basic target
 *  bio : The block I/O request from upper layer
 *  map_context : Its mapping context of target.
 *
 *: Return values from target map function:
 *  DM_MAPIO_SUBMITTED :  Your target has submitted the bio request to underlying request
 *  DM_MAPIO_REMAPPED  :  Bio request is remapped, Device mapper should submit bio.  
 *  DM_MAPIO_REQUEUE   :  Some problem has happened with the mapping of bio, So 
 *                        re queue the bio request. So the bio will be submitted 
 *                        to the map function  
 */

static int oscache_target_map(struct dm_target* ti, struct bio* bio, union map_info* map_context) {

    struct oscache_dm_target* odt = (struct oscache_dm_target*) ti->private;
    printk("In function : oscache_target_map()\n");

    bio->bi_bdev = odt->dev->bdev;

    if( (bio->bi_rw & WRITE) == WRITE )
        printk("oscache_dm_target() : bio is a write request\n");
    else
        printk("oscache_dm_target() : bio is a read request\n");

    submit_bio(bio->bi_rw, bio);

    printk("Out function : oscache_target_map()\n");

    return DM_MAPIO_SUBMITTED;
}

/* This is Constructor Function of basic target 
 * Constructor gets called when we create some device of type 'basic_target'.
 * So it will get called when we execute command 'dmsetup create'
 * This function gets called for each device over which you want to create basic 
 * target. Here it is just a basic target so it will take only one device so it  
 * will get called once. 
 */

static int oscache_target_ctr(struct dm_target* ti, unsigned int argc, char** argv) {

    struct oscache_dm_target* odt;
    unsigned long long start;

    printk("In function : oscache_target_ctr\n");

    if( argc != 2 ) {
        printk("Invalid Number of arguments\n");
        ti->error = "Invalid arg count";
        return -EINVAL;
    }

    odt = kmalloc( sizeof(struct oscache_dm_target), GFP_KERNEL );

    if( odt == NULL ) {
        printk("odt is null\n");
        ti->error = "oscache_target : Cannot allocate oscache_dm_target\n";
        return -ENOMEM;
    }


    if( sscanf(argv[1], "%llu", &start) != 1 ) {
        printk("Invalid device secter\n");
        ti->error = "oscache_target : Invalid device sector\n";
        goto bad;
    }

    /* dm_table_get_mode 
     * Gives out you the Permissions of device mapper table. 
     * This table is nothing but the table which gets created
     * when we execute dmsetup create. This is one of the
     * Data structure used by device mapper for keeping track of its devices.
     *
     * dm_get_device 
     * The function sets the mdt->dev field to underlying device dev structure.
     */

    if( dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &odt->dev) ) {
        printk("Device lookup failed\n");
        ti->error = "oscache_target : Device lookup failed\n";
        goto bad;
    }

    ti->private = odt;

    printk("Out function : oscache_target_ctr\n");

    return 0;

bad:
    kfree(odt);
    printk("Out function : oscache_target_ctr with ERROR\n");
}

/*
 * This is destruction function
 * This gets called when we remove a device of type basic target. The function gets 
 * called per device. 
 */

static void oscache_target_dtr(struct dm_target* ti) {

    struct oscache_dm_target* odt = (struct oscache_dm_target*) ti->private;
    printk("In function : oscache_target_dtr\n");
    dm_put_device(ti, odt->dev);
    kfree(odt);
    printk("Out function : oscache_target_dtr\n");
}

/*
 * This structure is fops for basic target.
 */

static struct target_type oscache_target = {

    .name = "oscache_target",
    .version = {1,0,0},
    .module = THIS_MODULE,
    .ctr = oscache_target_ctr,
    .dtr = oscache_target_dtr,
    .map = oscache_target_map,
};

/*---------Module Functions -----------------*/

static int __init_oscache_target(void) {

    int result;

    result = dm_register_target(&oscache_target);

    if( result<0 ) printk("Error in registering target\n");

    return 0;
}

static void __exit_oscache_target(void) {

    dm_unregister_target(&oscache_target);
}

module_init(__init_oscache_target);
module_exit(__exit_oscache_target);
MODULE_LICENSE("GPL");
