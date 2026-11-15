#include <psp2/promoterutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vitaPackage.h"
#include "sha1.h"
#include "filesystem.h"
#include "res/base_head_bin.h"

#define ntohl __builtin_bswap32

#define SFO_MAGIC 0x46535000

#define PSF_TYPE_BIN 0
#define PSF_TYPE_STR 2
#define PSF_TYPE_VAL 4

// Size of the embedded sce_sys/package/head.bin template (0x430 bytes)
#define HEAD_BIN_SIZE ((int)sizeof(base_head_bin))

#define SCE_ERROR_ERRNO_EEXIST 0x80010011

std::string VitaPackage::Package_path = PACKAGE_TEMP_FOLDER;

static void fpkg_hmac(const uint8_t *data, unsigned int len, uint8_t hmac[16]) {
    SHA1_CTX ctx;
	char sha1[20];
	char buf[64];

	sha1_init(&ctx);
	sha1_update(&ctx, (BYTE*)data, len);
    sha1_final(&ctx, (BYTE*)sha1);

	memset(buf, 0, 64);
	memcpy(&buf[0], &sha1[4], 8);
	memcpy(&buf[8], &sha1[4], 8);
	memcpy(&buf[16], &sha1[12], 4);
	buf[20] = sha1[16];
	buf[21] = sha1[1];
	buf[22] = sha1[2];
	buf[23] = sha1[3];
	memcpy(&buf[24], &buf[16], 8);

	sha1_init(&ctx);
	sha1_update(&ctx, (BYTE*)buf, 64);
    sha1_final(&ctx, (BYTE*)sha1);
	memcpy(hmac, sha1, 16);
}

typedef struct SfoHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t keyofs;
	uint32_t valofs;
	uint32_t count;
} __attribute__((packed)) SfoHeader;

typedef struct SfoEntry {
	uint16_t nameofs;
	uint8_t  alignment;
	uint8_t  type;
	uint32_t valsize;
	uint32_t totalsize;
	uint32_t dataofs;
} __attribute__((packed)) SfoEntry;

/* Reads a whole file into a malloc'ed buffer. Returns the size or a negative error code. */
static int readWholeFile(const std::string &file, char **buffer) {
	*buffer = NULL;

	SceUID fd = sceIoOpen(file.c_str(), SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	int size = sceIoLseek32(fd, 0, SCE_SEEK_END);
	sceIoLseek32(fd, 0, SCE_SEEK_SET);
	if (size <= 0) {
		sceIoClose(fd);
		return size < 0 ? size : -1;
	}

	char *buf = (char *)malloc(size);
	if (!buf) {
		sceIoClose(fd);
		return -1;
	}

	int read = sceIoRead(fd, buf, size);
	sceIoClose(fd);
	if (read != size) {
		free(buf);
		return read < 0 ? read : -1;
	}

	*buffer = buf;
	return size;
}

/* Writes a whole buffer to a file. Returns 0 on success. */
static int writeWholeFile(const std::string &file, const void *buf, int size) {
	SceUID fd = sceIoOpen(file.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return fd;

	int written = sceIoWrite(fd, buf, size);
	sceIoClose(fd);

	if (written < 0)
		return written;
	return written == size ? 0 : -1;
}

/* Checks that a SFO buffer is big enough to safely walk its table. */
static int checkSfo(const char *buffer, int buffer_size) {
	if (!buffer || buffer_size < (int)sizeof(SfoHeader))
		return -1;

	const SfoHeader *header = (const SfoHeader *)buffer;
	if (header->magic != SFO_MAGIC)
		return -1;

	int entries_max = (buffer_size - (int)sizeof(SfoHeader)) / (int)sizeof(SfoEntry);
	if (header->count > (uint32_t)entries_max)
		return -1;
	if (header->keyofs >= (uint32_t)buffer_size || header->valofs >= (uint32_t)buffer_size)
		return -1;

	return 0;
}

/* Gets a string value from a SFO buffer. Returns 0 when found. */
static int getSfoString(const char *buffer, int buffer_size, const char *name, char *string, int length) {
	if (length <= 0)
		return -1;
	string[0] = '\0';

	if (checkSfo(buffer, buffer_size) < 0)
		return -1;

	const SfoHeader *header = (const SfoHeader *)buffer;

	for (uint32_t i = 0; i < header->count; i++) {
		const SfoEntry *entry = (const SfoEntry *)(buffer + sizeof(SfoHeader) + i * sizeof(SfoEntry));

		uint32_t key_start = header->keyofs + entry->nameofs;
		if (key_start >= (uint32_t)buffer_size)
			continue;
		if (strncmp(buffer + key_start, name, buffer_size - key_start) != 0)
			continue;

		if (entry->dataofs > (uint32_t)buffer_size - header->valofs)
			return -1;
		uint32_t val_start = header->valofs + entry->dataofs;
		if (val_start >= (uint32_t)buffer_size)
			return -1;

		uint32_t available = (uint32_t)buffer_size - val_start;
		uint32_t stored = entry->valsize < available ? entry->valsize : available;

		// Keep the copied string NUL terminated, even if valsize is stale
		int search_len = (int)(stored < available ? stored : available);
		int real_len = 0;
		while (real_len < search_len && buffer[val_start + real_len] != '\0')
			real_len++;

		int copy_len = real_len < length - 1 ? real_len : length - 1;
		memcpy(string, buffer + val_start, copy_len);
		string[copy_len] = '\0';

		return 0;
	}

	return -2;
}

/* Sets a string value in a SFO buffer, truncating it to the entry size.
   Returns 0 on success. */
static int setSfoString(char *buffer, int buffer_size, const char *name, const char *string) {
	if (checkSfo(buffer, buffer_size) < 0)
		return -1;

	SfoHeader *header = (SfoHeader *)buffer;

	for (uint32_t i = 0; i < header->count; i++) {
		SfoEntry *entry = (SfoEntry *)(buffer + sizeof(SfoHeader) + i * sizeof(SfoEntry));

		uint32_t key_start = header->keyofs + entry->nameofs;
		if (key_start >= (uint32_t)buffer_size)
			continue;
		if (strncmp(buffer + key_start, name, buffer_size - key_start) != 0)
			continue;

		if (entry->dataofs > (uint32_t)buffer_size - header->valofs)
			return -1;
		uint32_t val_start = header->valofs + entry->dataofs;
		if (val_start >= (uint32_t)buffer_size)
			return -1;

		uint32_t available = (uint32_t)buffer_size - val_start;
		uint32_t capacity = entry->totalsize < available ? entry->totalsize : available;
		if (capacity < 1)
			return -1;

		// Copy as much as fits, without splitting a UTF-8 sequence
		uint32_t copy_len = (uint32_t)strlen(string);
		if (copy_len > capacity - 1) {
			copy_len = capacity - 1;
			while (copy_len > 0 && ((unsigned char)string[copy_len] & 0xC0) == 0x80)
				copy_len--;
		}

		memcpy(buffer + val_start, string, copy_len);
		buffer[val_start + copy_len] = '\0';
		entry->valsize = copy_len + 1;

		return 0;
	}

	return -2;
}

/* Returns the TITLE_ID stored in <package path>sce_sys/param.sfo. */
static int readTitleId(const std::string &sfo_path, std::string &title_id) {
	char *sfo_buffer = NULL;
	int sfo_size = readWholeFile(sfo_path, &sfo_buffer);
	if (sfo_size < 0)
		return sfo_size;

	char titleid[12];
	memset(titleid, 0, sizeof(titleid));
	int ret = getSfoString(sfo_buffer, sfo_size, "TITLE_ID", titleid, sizeof(titleid));
	free(sfo_buffer);

	if (ret < 0 || strlen(titleid) != 9)
		return -1;

	title_id = titleid;
	return 0;
}

/* Reads a big endian uint32_t without unaligned accesses. */
static uint32_t readBE32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

/* Builds <package path>sce_sys/package/head.bin from the embedded template. */
int makeHeadBin()
{
    uint8_t hmac[16];
    uint32_t off;
    uint32_t len;
    uint32_t out;

    const std::string package_path = VitaPackage::Package_path;
    const std::string head_path = package_path + "sce_sys/package/head.bin";
    const std::string sfo_path = package_path + "sce_sys/param.sfo";

    if (checkFileExist(head_path.c_str()))
        return 0;

    // Read param.sfo
    char *sfo_buffer = nullptr;
    int sfo_size = readWholeFile(sfo_path, &sfo_buffer);
    if (sfo_size < 0)
        return sfo_size;

    // Get title id
    char titleid[12];
    memset(titleid, 0, sizeof(titleid));
    int ret = getSfoString(sfo_buffer, sfo_size, "TITLE_ID", titleid, sizeof(titleid));
    if (ret < 0) {
        free(sfo_buffer);
        return -1;
    }

    // Enforce TITLE_ID format
    if (strlen(titleid) != 9) {
        free(sfo_buffer);
        return -1;
    }

    // Get content id
    char contentid[48];
    memset(contentid, 0, sizeof(contentid));
    getSfoString(sfo_buffer, sfo_size, "CONTENT_ID", contentid, sizeof(contentid));

    // Free sfo buffer
    free(sfo_buffer);

    // Copy the embedded head.bin template
    uint8_t head_bin[HEAD_BIN_SIZE];
    memcpy(head_bin, base_head_bin, HEAD_BIN_SIZE);

    // Write full title id
    char full_title_id[48];
    memset(full_title_id, 0, sizeof(full_title_id));
    if (strlen(contentid) > 0)
        snprintf(full_title_id, sizeof(full_title_id), "%s", contentid);
    else
        snprintf(full_title_id, sizeof(full_title_id), "EP9000-%s_00-0000000000000000", titleid);

    memset(&head_bin[0x30], 0, 48);
    size_t contentid_len = strlen(full_title_id);
    if (contentid_len > 48)
        contentid_len = 48;
    memcpy(&head_bin[0x30], full_title_id, contentid_len);

    // hmac of pkg header
    len = readBE32(&head_bin[0xD0]);
    if (len < 16 || len > (uint32_t)HEAD_BIN_SIZE - 16)
        return -1;
    fpkg_hmac(&head_bin[0], len, hmac);
    memcpy(&head_bin[len], hmac, 16);

    // hmac of pkg info
    off = readBE32(&head_bin[0x8]);
    len = readBE32(&head_bin[0x10]);
    out = readBE32(&head_bin[0xD4]);
    if (off > (uint32_t)HEAD_BIN_SIZE || len > (uint32_t)HEAD_BIN_SIZE - off || len < 64)
        return -1;
    if (out > (uint32_t)HEAD_BIN_SIZE - 16)
        return -1;
    fpkg_hmac(&head_bin[off], len-64, hmac);
    memcpy(&head_bin[out], hmac, 16);

    // hmac of everything
    len = readBE32(&head_bin[0xE8]);
    if (len < 16 || len > (uint32_t)HEAD_BIN_SIZE - 16)
        return -1;
    fpkg_hmac(&head_bin[0], len, hmac);
    memcpy(&head_bin[len], hmac, 16);

    // Make dir
    ret = sceIoMkdir((package_path + "sce_sys/package").c_str(), 0777);
    if (ret < 0 && ret != SCE_ERROR_ERRNO_EEXIST)
        return ret;

    // Write head.bin
    ret = writeWholeFile(head_path, head_bin, HEAD_BIN_SIZE);
    if (ret < 0)
        return ret;

    return 0;
}

//TITLE = STITLE TITLE_ID
int VitaPackage::SetSFOString(std::string title, std::string title_id) {
	// Enforce TITLE_ID format
	if (title_id.length() != 9)
		return -1;

	// Read param.sfo
	std::string sfo_path = VitaPackage::Package_path + "sce_sys/param.sfo";
	char *sfo_buffer = nullptr;
	int sfo_size = readWholeFile(sfo_path, &sfo_buffer);
	if (sfo_size < 0)
		return sfo_size;

	if (checkSfo(sfo_buffer, sfo_size) < 0) {
		free(sfo_buffer);
		return -1;
	}

	int ret = 0;
	if (setSfoString(sfo_buffer, sfo_size, "TITLE", title.c_str()) < 0)
		ret = -1;
	if (setSfoString(sfo_buffer, sfo_size, "STITLE", title.c_str()) < 0)
		ret = -1;
	if (setSfoString(sfo_buffer, sfo_size, "TITLE_ID", title_id.c_str()) < 0)
		ret = -1;

	if (ret == 0)
		ret = writeWholeFile(sfo_path, sfo_buffer, sfo_size);

	free(sfo_buffer);
	return ret;
}

VitaPackage::VitaPackage(const std::string path) :
    path_(path)
{
	// scePromoterUtilityPromotePkg() must not receive a trailing '/'
	while (!path_.empty() && path_.back() == '/')
		path_.pop_back();
	if (path_.empty())
		path_ = "ux0:temp/pkg";

	VitaPackage::Package_path = path_ + "/";

    // ScePaf is required for PromoterUtil
    uint32_t ptr[0x100] = {0};
	ptr[0] = 0;
	ptr[1] = (uint32_t)&ptr[0];
	uint32_t scepaf_argp[] = {0x400000, 0xEA60, 0x40000, 0, 0};
    sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(scepaf_argp), scepaf_argp, (SceSysmoduleOpt*)ptr);

    sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    scePromoterUtilityInit();
}

VitaPackage::~VitaPackage()
{
    scePromoterUtilityExit();
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
}

int VitaPackage::Install()
{
	// The title id is needed to check whether the package was really installed
	std::string title_id;
	int ret = readTitleId(Package_path + "sce_sys/param.sfo", title_id);
	if (ret < 0) {
		printf("Can't read TITLE_ID from %s: 0x%08X\n", path_.c_str(), ret);
		return 0;
	}

    // The installed application folder must not exist yet, otherwise it cannot
    // prove that this installation succeeded
    const std::string app_path = "ux0:app/" + title_id;
    if (checkFolderExist(app_path.c_str())) {
		printf("Can't Promote %s: %s already exists\n", path_.c_str(), app_path.c_str());
		return 0;
    }

    ret = makeHeadBin();
    if (ret < 0) {
		printf("Can't make head.bin for %s: 0x%08X\n", path_.c_str(), ret);
		return 0;
    }

    ret = scePromoterUtilityPromotePkg(path_.c_str(), 0);
    if (ret < 0) {
		printf("Can't Promote %s: scePromoterUtilityPromotePkg() = 0x%08X\n", path_.c_str(), ret);
		return 0;
    }

    // Give the promoter utility a moment to start the installation
    sceKernelDelayThread(1000 * 1000);

    int state = 0;
    for (int i = 0; i < 300; i++) {
        ret = scePromoterUtilityGetState(&state);
        if (ret < 0) {
			printf("Can't Promote %s: scePromoterUtilityGetState() = 0x%08X\n", path_.c_str(), ret);
			return 0;
        }
        if (state == 0)
            break;
        sceKernelDelayThread(100 * 1000);
    }

    if (state != 0) {
		printf("Can't Promote %s: timed out\n", path_.c_str());
		return 0;
    }

    int result = 0;
    ret = scePromoterUtilityGetResult(&result);

    // The installed application folder is the authoritative proof of success
    bool installed = checkFolderExist(app_path.c_str());
    for (int i = 0; i < 10 && !installed; i++) {
        sceKernelDelayThread(100 * 1000);
        installed = checkFolderExist(app_path.c_str());
    }

    if (!installed) {
		printf("Can't Promote %s: not installed (GetResult 0x%08X, result 0x%08X)\n",
			path_.c_str(), ret, result);
		return 0;
    }

    if (ret < 0 || result < 0)
		printf("Promote %s returned 0x%08X/0x%08X but %s is installed\n",
			path_.c_str(), ret, result, title_id.c_str());

    removePath(path_);
    //if(progress) progress->percent(100);
    return 1;
}
