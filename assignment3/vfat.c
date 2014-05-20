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
	const char *dev;
	int fs;
	struct fat_boot boot;

	size_t fat_begin; // offset of the FAT (in sectors)
	size_t clusters_begin; // offset of the clusters (in sectors)
	size_t fat_size; // size of FAT (in bytes)
	size_t clusters_size; // size of a cluster (in bytes)

	uint32_t* fat_content;
};

struct vfat_direntry {
	uint32_t first_cluster;

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
			printf("%02X ", ((uint8_t*) content)[offset]);
		}
		printf("\n");
	}
}

/*
 * Follows a chain in the FAT starting at the specified offset.
 * Also calls the callback function for each entry.
 * The size argument is used when reading files. Should be 0 for directories.
 *
 * The arguments of the callback function are :
 * - The cluster to read
 * - How many bytes to read from that cluster
 */
static void follow_fat_chain(size_t offset,
		void (*callback)(size_t cluster_number, size_t n), size_t size) {
	if (vfat_info.fat_content && callback) {
		uint32_t entry;

		do {
			entry = vfat_info.fat_content[offset] & 0xFFFFFFF; // Mask the 4 upper bits
			printf("Callback for cluster #%zu (next is #%zu)\n", offset, entry);

			size_t to_read =
					size < vfat_info.clusters_size ?
							size : vfat_info.clusters_size;
			callback(offset, to_read);
			size -= vfat_info.clusters_size;

			offset = entry;
		} while (offset < 0xFFFFFF8); // Any value greater or equal to 0xFFFFFF8 means end of chain
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
	return vfat_info.clusters_begin
			+ (cluster_number - 2) * vfat_info.clusters_size; // The first data cluster is cluster #2
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
 * Read the directory entries located at the specified cluster number
 * The n argument is ignored. It's used when reading files but we keep it to have the same signature.
 */
static void read_directory_cluster(size_t cluster_number, size_t n) {
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

				uint32_t cluster_location = entry.cluster_hi << 16
						| entry.cluster_lo;
				printf("%-11s - %u Bytes (First cluster = %u, offset = %08X)\n",
						name, entry.size, cluster_location,
						cluster_to_bytes(cluster_location));
			}

			//hex_print(&cluster[offset], DIRECTORY_RECORD_SIZE);
		}

		offset += DIRECTORY_RECORD_SIZE;
	} while (cluster[offset] != 0); // If the first byte is 0x00, end of directory
}

/*
 * Helper function to read the whole directory starting at cluster number
 */
static inline void read_directory(size_t cluster_number) {
	follow_fat_chain(cluster_number, read_directory_cluster, 0);
}

/*
 * Read the first n bytes of the specified cluster.
 */
static void read_file_cluster(size_t cluster_number, size_t n) {
	assert(n <= vfat_info.clusters_size);

	uint8_t cluster[vfat_info.clusters_size];
	read_cluster(cluster, cluster_number);

	hex_print(cluster, n);
}

/*
 * Read the file
 */
static void read_file(size_t cluster_number, size_t size) {
	assert(size > 0);

	follow_fat_chain(cluster_number, read_file_cluster, size);
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

	if (sectors_to_bytes(data->sectors_per_cluster) >= MAX_CLUSTER_SIZE) {
		errx(1, "Invalid cluster size. Exiting...");
	}

	// Checking various fields of the boot sector
	if (data->fat_count != 2 || data->root_max_entries != 0
			|| data->total_sectors_small != 0
			|| data->sectors_per_fat_small != 0 || data->fat32.version != 0
			|| data->fat32.signature != 0xAA55) {
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
	unsigned long dataSec = data->total_sectors
			- (data->reserved_sectors
					+ (data->fat32.sectors_per_fat * data->fat_count));
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

static void vfat_init(const char *dev) {
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
	vfat_info.clusters_begin = sectors_to_bytes(
			vfat_info.boot.reserved_sectors
					+ vfat_info.boot.fat32.sectors_per_fat
							* vfat_info.boot.fat_count);
	vfat_info.clusters_size = sectors_to_bytes(
			vfat_info.boot.sectors_per_cluster);

	// Read the FAT
	vfat_info.fat_content = calloc(vfat_info.fat_size, sizeof(uint32_t));
	if (vfat_info.fat_content == NULL) {
		errx(1, "Could't read the FAT. Exiting...");
	}

	lseek(vfat_info.fs, vfat_info.fat_begin, SEEK_SET);
	read(vfat_info.fs, vfat_info.fat_content, vfat_info.fat_size);

	// Print the FAT for debugging matters
	//hex_print(vfat_info.fat_content, vfat_info.fat_size);

	puts("=============================");
	puts(" Reading the root directory ");
	puts("=============================");
	//read_directory(vfat_info.boot.fat32.root_cluster);

	puts("Reading a file...");
	//read_file(783, 458759);
	//read_file(259, 10);

	// Free Willy !
	//cleanup();
}

/* XXX add your code here */

static int

vfat_readdir(struct vfat_direntry *e, fuse_fill_dir_t filler, void *fillerdata) {
	struct stat st; // we can reuse same stat entry over and over again
	void *buf = NULL;
	char name[12];
	struct fat32_direntry entry;
	//int max_len_name = VFAT_MAXFILE_NAME;
	//char name[max_len_name];
	uint8_t cluster[vfat_info.clusters_size];
	//memset(name, '\0', max_len_name);
	memset(&st, 0, sizeof(st));
	st.st_uid = mount_uid;
	st.st_gid = mount_gid;
	st.st_nlink = 1;
	st.st_rdev = 0;
	st.st_size =0;
	st.st_blocks=1;
	st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
	buf = fillerdata;

	read_cluster(cluster, e->first_cluster);

	unsigned int offset = 0;
	unsigned int next_offset = 0;

	do {
		memset(name, '\0', 12);

		next_offset += DIRECTORY_RECORD_SIZE;

		// If the first byte is 0xE5, the directory is unused
		if (cluster[offset] != 0xE5) {
			memcpy(&entry, &cluster[offset], sizeof(entry));

			// If it's a long file name, we're ignoring it (for now)
			if ((entry.attr & VFAT_ATTR_LFN) != VFAT_ATTR_LFN) {
				if (entry.attr & VFAT_ATTR_DIR) {
					st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
					printf("[D] ");
				} else if (entry.attr & VFAT_ATTR_VOLUME_ID) {
					printf("[V] ");
				} else if (entry.attr & VFAT_ATTR_INVAL) {
					printf("[I] ");
				} else {
					printf("[F] ");
					st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
				}
				st.st_rdev = 0;
				st.st_size = 30;
				st.st_blocks=1;
				// FIx me .. allocation of  name with MAXNAMELENGTH
				// We need one more byte to add the \0 character so that it's a valid C string
				trim_filename(name, entry.nameext);

				e->first_cluster = entry.cluster_hi << 16 | entry.cluster_lo;
				printf("%-11s - %u Bytes (First cluster = %u, offset = %08X)\n",
						name, entry.size, e->first_cluster, offset);

				filler(buf, name, &st, 0);

			}

			//hex_print(&cluster[offset], DIRECTORY_RECORD_SIZE);
		}

		offset += DIRECTORY_RECORD_SIZE;

	} while (cluster[offset] != 0); // If the first byte is 0x00, end of directory
	return cluster[offset];
}

// Used by vfat_search_entry()
struct vfat_search_data {
	const char *name;
	int found;
	struct stat *st;

};

// You can use this in vfat_resolve as a filler function for vfat_readdir
static int vfat_search_entry(void *data, const char *name,
		const struct stat *st, off_t offs) {
	struct vfat_search_data *sd = data;

	if (strcmp(sd->name, name) != 0)
		return (0);

	sd->found = 1;

	*sd->st = *st;

	return (1);
}

// Recursively find correct file/directory node given the path
static int vfat_resolve(const char *path, struct stat *st,
		struct vfat_direntry *e) {
	struct vfat_search_data sd;

	char *path_v;
	char ch[2] = "/";
	char *name;
	int length;
	int size;
	char *next;
	size = strlen(path) + 1;
	path_v = malloc(sizeof(char) * size);
	memset(path_v, '\0', size);
	strcpy(path_v, path);

	path_v = path_v + strspn(path_v, ch); //for leading "/";

	name = strchr(path_v, ch);
	length = size - 1;
	if (name != 0) {
		length = name - path_v;
	}

	name = malloc(sizeof(char) * (length + 1));
	memset(name, '\0', length + 1);
	memcpy(name, path_v, length);
	sd.name = name;
	sd.found = 0;

	vfat_readdir(e, vfat_search_entry, &sd);
	if (sd.found) {
		next = strchr(path_v, ch);
		if (next != 0) {
			vfat_resolve(next, st, e);
		}

	}

	return sd.found;

	/* XXX add your code here */
}

// Get file attributes
static int vfat_fuse_getattr(const char *path, struct stat *st) {
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse getattr %s\n", path);
	struct vfat_search_data sd;
	struct vfat_direntry e;

	// No such file
	if (strcmp(path, "/") == 0) {
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
	} else
		e.first_cluster = vfat_info.boot.fat32.root_cluster;
		sd.name = path+1;
		sd.st =st;
	    vfat_readdir(&e, vfat_search_entry, &sd);
		return 0;
	}
//	{
//		st->st_dev = 0; // Ignored by FUSE
//		st->st_ino = 0; // Ignored by FUSE unless overridden
//		st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
//		st->st_nlink = 1;
//		st->st_uid = mount_uid;
//		st->st_gid = mount_gid;
//		st->st_rdev = 0;
//		st->st_size = 0;
//		st->st_blksize = 0; // Ignored by FUSE
//		st->st_blocks = 1;
//		return 0;
//	}

	//return -ENOENT;
//}

static int vfat_fuse_readdir(const char *path, void *buf,
		fuse_fill_dir_t filler, off_t offs, struct fuse_file_info *fi) {
	struct stat st;
	struct vfat_direntry e;
	int next = 0;
	int res = 1;

	/* XXX: This is example code, replace with your own implementation */
	printf("fuse readdir %s\n", path);
	printf("offset = %08X)\n", offs);
	e.first_cluster = vfat_info.boot.fat32.root_cluster;
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	if (strcmp(path, "/")) {
		res = vfat_resolve(path, &st, &e);

	}
	if (res)
		vfat_readdir(&e, filler, buf);

	//assert(offs == 0);
	/* XXX add your code here */
	//filler(buf, "a.txt", NULL, 0);
	//filler(buf, "b.txt", NULL, 0);
	return next;
}

static int vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,
		struct fuse_file_info *fi) {
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
static int vfat_opt_args(void *data, const char *arg, int key,
		struct fuse_args *oargs) {
	if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
		vfat_info.dev = strdup(arg);
		return (0);
	}
	return (1);
}

static struct fuse_operations vfat_available_ops =
		{ .getattr = vfat_fuse_getattr, .readdir = vfat_fuse_readdir, .read =
				vfat_fuse_read, };

int main(int argc, char **argv) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

	if (!vfat_info.dev)
		errx(1, "missing file system parameter");

	vfat_init(vfat_info.dev);
	return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
