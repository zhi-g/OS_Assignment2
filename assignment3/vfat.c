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

#define DIRECTORY_RECORD_SIZE 32
#define MIN_NB_OF_SECTORS 65525
#define MAX_CLUSTER_SIZE 32768

// A kitchen sink for all important data about filesystem
struct vfat_data {
	const char	*dev;
	int		fs;
	struct fat_boot boot;

	size_t fat_begin; // offset of the FAT (in sectors)
	size_t clusters_begin; // offset of the clusters (in sectors)
	size_t fat_size; // size of FAT (in bytes)
	size_t clusters_size; // size of a cluster (in bytes)

	uint32_t* fat_content;
};

struct vfat_data vfat_info = { 0 };
iconv_t iconv_utf16;

uid_t mount_uid;
gid_t mount_gid;
time_t mount_time;

/*
 * Print the content in hexadecimal form for debugging
 */
static void hex_print(const void* content, size_t size) {
	if (content) {
		size_t offset;

		for (offset = 0; offset < size; ++offset) {
			if (offset % 16 == 0 && offset != 0)
				puts("");
			printf("%02X ", ((uint8_t*)content)[offset]);
		}
		printf("\n");
	}
}

/*
 * Follows a chain in the FAT starting at the specified offset.
 * Also calls the callback function for each entry.
 */
static void follow_fat_chain(size_t offset, void (*callback)(size_t cluster_number)) {
	if (vfat_info.fat_content && callback) {
		uint32_t entry;
		do {
			entry = vfat_info.fat_content[offset] & 0xFFFFFFF; // Mask the 4 upper bits
			printf("Callback for cluster #%08X\n", offset);
			callback(offset);
			offset++;
		} while (entry < 0xFFFFFF8);
	}
}

/*
 * Helper function to convert a number of sectors to a number of bytes
 */
static inline size_t sectors_to_bytes(size_t number_of_sectors) {
	return number_of_sectors * vfat_info.boot.bytes_per_sector;
}

/*
 * Helper function to convert a cluster number to a byte offset
 */
static inline size_t cluster_to_bytes(size_t cluster_number) {
	return vfat_info.clusters_begin + (cluster_number - 2) * vfat_info.clusters_size; // The first data cluster is cluster #2
}

/*
 * Read one cluster to the specified buffer
 */
static void read_cluster(void* buffer, size_t cluster_number) {
	if (buffer) { // TODO : Add check for sector alignment ?
		lseek(vfat_info.fs, cluster_to_bytes(cluster_number), SEEK_SET);
		read(vfat_info.fs, buffer, vfat_info.clusters_size);
	}
}

/*
 * Remove spaces from a filename (name + extension)
 * The output array has to be able to contain at least 12 characters
 */
static void trim_filename(char* output, char* nameext) {
	if (output && nameext) {
		size_t i;
		size_t out_offset = 0;

		for (i = 0; i < 11; ++i) { // Short names are always 11 characters long
			if (nameext[i] != 0x20) { // The character is not a space
				output[out_offset++] = nameext[i];
			}
		}

		output[out_offset] = '\0';
	}
}

/*
 * Read the directory starting at the specified cluster number
 */
static void read_directory(size_t cluster_number) {
	uint8_t cluster[vfat_info.clusters_size];
	read_cluster(cluster, cluster_number);

	size_t offset = 0;
	struct fat32_direntry entry;
	do {

		// If the first byte is 0xE5, the directory is unused
		if (cluster[offset] != 0xE5) {
			memcpy(&entry, &cluster[offset], sizeof(entry));

			// If it's a long file name, we're ignoring it (for now)
			if ((entry.attr & VFAT_ATTR_LFN) != VFAT_ATTR_LFN) {
				if (entry.attr & VFAT_ATTR_DIR) {
					printf("[D] ");
				} else if (entry.attr & VFAT_ATTR_VOLUME_ID) {
					printf("[V] ");
				} else if (entry.attr & VFAT_ATTR_INVAL) {
					printf("[I] ");
				} else {
					printf("[F] ");
				}

				char name[12]; // We need one more byte to add the \0 character so that it's a valid C string
				trim_filename(name, entry.nameext);

				uint32_t cluster_location = entry.cluster_hi << 16 | entry.cluster_lo;
				printf("%-11s - %u Bytes (First cluster = %u, offset = %08X)\n", name, entry.size, cluster_location, cluster_to_bytes(cluster_location));
			}

			//hex_print(&cluster[offset], DIRECTORY_RECORD_SIZE);
		}

		offset += DIRECTORY_RECORD_SIZE;
	} while(cluster[offset] != 0); // If the first byte is 0x00, end of directory
}

/*
 * Checks the Volume ID and make sure it's a FAT32 partition
 * Be careful since this function ends the program if the boot sector is invalid
 */
static void check_boot_validity(const struct fat_boot* data) {

	switch (data->bytes_per_sector) {
		case 512:
		case 1024:
		case 2048:
		case 4096:
			break; // Valid value
		default:
			errx(1, "Invalid number of bytes per sector. Exiting...");
	}

	switch (data->sectors_per_cluster) {
		case 1:
		case 2:
		case 4:
		case 8:
		case 16:
		case 32:
		case 64:
		case 128:
			break; // Valid value
		default:
			errx(1, "Invalid number of sectors per cluster. Exiting...");
	}

	if (data->bytes_per_sector * data->sectors_per_cluster >= MAX_CLUSTER_SIZE) {
		errx(1, "Invalid cluster size. Exiting...");
	}

	// Checking various fields of the boot sector
	if (
			data->fat_count != 2 ||
	    data->root_max_entries != 0 ||
	    data->total_sectors_small != 0 ||
	    data->sectors_per_fat_small != 0 ||
	    data->fat32.version != 0 ||
	    data->fat32.signature != 0xAA55)
	{
		errx(1, "Invalid FAT32 boot sector. Exiting...");
	}

	// Make sure the reserved space of the boot sector is empty (as it should be for a valid FAT32 partition)
	size_t offset;
	for (offset = 0; offset < sizeof(data->fat32.reserved2); ++offset) {
		if (data->fat32.reserved2[offset] != 0) {
			errx(1, "Reserved space of boot sector is not zero. Exiting...");
		}
	}
	
	// Checking if the number of clusters is valid
	unsigned long dataSec = data->total_sectors - (data->reserved_sectors + (data->fat32.sectors_per_fat * data->fat_count));
	unsigned long count_clusters = dataSec / data->sectors_per_cluster;
	
	if (count_clusters < MIN_NB_OF_SECTORS) {
		errx(1, "Invalid number of sectors for FAT32. Exiting...");
	}
}

/*
 * Free ressources
 */
static void cleanup(void) {
	free(vfat_info.fat_content);
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

	// Compute some useful values
	vfat_info.fat_begin = sectors_to_bytes(vfat_info.boot.reserved_sectors);
	vfat_info.fat_size = sectors_to_bytes(vfat_info.boot.fat32.sectors_per_fat);
	vfat_info.clusters_begin = sectors_to_bytes(vfat_info.boot.reserved_sectors + vfat_info.boot.fat32.sectors_per_fat * vfat_info.boot.fat_count);
	vfat_info.clusters_size = sectors_to_bytes(vfat_info.boot.sectors_per_cluster);

	// Read the FAT
	vfat_info.fat_content = calloc(vfat_info.fat_size, sizeof(uint32_t));
	if (vfat_info.fat_content == NULL) {
		errx(1, "Could't read the FAT. Exiting...");
	}

	lseek(vfat_info.fs, vfat_info.fat_begin, SEEK_SET);
	read(vfat_info.fs, vfat_info.fat_content, vfat_info.fat_size);
	
	// Print the FAT for debugging matters
	//hex_print(vfat_info.fat_content, vfat_info.fat_size);

	puts("Follow the FAT chain of the root cluster");
	follow_fat_chain(vfat_info.boot.fat32.root_cluster, read_directory);

	// Free Willy !
	cleanup();
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
