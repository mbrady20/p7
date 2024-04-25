#include "wfs.h"
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    int opt;
    int blocks = 0;
    int inodes = 0;
    char *disk = NULL;

    while ((opt = getopt(argc, argv, "d:i:b:")) != -1)
    {
        switch (opt)
        {
        case 'd':
            break;

        case 'i':
            inodes = atoi(optarg);
            break;
        case 'b':
            blocks = atoi(optarg);
            int mod = blocks % 32;
            if (mod != 0)
            {
                blocks += 32 - mod;
            }
            break;
        default:
            break;
        }
    }

    if (disk == NULL || inodes == 0 || blocks == 0)
    {
        printf("Usage: mkfs -d <disk> -i <inodes> -b <blocks>\n");
        return -1;
    }

    int disk_file = open(disk, O_RDWR | O_CREAT, 0666);
    if (disk_file == -1)
    {
        perror("open");
        return -1;
    }

    // ensure disk is large enough
    size_t required_size = sizeof(struct wfs_sb) + (inodes + 7) / 8 + (blocks + 7) / 8 + (inodes * sizeof(struct wfs_inode)) + blocks * BLOCK_SIZE;

    struct stat st;
    fstat(disk_file, &st);
    if (st.st_size < required_size)
    {
        perror("Disk is too small");
        return 1;
    }

    struct wfs_sb superblock =
        {
            .num_inodes = inodes,
            .num_data_blocks = blocks,
            .i_bitmap_ptr = sizeof(struct wfs_sb),
            .d_bitmap_ptr = sizeof(struct wfs_sb) + (inodes + 7) / 8,
            .i_blocks_ptr = sizeof(struct wfs_sb) + (inodes + 7) / 8 + (blocks + 7) / 8,
            .d_blocks_ptr = sizeof(struct wfs_sb) + (inodes + 7) / 8 + (blocks + 7) / 8 + (inodes * sizeof(struct wfs_inode))};

    // write superblock to disk
    lseek(disk_file, 0, SEEK_SET);
    write(disk_file, &superblock, sizeof(struct wfs_sb));

    // write inode bitmap to disk
    lseek(disk_file, superblock.i_bitmap_ptr, SEEK_SET);
    int numIbytes = (inodes + 7) / 8;
    unsigned char *i_bitmap = (unsigned char *)calloc(numIbytes, sizeof(unsigned char));
    write(disk_file, i_bitmap, numIbytes);
    free(i_bitmap);

    // write data bitmap to disk
    lseek(disk_file, superblock.d_bitmap_ptr, SEEK_SET);
    int numDbytes = (blocks) / 8;
    unsigned char *d_bitmap = (unsigned char *)calloc(numDbytes, sizeof(unsigned char));
    write(disk_file, d_bitmap, numDbytes);
    free(d_bitmap);

    // write root inode to disk
    lseek(disk_file, superblock.i_blocks_ptr, SEEK_SET);
    struct wfs_inode root_inode =
        {
            .num = 0};
    write(disk_file, &root_inode, sizeof(struct wfs_inode));
}