#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "fat_filelib.h"
#include "fat_format.h"

static int g_img_fd = -1;

static int disk_read(uint32 sector, uint8 *buffer, uint32 count) {
    if (lseek(g_img_fd, (off_t)sector * 512, SEEK_SET) == (off_t)-1) return 0;
    ssize_t n = read(g_img_fd, buffer, count * 512);
    return (n == (ssize_t)(count * 512)) ? 1 : 0;
}

static int disk_write(uint32 sector, uint8 *buffer, uint32 count) {
    if (lseek(g_img_fd, (off_t)sector * 512, SEEK_SET) == (off_t)-1) return 0;
    ssize_t n = write(g_img_fd, buffer, count * 512);
    return (n == (ssize_t)(count * 512)) ? 1 : 0;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        printf("Failed to open source file: %s\n", src);
        return -1;
    }

    void *out = fl_fopen(dst, "wb");
    if (!out) {
        printf("Failed to open destination file in FAT: %s\n", dst);
        fclose(in);
        return -1;
    }

    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fl_fwrite(buf, 1, n, out);
    }

    fclose(in);
    fl_fclose(out);
    return 0;
}

static int copy_head(const char *src, const char *dst, size_t max_bytes) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        printf("Failed to open source file: %s\n", src);
        return -1;
    }

    void *out = fl_fopen(dst, "wb");
    if (!out) {
        printf("Failed to open destination file in FAT: %s\n", dst);
        fclose(in);
        return -1;
    }

    char buf[4096];
    size_t written = 0;
    while (written < max_bytes) {
        size_t to_read = sizeof(buf);
        if (to_read > max_bytes - written) to_read = max_bytes - written;
        size_t n = fread(buf, 1, to_read, in);
        if (n == 0) break;
        fl_fwrite(buf, 1, n, out);
        written += n;
    }

    fclose(in);
    fl_fclose(out);
    return 0;
}

static int copy_dir(const char *src_dir, const char *dst_dir, const char *skip1, const char *skip2) {
    fl_createdirectory(dst_dir);

    DIR *d = opendir(src_dir);
    if (!d) {
        printf("Failed to opendir: %s\n", src_dir);
        return -1;
    }

    struct dirent *de;
    char src_sub[1024];
    char dst_sub[1024];
    struct stat st;

    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (skip1 && strcmp(de->d_name, skip1) == 0)
            continue;
        if (skip2 && strcmp(de->d_name, skip2) == 0)
            continue;
        if (strstr(de->d_name, ".img") != NULL && strcmp(dst_dir, "/grub/i386-pc") == 0)
            continue;
        if (strstr(de->d_name, "ventoy.disk.img") != NULL)
            continue;

        snprintf(src_sub, sizeof(src_sub), "%s/%s", src_dir, de->d_name);
        snprintf(dst_sub, sizeof(dst_sub), "%s/%s", dst_dir, de->d_name);

        if (stat(src_sub, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            copy_dir(src_sub, dst_sub, NULL, NULL);
        } else if (S_ISREG(st.st_mode)) {
            copy_file(src_sub, dst_sub);
        }
    }

    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    const char *img_path = "INSTALL/ventoy/ventoy.disk.img";
    printf("Creating %s (32MB)...\n", img_path);

    g_img_fd = open(img_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (g_img_fd < 0) {
        perror("open");
        return 1;
    }

    if (ftruncate(g_img_fd, 32 * 1024 * 1024) != 0) {
        perror("ftruncate");
        return 1;
    }

    struct fatfs fs;
    memset(&fs, 0, sizeof(fs));
    fs.disk_io.read_media = disk_read;
    fs.disk_io.write_media = disk_write;

    if (!fatfs_format_fat16(&fs, 65536, "VTOYEFI")) {
        printf("FAT16 format failed!\n");
        return 1;
    }
    printf("Formatted FAT16 (VTOYEFI) successfully.\n");

    fl_init();
    if (fl_attach_media(disk_read, disk_write) != 0) {
        printf("fl_attach_media failed!\n");
        return 1;
    }

    printf("Populating /grub ...\n");
    fl_createdirectory("/grub");
    // grub.cfg first
    copy_file("INSTALL/grub/grub.cfg", "/grub/grub.cfg");
    copy_dir("INSTALL/grub", "/grub", "help", "menu");

    printf("Populating /ventoy ...\n");
    fl_createdirectory("/ventoy");
    copy_dir("INSTALL/ventoy", "/ventoy", NULL, NULL);

    printf("Populating /EFI ...\n");
    fl_createdirectory("/EFI");
    copy_dir("INSTALL/EFI", "/EFI", NULL, NULL);

    printf("Populating /tool ...\n");
    fl_createdirectory("/tool");
    copy_head("INSTALL/tool/i386/vtoycli", "/tool/mount.exfat-fuse_i386", 16384);
    copy_head("INSTALL/tool/x86_64/vtoycli", "/tool/mount.exfat-fuse_x86_64", 16384);
    copy_head("INSTALL/tool/aarch64/vtoycli", "/tool/mount.exfat-fuse_aarch64", 16384);
    copy_file("INSTALL/tool/create_ventoy_iso_part_dm.sh", "/tool/create_ventoy_iso_part_dm.sh");
    copy_file("INSTALL/tool/ENROLL_THIS_KEY_IN_MOKMANAGER.cer", "/ENROLL_THIS_KEY_IN_MOKMANAGER.cer");

    fl_shutdown();
    close(g_img_fd);

    printf("Done populating %s!\n", img_path);
    return 0;
}

