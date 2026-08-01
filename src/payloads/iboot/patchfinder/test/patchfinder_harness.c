#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define IBOOT_SEARCH_LEN 0x200000ull

extern int patchfinder(uintptr_t iboot_addr);

struct patch_record
{
	const char *patcher;
	uint64_t offset;
	uint64_t size;
	uint8_t *bytes;
};

static uint8_t *g_image;
static uintptr_t g_base_addr;
static size_t g_file_size;
static size_t g_alloc_size;
static struct patch_record *g_patches;
static size_t g_patch_count;
static size_t g_patch_capacity;
static const char *g_patcher;

static void die_errno(const char *path)
{
	fprintf(stderr, "%s: %s\n", path, strerror(errno));
	exit(2);
}

static void die_message(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(2);
}

void combined_stage2_test_set_patcher(const char *name)
{
	g_patcher = name;
}

void combined_stage2_test_record_patch(uintptr_t addr, const uint8_t *bytes, uintptr_t count)
{
	if (addr < g_base_addr || addr - g_base_addr > g_alloc_size || count > g_alloc_size - (addr - g_base_addr))
		die_message("patch write outside loaded image allocation");
	if (count > SIZE_MAX)
		die_message("patch write is too large to record");

	if (g_patch_count == g_patch_capacity)
	{
		size_t next_capacity = g_patch_capacity == 0 ? 16 : g_patch_capacity * 2;
		struct patch_record *next = realloc(g_patches, next_capacity * sizeof(*next));
		if (!next)
			die_errno("realloc");
		g_patches = next;
		g_patch_capacity = next_capacity;
	}

	struct patch_record *record = &g_patches[g_patch_count++];
	record->patcher = g_patcher;
	record->offset = addr - g_base_addr;
	record->size = count;
	record->bytes = malloc((size_t)count);
	if (!record->bytes)
		die_errno("malloc");
	memcpy(record->bytes, bytes, (size_t)count);
}

static int load_file(const char *path, uint8_t **image, size_t *file_size, size_t *alloc_size)
{
	struct stat st;
	if (stat(path, &st) != 0)
	{
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX)
	{
		fprintf(stderr, "%s: file is too large\n", path);
		return -1;
	}

	*file_size = (size_t)st.st_size;
	*alloc_size = *file_size > IBOOT_SEARCH_LEN ? *file_size : IBOOT_SEARCH_LEN;
	*image = calloc(1, *alloc_size);
	if (!*image)
	{
		fprintf(stderr, "calloc: %s\n", strerror(errno));
		return -1;
	}

	FILE *fp = fopen(path, "rb");
	if (!fp)
	{
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		free(*image);
		return -1;
	}

	if (*file_size != 0 && fread(*image, 1, *file_size, fp) != *file_size)
	{
		fprintf(stderr, "%s: failed to read complete file\n", path);
		fclose(fp);
		free(*image);
		return -1;
	}

	if (fclose(fp) != 0)
	{
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		free(*image);
		return -1;
	}

	return 0;
}

static int write_file(const char *path, const uint8_t *image, size_t file_size)
{
	FILE *fp = fopen(path, "wb");
	if (!fp)
	{
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}

	if (file_size != 0 && fwrite(image, 1, file_size, fp) != file_size)
	{
		fprintf(stderr, "%s: failed to write complete file\n", path);
		fclose(fp);
		return -1;
	}

	if (fclose(fp) != 0)
	{
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}

	return 0;
}

static void print_hex(const uint8_t *bytes, uint64_t count)
{
	for (uint64_t i = 0; i < count; i++)
		printf("%02x", bytes[i]);
}

static void print_patch_records(void)
{
	if (g_patch_count == 0)
	{
		printf("no patch writes recorded\n");
		return;
	}

	printf("patch writes:\n");
	for (size_t i = 0; i < g_patch_count; i++)
	{
		const struct patch_record *record = &g_patches[i];
		int outside_input = record->offset > g_file_size || record->size > g_file_size - record->offset;

		printf(
			"%02zu patcher=%s offset=0x%08" PRIx64 " probably-absolute= 0x%" PRIx64
			" size=0x%" PRIx64 "%s bytes=",
			i + 1,
			record->patcher,
			record->offset,
			record->offset + 0x19c050000,
			record->size,
			outside_input ? " outside-input" : ""
		);
		print_hex(record->bytes, record->size);
		printf("\n");
	}
}

static void free_patch_records(void)
{
	for (size_t i = 0; i < g_patch_count; i++)
		free(g_patches[i].bytes);
	free(g_patches);
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <iboot-image> [patched-output]\n", argv0);
}

int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3)
	{
		usage(argv[0]);
		return 2;
	}

	if (load_file(argv[1], &g_image, &g_file_size, &g_alloc_size) != 0)
		return 1;

	g_base_addr = (uintptr_t)g_image;
	int result = patchfinder(g_base_addr);

	printf("patchfinder returned %d\n", result);
	print_patch_records();

	if (argc == 3)
	{
		if (result != 0)
			fprintf(stderr, "not writing patched output because patchfinder failed\n");
		else if (write_file(argv[2], g_image, g_file_size) != 0)
		{
			free_patch_records();
			free(g_image);
			return 1;
		}
		else
			printf("wrote patched output: %s\n", argv[2]);
	}

	free_patch_records();
	free(g_image);
	return result == 0 ? 0 : 1;
}
