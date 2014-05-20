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
#include <time.h>

#ifdef __APPLE__
  #include <osxfuse/fuse.h>
  #include <machine/endian.h>
#else
  #include <fuse.h>
  #include <endian.h>
#endif

#include "vfat.h"

#define DEBUG_PRINT printf

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
	size_t fat_size; // size of the FAT (in bytes)
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

		for (i = 0; i < 8; ++i) { // Filename is 8 characters long
			if (nameext[i] != 0x20) { // The character is not a space
				output[out_offset++] = nameext[i];
			}
		}

		for (i = 8; i < 11; ++i) { // Extension is 3 characters long and begin at character 11
			if (nameext[i] != 0x20) { // The character is not a space
				if (i == 8) // We need to add a dot before the extension (if there is one)
					output[out_offset++] = '.';
				output[out_offset++] = nameext[i];
			}
		}

		output[out_offset] = '\0';
	}
}

/*
 * Read the file
 * Return the number of bytes read
 */
static size_t read_file(size_t first_cluster, char *buf, size_t size, off_t offset) {
	assert(size > 0);

	//DEBUG_PRINT("Reading %zu bytes starting at offset %d\n", size, offset);

	uint32_t fat_entry;
	uint32_t cluster_number = first_cluster;
	size_t read_so_far = 0;

	uint8_t *cluster = calloc(vfat_info.clusters_size, sizeof(uint8_t));
	if (!cluster) {
		fputs("Could't allocate memory to read a cluster", stderr);
		return 0; // 0 byte read
	}

	do {
		fat_entry = vfat_info.fat_content[cluster_number] & 0xFFFFFFF; // Mask the 4 upper bits
		//DEBUG_PRINT("Processing cluster #%zu (next is #%zu). Offset is %d\n", cluster_number, fat_entry, offset);

		if (offset < vfat_info.clusters_size) { // We need to read in the current cluster
			size_t to_read = size < vfat_info.clusters_size ? size : vfat_info.clusters_size - offset;
			//DEBUG_PRINT("We need to read %zu bytes in cluster %zu\n", to_read, cluster_number);
			
			read_cluster(cluster, cluster_number);

			memcpy(buf + read_so_far, cluster + offset, to_read);
			//DEBUG_PRINT("Wrote %zu bytes to the buffer at offset %d\n", to_read, read_so_far);

			size -= to_read;
			read_so_far += to_read;
			//DEBUG_PRINT("We still need to read %zu bytes, and we've read %zu bytes so far.\n", size, read_so_far);
		} else { // We just need to go to the next cluster
			//DEBUG_PRINT("Skipping to the next cluster\n");
			offset -= vfat_info.clusters_size;
		}

		cluster_number = fat_entry;
	} while (cluster_number < 0xFFFFFF8 && size > 0); // Any value greater or equal to 0xFFFFFF8 means end of chain

	free(cluster);

	return read_so_far;
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

static void vfat_init(const char *dev)
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
}

// Used by vfat_search_entry()
struct vfat_search_data {
	const char	*name;
	int		found;
	struct stat	*st;
	uint32_t first_cluster;
};

// You can use this in vfat_resolve as a filler function for vfat_readdir
static int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
	struct vfat_search_data *sd = data;

	if (strcmp(sd->name, name) != 0)
		return (0);

	sd->found = 1;
	if (sd->st != NULL)
		*sd->st = *st;
	sd->first_cluster = offs;

	return (1);
}

static time_t to_unix_time(uint16_t fat_date, uint16_t fat_time) {
	struct tm time_struct;

  uint8_t day = fat_date & 0x001F;
  uint8_t month = (fat_date & 0x01E0) >> 5;
  uint8_t year = (fat_date & 0xFE00) >> 9;
	time_struct.tm_year = year + 80;
	time_struct.tm_mon = month - 1;
	time_struct.tm_mday = day;

	uint8_t seconds = fat_time & 0x001F;
	uint8_t minutes = (fat_time & 0x07E0) >> 5;
	uint8_t hours = (fat_time & 0xF800) >> 11;
	time_struct.tm_sec = seconds * 2;
	time_struct.tm_min = minutes;
	time_struct.tm_hour = hours;

	return mktime(&time_struct);
}

static int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t filler, void *fillerdata)
{
	struct stat st; // we can reuse same stat entry over and over again
	//void *buf = NULL;
	struct fat32_direntry entry;
	//char *name;
	int done = 0;

	memset(&st, 0, sizeof(st));
	st.st_uid = mount_uid;
	st.st_gid = mount_gid;
	st.st_nlink = 1;
	st.st_blocks = 1;

	// Following the FAT chain
	uint32_t fat_entry;
	uint32_t cluster_offset = first_cluster;

	uint8_t *cluster = calloc(vfat_info.clusters_size, sizeof(uint8_t));
	if (!cluster) {
		fputs("Could't allocate memory to read a cluster", stderr);
		return 0; // 0 byte read
	}

	do {
		fat_entry = vfat_info.fat_content[cluster_offset] & 0xFFFFFFF; // Mask the 4 upper bits

		// Read the current cluster
		read_cluster(cluster, cluster_offset);

		// Processing the directory entries of the current cluster
		size_t dir_offset = 0;
		do {
			// If the first byte is 0xE5, the directory is unused
			if (cluster[dir_offset] != 0xE5) {
				memcpy(&entry, &cluster[dir_offset], sizeof(struct fat32_direntry));

				// If it's a long file name, we're ignoring it (for now)
				// We're also ignoring Volume ID and files with invalid attributes
				if ((entry.attr & VFAT_ATTR_LFN) != VFAT_ATTR_LFN &&
					  (entry.attr & VFAT_ATTR_VOLUME_ID) == 0 &&
					  (entry.attr & VFAT_ATTR_INVAL) == 0) {

					if (entry.attr & VFAT_ATTR_DIR) {
						st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR; // Directory
					} else {
						st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG; // File
					}

					st.st_size = entry.size;

					char name[13]; // We need two more bytes, one to add the dot between the name and the extension, the other for the \0 character so that it's a valid C string
					trim_filename(name, entry.nameext);

					// Set times
					st.st_ctime = to_unix_time(entry.ctime_date, entry.ctime_time);
					st.st_atime = to_unix_time(entry.atime_date, 0);
					st.st_mtime = to_unix_time(entry.mtime_date, entry.mtime_time);

					off_t cluster_location = entry.cluster_hi << 16 | entry.cluster_lo;

					// Calling the filler
					if (filler == vfat_search_entry)
						done = filler(fillerdata, name, &st, cluster_location);
					else
						done = filler(fillerdata, name, &st, 0);
				}
			}

			dir_offset += DIRECTORY_RECORD_SIZE;
		} while (cluster[dir_offset] != 0x00 && !done); // If the first byte is 0x00, end of directory


		cluster_offset = fat_entry;
	} while (cluster_offset < 0xFFFFFF8 && !done); // Any value greater or equal to 0xFFFFFF8 means end of chain

	free(cluster);

	return 0;
}

// Recursively find correct file/directory node given the path
static int vfat_resolve(const char *path, struct stat *st)
{
	if (strcmp(path, "/") == 0)
		return vfat_info.boot.fat32.root_cluster;

	struct vfat_search_data sd;

	char* token = strtok(path, "/");
	sd.st = st;
	sd.first_cluster = vfat_info.boot.fat32.root_cluster;

	do {
		sd.name = token;
		sd.found = 0;

		vfat_readdir(sd.first_cluster, vfat_search_entry, &sd);
		token = strtok(NULL, "/");
	} while (token != NULL);

	return sd.found ? sd.first_cluster : -1;
}

// Get file attributes
static int64_t vfat_fuse_getattr(const char *path, struct stat *st)
{
	//DEBUG_PRINT("fuse getattr %s\n", path);
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
	} else if (vfat_resolve(path, st) >= 0) {
		return 0;
	}

	return -ENOENT;
}

static int vfat_fuse_readdir(const char *path, void *buf,
		  fuse_fill_dir_t filler, off_t offs, struct fuse_file_info *fi)
{
	//DEBUG_PRINT("fuse readdir %s\n", path);
	//assert(offs == 0);

	uint32_t first_cluster = vfat_resolve(path, NULL);

	if (first_cluster)
		vfat_readdir(first_cluster, filler, buf);
	
	//filler(buf, "b.txt", NULL, 0);
	return 0;
}

static int vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,
	       struct fuse_file_info *fi)
{
	//DEBUG_PRINT("fuse read %s\n", path);
	assert(size > 1);

	uint32_t first_cluster = vfat_resolve(path, NULL);

	if (first_cluster) {
		return read_file(first_cluster, buf, size, offs);
	} else {
		return -1; // not found
	}
}

////////////// No need to modify anything below this point
static int vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
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
	.destroy = cleanup,
};

int main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

	if (!vfat_info.dev)
		errx(1, "missing file system parameter");

	vfat_init(vfat_info.dev);
	return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
