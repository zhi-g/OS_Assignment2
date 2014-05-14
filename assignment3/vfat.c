// vim: noet:ts=8:sts=8
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <sys/mman.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>  
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
  #include <osxfuse/fuse.h>
  #include <machine/endian.h>
#else
  #include <fuse.h>
  #include <endian.h>
#endif

#include "vfat.h"

#define DEBUG_PRINT //

// A kitchen sink for all important data about filesystem
struct vfat_data {
	const char	*dev;
	int		fs;
	struct fat_boot boot;
	/* XXX add your code here */
};

struct vfat_data vfat_info;
iconv_t iconv_utf16;

uid_t mount_uid;
gid_t mount_gid;
time_t mount_time;

// Print the content in hexadecimal form for debugging
static void hex_print(const uint8_t* content, size_t size)
{
	size_t offset;

	for (offset = 0; offset < size; ++offset) {
		if (offset % 16 == 0 && offset != 0)
			puts("");
		printf("%02x ", content[offset]);
	}
	printf("\n");
}

static void check_boot_validity(const struct fat_boot* data) {

	int dataSec;
	int count_clusters;



	if (data->bytes_per_sector != 512 || 
	// We assume it should be 512 bytes //Pourquoi? les
	//secteurs ne sont pas censés avoir une taille de 512 (flag utilisé pour indiqué la taille
	//disponible à 0x0B
	    data->fat_count != 2 || // Encore une fois pourquoi enforce cette valeur?
	    data->root_max_entries != 0 ||
	    data->total_sectors_small != 0 ||
	    data->sectors_per_fat_small != 0 ||
	    data->fat32.signature != 0xAA55)
	{
		errx(1, "Invalid FAT32 boot sector. Exiting...");
	}
	
	// Vérification de la vadilité du nombre de clusters pour un FAT32;
	dataSec = data->total_sectors – (data->reserved_sectors + (data->fat32.sectors_per_fat * data->fat_count) );
	count_clusters = = dataSec / sectors_per_cluster;
	if(count_clusters < 65525){
	   errx(1, "Invalid number of sectors for FAT32... Exiting...");
	
	}
	
}

static void
vfat_init(const char *dev)
{
	iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
	// These are useful so that we can setup correct permissions in the mounted directories
	mount_uid = getuid();
	mount_gid = getgid();

	// Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
	mount_time = time(NULL);

	vfat_info.fs = open(dev, O_RDONLY);
	if (vfat_info.fs < 0)
		err(1, "open(%s)", dev);

	// Read the boot sector
	read(vfat_info.fs, &vfat_info.boot, sizeof(vfat_info.boot));
	check_boot_validity(&vfat_info.boot);

}

/* XXX add your code here */

static int
vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t filler, void *fillerdata)
{
	struct stat st; // we can reuse same stat entry over and over again
	void *buf = NULL;
	struct vfat_direntry *e;
	char *name;

	memset(&st, 0, sizeof(st));
	st.st_uid = mount_uid;
	st.st_gid = mount_gid;
	st.st_nlink = 1;

	/* XXX add your code here */
}


// Used by vfat_search_entry()
struct vfat_search_data {
	const char	*name;
	int		found;
	struct stat	*st;
};


// You can use this in vfat_resolve as a filler function for vfat_readdir
static int
vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
	struct vfat_search_data *sd = data;

	if (strcmp(sd->name, name) != 0)
		return (0);

	sd->found = 1;
	*sd->st = *st;

	return (1);
}

// Recursively find correct file/directory node given the path
static int
vfat_resolve(const char *path, struct stat *st)
{
	struct vfat_search_data sd;

	/* XXX add your code here */
}

// Get file attributes
static int
vfat_fuse_getattr(const char *path, struct stat *st)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse getattr %s\n", path);
	// No such file
	if (strcmp(path, "/")==0) {
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
		st->st_nlink = 1;
		st->st_uid = mount_uid;
		st->st_gid = mount_gid;
		st->st_rdev = 0;
		st->st_size = 0;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return 0;
	}
	if (strcmp(path, "/a.txt")==0 || strcmp(path, "/b.txt")==0) {
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
		st->st_nlink = 1;
		st->st_uid = mount_uid;
		st->st_gid = mount_gid;
		st->st_rdev = 0;
		st->st_size = 10;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return 0;
	}

	return -ENOENT;
}

static int
vfat_fuse_readdir(const char *path, void *buf,
		  fuse_fill_dir_t filler, off_t offs, struct fuse_file_info *fi)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse readdir %s\n", path);
	//assert(offs == 0);
	/* XXX add your code here */
	filler(buf, "a.txt", NULL, 0);
	filler(buf, "b.txt", NULL, 0);
	return 0;
}

static int
vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,
	       struct fuse_file_info *fi)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse read %s\n", path);
	assert(size > 1);
	buf[0] = 'X';
	buf[1] = 'Y';
	/* XXX add your code here */
	return 2; // number of bytes read from the file
		  // must be size unless EOF reached, negative for an error 
}

////////////// No need to modify anything below this point
static int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
	if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
		vfat_info.dev = strdup(arg);
		return (0);
	}
	return (1);
}

static struct fuse_operations vfat_available_ops = {
	.getattr = vfat_fuse_getattr,
	.readdir = vfat_fuse_readdir,
	.read = vfat_fuse_read,
};

int
main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

	if (!vfat_info.dev)
		errx(1, "missing file system parameter");

	vfat_init(vfat_info.dev);
	return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
