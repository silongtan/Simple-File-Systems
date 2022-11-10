#include "fs.h"
#include "disk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#define FS_MAGIC           0xf0f03410
#define INODES_PER_BLOCK   128
#define POINTERS_PER_INODE 5
#define POINTERS_PER_BLOCK 1024

// Returns the number of dedicated inode blocks given the disk size in blocks
#define NUM_INODE_BLOCKS(disk_size_in_blocks) (1 + (disk_size_in_blocks / 10))

static int mount_flag = 0;
static int *free_map;

struct fs_superblock {
	int magic;          // Magic bytes
	int nblocks;        // Size of the disk in number of blocks
	int ninodeblocks;   // Number of blocks dedicated to inodes
	int ninodes;        // Number of dedicated inodes
};

struct fs_inode {
	int isvalid;                      // 1 if valid (in use), 0 otherwise
	int size;                         // Size of file in bytes
	int direct[POINTERS_PER_INODE];   // Direct data block numbers (0 if invalid)
	int indirect;                     // Indirect data block number (0 if invalid)
};

union fs_block {
	struct fs_superblock super;               // Superblock
	struct fs_inode inode[INODES_PER_BLOCK];  // Block of inodes
	int pointers[POINTERS_PER_BLOCK];         // Indirect block of direct data block numbers
	char data[DISK_BLOCK_SIZE];               // Data block
};

void fs_debug()
{
	union fs_block block;

	disk_read(0,block.data);

	printf("superblock:\n");
	printf("    %d blocks\n",block.super.nblocks);
	printf("    %d inode blocks\n",block.super.ninodeblocks);
	printf("    %d inodes\n",block.super.ninodes);

	for (int i = 1; i <= block.super.ninodeblocks; i++) {
		disk_read(i, block.data);
		for (int j = 0; j < 128; j++) {
			if (block.inode[j].isvalid) {
				printf("inode %d:\n", (i-1)*128+j);
				printf("    size: %d\n", block.inode[j].size);
				// direct block seciton
				printf("    number of blocks: %ld\n", ((block.inode[j].size+4096-1)/sizeof(block)));
				printf("    direct blocks:");
				if ((block.inode[j].size+4096-1)/sizeof(block) < POINTERS_PER_INODE) {
					for (int k = 0; k < (block.inode[j].size+4096-1)/sizeof(block); k++) { // round up
						printf(" %d", block.inode[j].direct[k]);
					}
					printf("\n");
				} else {
					for (int k = 0; k < POINTERS_PER_INODE; k++) {
						printf(" %d", block.inode[j].direct[k]);
					}
					// indirect block section
					printf("\n    indirect block: %d\n", block.inode[j].indirect);
					printf("    indirect data blocks: " );
					union fs_block indir;
					disk_read(block.inode[j].indirect, indir.data);				
					for (int l = 0; l < (block.inode[j].size+4096-1)/sizeof(block)-POINTERS_PER_INODE; l++){
						printf(" %d", indir.pointers[l]);
					}
					printf("\n");
				}
			}
		}
	}
}

int fs_format()
{
	if (mount_flag) { // cannot format while mounted
		printf("Cannot format while mounted \n");
		return 0;
	}
	union fs_block block;
	// intialize superblock
	block.super.nblocks = disk_size();
	block.super.ninodeblocks = NUM_INODE_BLOCKS(disk_size());
	block.super.ninodes = block.super.ninodeblocks*128;
	block.super.magic = FS_MAGIC;
	disk_write(0, block.data);

	// intialize inode and inode blocks
	struct fs_inode inode;
	inode.isvalid = 0;
	inode.size = 0;
	for (int i = 0; i < POINTERS_PER_INODE; i++) { // clear direct blocks
		inode.direct[i] = 0;
	}
	inode.indirect = 0;
	for (int i = 0; i < INODES_PER_BLOCK; i++) { // initalize inode blocks
		block.inode[i] = inode;
	}
	for (int i = 1; i < NUM_INODE_BLOCKS(disk_size())+1; i++) {
		disk_write(i, block.data);
	}
	return 1;
}

int fs_mount()
{	
	if (mount_flag == 1) {
		// already mounted
		printf("already mounted\n");
		return 0;
	}

	union fs_block block;
	disk_read(0,block.data); // read superblock
	if (block.super.magic != FS_MAGIC) {
		printf("invalid magic number\n");
		return 0;
	}
	free_map = malloc(sizeof(int) * (block.super.nblocks - block.super.ninodeblocks));
	for (int i = 0; i < (block.super.nblocks - block.super.ninodeblocks); i++) {
		free_map[i] = 0; // initialize bit map
	}
	// inode blocks
	for (int i = 1; i < block.super.ninodeblocks+1; i++) {
		disk_read(i, block.data); // get the block content
		for (int j = 0; j < INODES_PER_BLOCK; j++) {
			if (block.inode[j].isvalid) {
				if ((block.inode[j].size+4096-1)/sizeof(block) < POINTERS_PER_INODE) { // < 5 direct blocks
					for (int k = 0; k < (block.inode[j].size+4096-1)/sizeof(block); k++) { // round up
						free_map[block.inode[j].direct[k]] = 1;
					}
				} else {
					for (int k = 0; k < POINTERS_PER_INODE; k++) { // >= 5 direct blocks, need to use indirect blocks
						free_map[block.inode[j].direct[k]] = 1;
					}
					// indirect block section
					free_map[block.inode[j].indirect] = 1;
					union fs_block indir;
					disk_read(block.inode[j].indirect, indir.data);
					for (int l = 0; l < (block.inode[j].size+4096-1)/sizeof(block)-POINTERS_PER_INODE; l++){ // number of indirect blocks
						free_map[indir.pointers[l]] = 1;
					}
				}
			}
		}
	}
	mount_flag = 1;
	return 1;
}

int fs_unmount()
{	
	if (mount_flag) {
		mount_flag = 0;
		free(free_map);
		return 1;
	} else {
		printf("already unmounted\n");
		return 0; // already unmounted
	}
}

int fs_create()
{
	if (mount_flag == 0) {
		//cannot create before mount
		printf("disk not mount\n");
		return -1; 
	}
	union fs_block block;
	disk_read(0, block.data); // read superblock

	for (int i = 1; i <= block.super.ninodeblocks; i++) {
		disk_read(i, block.data);
		for (int j = 1; j <= INODES_PER_BLOCK; j++) {
			if (block.inode[j].isvalid == 0) {
				struct fs_inode inode;
				inode.isvalid = 1;
				inode.size = 0;
				block.inode[j] = inode;
				disk_write(i, block.data);
				int inode_num = (i-1)*INODES_PER_BLOCK+j;
				return inode_num;
			}
		}
	}
	return -1;
}

int fs_delete( int inumber )
{	
	if (mount_flag == 0) {
		printf("disk not mount\n");
		return 0; 
	}
	union fs_block block;
	disk_read(0, block.data);
	if (inumber < 0 || inumber > block.super.ninodes) {
		printf("invalid inumber\n");
		return 0;
	}
	int target = inumber/INODES_PER_BLOCK + 1; // which block
	disk_read(target, block.data);
	int inode_num = inumber % INODES_PER_BLOCK; // which inode in the block
	if (block.inode[inode_num].isvalid) {
		if ((block.inode[inode_num].size+4096-1)/sizeof(block) < POINTERS_PER_INODE) { // < 5 direct blocks
			for (int i = 0; i < (block.inode[inode_num].size+4096-1)/sizeof(block); i++) { // round up
				free_map[block.inode[inode_num].direct[i]] = 0;
			}
		} else {
			for (int i = 0; i < POINTERS_PER_INODE; i++) { // >= 5 direct blocks, need to use indirect blocks
				free_map[block.inode[inode_num].direct[i]] = 0;
			}
			// indirect block section
			free_map[block.inode[inode_num].indirect] = 0;
			union fs_block indir;
			disk_read(block.inode[inode_num].indirect, indir.data);
			for (int l = 0; l < (block.inode[inode_num].size+4096-1)/sizeof(block)-POINTERS_PER_INODE; l++){ // number of indirect blocks
				free_map[indir.pointers[l]] = 0;
			}
		}
		struct fs_inode inode;
		inode.size = 0;
		inode.isvalid = 0;
		block.inode[inode_num] = inode;
		disk_write(target, block.data);
	}
	return 1;
}

int fs_getsize( int inumber )
{
	if (mount_flag == 0) {
		printf("disk not mount\n");
		return -1; 
	}
	union fs_block block;
	disk_read(0, block.data);
	if (inumber < 0 || inumber > block.super.ninodes) {
		printf("invalid inumber\n");
		return -1;
	}
	int target = inumber/INODES_PER_BLOCK + 1; // which block
	disk_read(target, block.data);
	int inode_num = inumber % INODES_PER_BLOCK; // which inode in the block
	if (block.inode[inode_num].isvalid) {
		return block.inode[inode_num].size;
	}
	return -1;
}

int fs_read( int inumber, char *data, int length, int offset )
{	
	if (mount_flag == 0) {
		printf("disk not mount\n");
		return 0; 
	}
	union fs_block block;
	disk_read(0, block.data);
	if (inumber < 0 || inumber > block.super.ninodes) {
		printf("invalid inumber\n");
		return 0;
	}
	int target = inumber/INODES_PER_BLOCK + 1; // which block
	disk_read(target, block.data);
	int inode_num = inumber % INODES_PER_BLOCK; // which inode in the block
	if (block.inode[inode_num].isvalid == 0) {
		printf("inode not valid\n");
		return 0;
	}
	if (offset > block.inode[inode_num].size-1) {
		return 0;
	}
	// number of indir blocks
	int indir_num = 0;
	int bytes_read = 0;
	int block_start = offset / DISK_BLOCK_SIZE; // block number for offset
	int up_bound = offset + length;
	if (up_bound > block.inode[inode_num].size) {
		up_bound = block.inode[inode_num].size;
	}
	int	block_end = (up_bound + 4096 - 1)/ DISK_BLOCK_SIZE; // index for the bound
	if (block_end > POINTERS_PER_INODE && block_start < POINTERS_PER_INODE) {
		indir_num = block_end - POINTERS_PER_INODE; // number of indir blocks
		block_end = POINTERS_PER_INODE;
	}
	if (block_end > POINTERS_PER_INODE && block_start > POINTERS_PER_INODE) {
		indir_num = block_end - block_start;
	}
	int	byte_offset = offset % DISK_BLOCK_SIZE;
	if (block_start < POINTERS_PER_INODE) {
		for (int i = block_start; i < block_end; i++) {
			union fs_block block_buffer;
			disk_read(block.inode[inode_num].direct[i], block_buffer.data);
			for (int j = byte_offset; j < DISK_BLOCK_SIZE; j++){
				data[bytes_read] = block_buffer.data[j];
				bytes_read++;
				if (bytes_read == block.inode[inode_num].size || bytes_read == length) { // reach the end of file
					return bytes_read;
				}
			}
			byte_offset = 0; // move to next block reset offset
		}
	}
	int indir_start = 0;
	if (block_start > POINTERS_PER_INODE) {
		indir_start = block_start-5;
	}
	// indirect block section
	if (indir_num > 0) {
		union fs_block indir;
		disk_read(block.inode[inode_num].indirect, indir.data);
		for (int l = 0; l < indir_num; l++){ // number of indirect blocks
			union fs_block block_buffer;
			disk_read(indir.pointers[l]+indir_start, block_buffer.data);
			for (int j = byte_offset; j < DISK_BLOCK_SIZE; j++){
				data[bytes_read] = block_buffer.data[j];
				bytes_read++;
				if (bytes_read == block.inode[inode_num].size || bytes_read == length) {
					return bytes_read;
				}
			}
			byte_offset = 0; // move to next block reset offset
		}
	}
	return 0;
}

int fs_write( int inumber, const char *data, int length, int offset )
{
	if (mount_flag == 0) {
		printf("disk not mount\n");
		return 0; 
	}
	union fs_block super_block;
	disk_read(0, super_block.data);
	if (inumber < 0 || inumber > super_block.super.ninodes) {
		printf("invalid inumber\n");
		return 0;
	}
	int target = inumber/INODES_PER_BLOCK + 1; // which block
	union fs_block block;
	disk_read(target, block.data);
	int inode_num = inumber % INODES_PER_BLOCK; // which inode in the block
	if (!block.inode[inode_num].isvalid) {
		printf("inode not valid\n");
		return 0;
	}
	if (offset > block.inode[inode_num].size) {
		printf("attemp to write sparse file");
		return 0;
	}
	int indir_num = 0;
	int bytes_write = 0;
	int block_start = offset / DISK_BLOCK_SIZE; // block number for offset
	int up_bound = offset + length;
	if (up_bound > (1024+5)*4096) {
		up_bound = (1024+5)*4096; // max size for a inode
	}
	if (up_bound > 4096*(super_block.super.nblocks-super_block.super.ninodeblocks-1)) {
		printf("disk small\n");
		up_bound = 4096*(super_block.super.nblocks-super_block.super.ninodeblocks-1); // if disk is very small
	}
	int	block_end = (up_bound + 4096 - 1)/ DISK_BLOCK_SIZE; // index for the bound
	if (block_end > POINTERS_PER_INODE && block_start < POINTERS_PER_INODE) {
		indir_num = block_end - POINTERS_PER_INODE; // number of indir blocks
		block_end = POINTERS_PER_INODE; // end = 5 if total blocks > 5
	}
	if (block_end > POINTERS_PER_INODE && block_start > POINTERS_PER_INODE) {
		indir_num = block_end - block_start;
	}
	int	byte_offset = offset % DISK_BLOCK_SIZE;
	int start = block.inode[inode_num].direct[block_start] + super_block.super.ninodeblocks + 1;
	if (block_start < POINTERS_PER_INODE) {
		for (int i = block_start; i < block_end; i++) {
			union fs_block block_buffer;
			disk_read(start + i, block_buffer.data);
			for (int j = byte_offset; j < DISK_BLOCK_SIZE; j++){
				block_buffer.data[j] = data[bytes_write];
				bytes_write++;
				if (bytes_write == up_bound || bytes_write == length) { // reach the end of length
					break;
				}
			}
			byte_offset = 0; // move to next block reset offset
			block.inode[inode_num].direct[i] = start + i;
			disk_write(start + i, block_buffer.data);
			if (bytes_write == up_bound || bytes_write == length) { // reach the end of length
				break;
			}
		}
	}

	int indir_start = 0;
	if (block_start > POINTERS_PER_INODE) {
		indir_start = block_start-5;
	}
	if (indir_num > 0) {
		int indir_index = block.inode[inode_num].direct[0]+5;
		union fs_block indir;
		block.inode[inode_num].indirect = indir_index;
		disk_read(indir_index, indir.data);
		for (int l = 0; l < indir_num; l++){ // number of indirect blocks
			union fs_block block_buffer;
			disk_read(indir_index+l+1+indir_start, block_buffer.data);
			for (int j = byte_offset; j < DISK_BLOCK_SIZE; j++){
				block_buffer.data[j] = data[bytes_write];
				if (bytes_write == up_bound) {
					break;
				}
				bytes_write++;
			}
			byte_offset = 0; // move to next block reset offset
			indir.pointers[l+indir_start] = indir_index+l+1+indir_start;
			disk_write(indir_index+l+1+indir_start, block_buffer.data);
			if (bytes_write == up_bound) { // reach the end of length
				break;
			}
		}
		disk_write(indir_index, indir.data);
	}
	if (bytes_write > 0) {
		block.inode[inode_num].size += bytes_write;
		disk_write(target, block.data);
	} else {
		return 0;
	}
	if (bytes_write == up_bound || bytes_write == length) { // reach the end of length
		return bytes_write;
	}
	return 0;
}
