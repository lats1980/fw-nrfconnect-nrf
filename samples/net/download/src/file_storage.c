#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <ff.h>

LOG_MODULE_REGISTER(fs_test);

#define DISK_DRIVE_NAME			"SD"
#define DISK_MOUNT_PT			"/" DISK_DRIVE_NAME ":"
#define MAX_PATH 			128
#define DOWNLOADED_FILE_NAME		"file.bin"
#define DOWNLOADED_FILE_PATH		DISK_MOUNT_PT "/" DOWNLOADED_FILE_NAME
#define SOME_REQUIRED_LEN		MAX(sizeof(DOWNLOADED_FILE_NAME), sizeof(SOME_DIR_NAME))

static FATFS fat_fs;
/* mounting info */
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
};

static const char *disk_mount_pt = DISK_MOUNT_PT;

static int lsdir(const char *path)
{
	int res;
	struct fs_dir_t dirp;
	static struct fs_dirent entry;
	int count = 0;

	fs_dir_t_init(&dirp);

	/* Verify fs_opendir() */
	res = fs_opendir(&dirp, path);
	if (res) {
		printk("Error opening dir %s [%d]\n", path, res);
		return res;
	}

	printk("\nListing dir %s ...\n", path);
	for (;;) {
		/* Verify fs_readdir() */
		res = fs_readdir(&dirp, &entry);

		/* entry.name[0] == 0 means end-of-dir */
		if (res || entry.name[0] == 0) {
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR) {
			printk("[DIR ] %s\n", entry.name);
		} else {
			printk("[FILE] %s (size = %zu)\n",
				entry.name, entry.size);
		}
		count++;
	}

	/* Verify fs_closedir() */
	fs_closedir(&dirp);
	if (res == 0) {
		res = count;
	}

	return res;
}



int file_storage_init(void)
{

        static const char *disk_pdrv = DISK_DRIVE_NAME;
        uint32_t memory_size_kb;
        uint32_t sector_count;
        uint32_t sector_size;

        if (disk_access_init(disk_pdrv) != 0) {
                LOG_ERR("Storage init ERROR!");
                return -1;
        }

        if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_COUNT, &sector_count)) {
                LOG_ERR("Unable to get sector count");
                return -1;
        }
        LOG_INF("Block count %u", sector_count);

        if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size)) {
                LOG_ERR("Unable to get sector size");
                return -1;
        }
        LOG_INF("Sector size %u", sector_size);

        memory_size_kb = (uint32_t)(sector_count * sector_size) >> 10;
        LOG_INF("Memory Size (kB): %u", memory_size_kb);

	mp.mnt_point = disk_mount_pt;

	int res = fs_mount(&mp);

	if (res == FR_OK) {
		LOG_INF("Disk mounted");
	} else {
		LOG_ERR("Error mounting disk: %d", res);

		return -1;
	}

	return 0;
}



int file_storage_read(uint8_t *const buf, const size_t buf_size)
{
	struct fs_file_t file;
	int rc;
	char *fname = DOWNLOADED_FILE_PATH;

	fs_file_t_init(&file);

	rc = fs_open(&file, fname, FS_O_CREATE | FS_O_READ);
	if (rc < 0) {
		LOG_ERR("Failed to open file '%s': %d", fname, rc);
		return rc;
	}

	rc = fs_read(&file, buf, buf_size);
	if (rc < 0) {
		LOG_ERR("Failed to read from file '%s': %d", fname, rc);
	}

	if (fs_close(&file)) {
		LOG_ERR("Failed to close file '%s'", fname);
	}

	return rc;
}

int file_storage_write(const uint8_t *const buf, const size_t buf_size)
{
	struct fs_file_t file;
	int rc;
	char *fname = DOWNLOADED_FILE_PATH;

	fs_file_t_init(&file);

	rc = fs_open(&file, fname, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		LOG_ERR("Failed to open file '%s': %d", fname, rc);
		return rc;
	}

	rc = fs_seek(&file, 0, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to set file position in '%s': %d", fname, rc);
		goto out;
	}

	rc = fs_write(&file, buf, buf_size);
	if (rc < 0) {
		LOG_ERR("Failed to write data to '%s': %d", fname, rc);
	}

out:
	if (fs_close(&file)) {
		LOG_ERR("Failed to close file '%s'", fname);
	}

	return rc;
}

static struct fs_file_t stream_file;

int file_storage_write_stream_start(void)
{
	int rc;
	char *fname = DOWNLOADED_FILE_PATH;

	fs_file_t_init(&stream_file);

	rc = fs_open(&stream_file, fname, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		LOG_ERR("Failed to open file '%s': %d", fname, rc);
		return rc;
	}

	rc = fs_seek(&stream_file, 0, FS_SEEK_SET);
	if (rc < 0) {
		LOG_ERR("Failed to set file position in '%s': %d", fname, rc);

		if (fs_close(&stream_file)) {
			LOG_ERR("Failed to close file '%s'", fname);
		}

		return rc;
	}

	return 0;
}

int file_storage_write_stream_stop(void)
{
	int rc;

	rc = fs_close(&stream_file);
	if (rc) {
		LOG_ERR("Failed to close stream file");
	}

	return rc;
}

int file_storage_write_stream_fragment(const uint8_t *const buf, const size_t buf_size)
{
	int rc;

	rc = fs_write(&stream_file, buf, buf_size);
	if (rc < 0) {
		LOG_ERR("Failed to write data to stream file: %d", rc);
	}

	return rc;
}

void file_storage_lsdir(void)
{
	lsdir(disk_mount_pt);
}
