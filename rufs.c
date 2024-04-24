/*
 *  Copyright (C) 2024 CS416/CS518 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

#define DEBUG 0
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"

char diskfile_path[PATH_MAX];

/**
 * Do not put new line char at end
 */
void my_print(const char *format, ...)
{
	if (DEBUG)
	{
		va_list args;
		printf(ANSI_COLOR_RED);
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
		printf(ANSI_COLOR_RESET "\n");
	}
}
/**
 * Do not put new line char at end
 */
void my_print_mag(const char *format, ...)
{
	if (DEBUG)
	{
		va_list args;
		printf(ANSI_COLOR_MAGENTA);
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
		printf(ANSI_COLOR_RESET "\n");
	}
}
/**
 * Always print, no matter DEBUG
 */
void my_print_always(const char *format, ...)
{
	va_list args;
	printf(ANSI_COLOR_MAGENTA);
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf(ANSI_COLOR_RESET "\n");
}

// Declare your in-memory data structures here
struct superblock *sb = NULL;
// bitmap for indoes
bitmap_t inode_bm = NULL;
// bitmap for datablocks not diskblocks
bitmap_t dblock_bm = NULL;
/*
 * Get available inode number from bitmap
 */
int get_avail_ino()
{
	// Step 1: Read inode bitmap from disk
	bio_read(sb->i_bitmap_blk, inode_bm);
	// Step 2: Traverse inode bitmap to find an available slot
	int i;
	for (i = 0; i < sb->max_inum; i++)
	{
		if (get_bitmap(inode_bm, i) == 0)
			break;
	}
	// Step 3: Update inode bitmap and write to disk
	set_bitmap(inode_bm, i);
	bio_write(sb->i_bitmap_blk, inode_bm);
	return i;
}

/*
 * Get available data block number from bitmap
 */
int get_avail_blkno()
{

	// Step 1: Read data block bitmap from disk
	bio_read(sb->d_bitmap_blk, dblock_bm);
	// Step 2: Traverse data block bitmap to find an available slot
	int i;
	for (i = 0; i < sb->max_dnum; i++)
	{
		if (get_bitmap(dblock_bm, i) == 0)
			break;
	}
	// Step 3: Update data block bitmap and write to disk
	//my_print_always("setting bitmap at i:%d", i);
	set_bitmap(dblock_bm, i);
	bio_write(sb->d_bitmap_blk, dblock_bm);
	return i;
}

/*
 * inode operations
 * unit16_t ino = [0 to MAX_INUM) = [0 to 1024)
 * 256
 */

int readi(uint16_t ino, struct inode *inode)
{
	// Step 1: Get the inode's on-disk block number = inodestart + ino/number of inodes per block
	uint16_t block_num = sb->i_start_blk + (ino / (BLOCK_SIZE / sizeof(struct inode)));

	// Step 2: Get offset of the inode in the inode on-disk block
	uint16_t offset = (ino % (BLOCK_SIZE / sizeof(struct inode)) * sizeof(struct inode));

	// Step 3: Read the block from disk and then copy into inode structure
	void *block = malloc(BLOCK_SIZE);
	bio_read(block_num, block);
	memcpy(inode, block + offset, sizeof(struct inode));
	free(block);
	return 0;
}

int writei(uint16_t ino, struct inode *inode)
{
	// Step 1: Get the block number where this inode resides on disk
	uint16_t block_num = sb->i_start_blk + (ino / (BLOCK_SIZE / sizeof(struct inode)));

	// Step 2: Get the offset in the block where this inode resides on disk
	uint16_t offset = (ino % (BLOCK_SIZE / sizeof(struct inode)) * sizeof(struct inode));

	// Step 3: Write inode to disk
	void *block = malloc(BLOCK_SIZE);
	bio_read(block_num, block);
	memcpy(block + offset, inode, sizeof(struct inode));
	bio_write(block_num, block);
	free(block);

	return 0;
}

/*
 * returns 0 on sucess, -1 on failure
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *final_dirent)
{
	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode *curr_dir_inode = malloc(sizeof(struct inode));
	struct dirent *dirents = malloc(BLOCK_SIZE);
	readi(ino, curr_dir_inode);

	if (S_ISREG(curr_dir_inode->type))
		return -1;

	// Step 2: Get data block of current directory from inode
	for (int i = 0; i < MAX_DIRECT_PTRS; i++)
	{
		if (curr_dir_inode->direct_ptr[i] == INVALID_DBLOCK)
		{
			break;
		}
		bio_read(sb->d_start_blk + curr_dir_inode->direct_ptr[i], dirents);
		for (int j = 0; j < MAX_DIRENTS_PER_DIRECT_PTR; j++)
		{
			if (dirents[j].valid == INVALID_DIRENT)
			{
				continue;
			}
			if (strcmp(dirents[j].name, fname) == 0)
			{
				// match found
				if (final_dirent != NULL)
					memcpy(final_dirent, &dirents[j], sizeof(struct dirent));
				free(curr_dir_inode);
				free(dirents);
				return 0;
			}
		}
	}
	// Step 3: Read directory's data block and check each directory entry.
	// If the name matches, then copy directory entry to dirent structure
	free(curr_dir_inode);
	free(dirents);
	return -1;
}
/*
 * returns 0 on sucess, -1 on failure
 */
int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len)
{
	my_print_mag("In Dir Add");
	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	struct dirent *dirents = malloc(BLOCK_SIZE);
	// if (S_ISREG(dir_inode.type))
	// 	return -1;

	// Step 2: Check if fname (directory name) is already used in other entries
	if (dir_find(dir_inode.ino, fname, name_len, NULL) == 0)
	{
		my_print("Dir add error. same name already exists");
		return -1;
	}

	// Step 3: Add directory entry in dir_inode's data block and write to disk
	int i;
	for (i = 0; i < MAX_DIRECT_PTRS; i++)
	{
		if (dir_inode.direct_ptr[i] == INVALID_DBLOCK)
		{
			my_print_mag("Going to add new Dirent Block at i:|%d|", i);
			break;
		}
		bio_read(sb->d_start_blk + dir_inode.direct_ptr[i], dirents);
		for (int j = 0; j < MAX_DIRENTS_PER_DIRECT_PTR; j++)
		{
			if (dirents[j].valid == INVALID_DIRENT)
			{
				dirents[j].valid = VALID_DIRENT;
				dirents[j].ino = f_ino;
				strcpy(dirents[j].name, fname);
				dirents[j].len = name_len;

				dir_inode.link += 1;
				time(&dir_inode.vstat.st_mtime);

				writei(dir_inode.ino, &dir_inode);
				bio_write(sb->d_start_blk + dir_inode.direct_ptr[i], dirents);
				free(dirents);
				my_print_mag("New Dirent Added |%s| at i:|%d| index:j|%d|", fname, i , j);
				return 0;
			}
		}
	}
	if (i == MAX_DIRECT_PTRS){
		my_print("Dir add error. max amount of dirents reached");
		my_print_mag("Exiting Dir Add");
		return -1;
	}
		
	my_print_mag("New Dirent Block Adding at I: |%d|", i);
	// Allocate a new data block for this directory if it does not exist
	int new_block = get_avail_blkno();
	struct dirent *new_dirents = calloc(1, BLOCK_SIZE);

	// Update directory inode
	dir_inode.direct_ptr[i] = new_block;
	dir_inode.size += BLOCK_SIZE;
	dir_inode.link += 1;
	time(&dir_inode.vstat.st_mtime);
	writei(dir_inode.ino, &dir_inode);

	// Write directory entry
	new_dirents->valid = VALID_DIRENT;
	new_dirents->ino = f_ino;
	strcpy(new_dirents->name, fname);
	new_dirents->len = name_len;
	bio_write(sb->d_start_blk + dir_inode.direct_ptr[i], new_dirents);
	

	free(new_dirents);
	free(dirents);

	return 0;
}

// Required for 518
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len)
{

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode

	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/*
 * absulute pathnames only. return 0 on sucess, -1 on failure.
 */
int get_node_by_path(const char *const_path, uint16_t ino, struct inode *final_inode)
{
	my_print("get_node_by_path on |%s|", const_path);
	if(strcmp("/", const_path) == 0){
		readi(0, final_inode);
		return 0;
	}
		
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	if (const_path[0] == '/')
		const_path += 1;
	char *second_part = strchr(const_path, '/');

	int len_child;

	if (second_part == NULL)
	{
		// end of path
		len_child = strlen(const_path);
	}
	else
	{
		len_child = second_part - const_path;
	}
	char *child = malloc(len_child + 1);
	strncpy(child, const_path, len_child);
	child[len_child] = '\0';

	my_print("Child of len %d is |%s|", len_child, child);

	struct dirent *child_dirent = malloc(sizeof(struct dirent));
	int found = dir_find(ino, child, len_child, child_dirent);
	free(child);

	if (second_part == NULL)
	{
		if (found == 0)
		{
			my_print("Path Resolved at inode %d", child_dirent->ino);
			if(final_inode != NULL)
				readi(child_dirent->ino, final_inode);
			return 0;
		}
		else
		{
			return -1;
		}
	}
	else
	{
		if (found == 0)
		{
			return get_node_by_path(second_part, child_dirent->ino, final_inode);
		}
		{
			return -1;
		}
	}
}

/*
 * Make file system
 */
int rufs_mkfs()
{
	my_print("MKFS START");

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);

	// write superblock information
	sb = calloc(1, BLOCK_SIZE);
	sb->magic_num = MAGIC_NUM;
	sb->i_bitmap_blk = 1;
	sb->d_bitmap_blk = sb->i_bitmap_blk + 1;
	sb->i_start_blk = sb->d_bitmap_blk + 1;
	sb->d_start_blk = sb->i_start_blk + (MAX_INUM * sizeof(struct inode)) / BLOCK_SIZE;
	sb->max_inum = MAX_INUM;
	sb->max_dnum = MAX_DNUM - sb->d_start_blk;
	bio_write(0, sb);

	// initialize inode bitmap
	inode_bm = calloc(1, BLOCK_SIZE);
	bio_write(sb->i_bitmap_blk, inode_bm);

	// initialize data block bitmap
	dblock_bm = calloc(1, BLOCK_SIZE);
	bio_write(sb->d_bitmap_blk, dblock_bm);

	// update bitmap information for root directory
	int root_ino = get_avail_ino();
	int root_dno = get_avail_blkno();
	my_print("Root Inode at inode: |%d|", root_ino);
	my_print("Root DataBlock at num: |%d|", root_dno);

	// update inode for root directory
	struct inode *root_inode = malloc(sizeof(struct inode));
	root_inode->ino = root_ino;
	root_inode->valid = VALID_INODE;
	root_inode->link = 2;
	root_inode->size = BLOCK_SIZE;
	root_inode->type = __S_IFDIR;
	for (int i = 0; i < MAX_DIRECT_PTRS; i++)
	{
		root_inode->direct_ptr[i] = INVALID_DBLOCK;
	}
	root_inode->direct_ptr[0] = root_dno;
	time(&root_inode->vstat.st_mtime);
	root_inode->vstat.st_uid = getuid();
	root_inode->vstat.st_gid = getgid();

	writei(root_ino, root_inode);

	struct dirent* dirents = calloc(1, BLOCK_SIZE);
	dirents[0].ino = root_inode->ino;
	strcpy(dirents[0].name, ".");
	dirents[0].len = strlen(dirents[0].name);
	dirents[0].valid = VALID_DIRENT;

	// no need for dot-dot 
	// dirents[1].ino = root_inode->ino;
	// strcpy(dirents[1].name, "..");
	// dirents[1].len = strlen(dirents[1].name);
	// dirents[1].valid = VALID_DIRENT;

	bio_write(sb->d_start_blk + root_inode->direct_ptr[0], dirents);

	// struct dirent *dot = dirents;
	// struct dirent *dotdot = dirents + sizeof(struct dirent);
	// dot->ino = root_inode->ino;
	// strcpy(dot->name, ".");
	// dot->len = strlen(dot->name);
	// dot->valid = VALID_DIRENT;
	// dotdot->ino = root_inode->ino;
	// strcpy(dotdot->name, "..");
	// dotdot->len = strlen(dotdot->name);
	// dotdot->valid = VALID_DIRENT;
	// bio_write(sb->d_start_blk + root_inode->direct_ptr[0], dirents);

	free(root_inode);
	free(dirents);
	return 0;
}

/*
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn)
{
	my_print("INIT START");
	if (dev_open(diskfile_path) < 0)
	{
		my_print("Making DISK. Calling mkfs");
		rufs_mkfs();
	}
	else
	{
		my_print("DISK Already Exists");
		sb = malloc(BLOCK_SIZE);
		inode_bm = malloc(BLOCK_SIZE);	// so we do not have to malloc and free everytime doing bitmap ops
		dblock_bm = malloc(BLOCK_SIZE); // so we do not have to malloc and free everytime doing bitmap ops
		bio_read(0, sb);

		//tests file io

		// tests. passed
		// TODO: delete tests
		
		// my_print("Magic Num: %d", sb->magic_num);

		// int next = get_avail_blkno();
		// my_print("Next Block %d", next);
		// next = get_avail_ino();
		// my_print("Next Inode %d", next);

		// struct inode *root_inode = malloc(sizeof(struct inode));
		// readi(0, root_inode);
		// my_print("RootNode Valid Check: %d", root_inode->valid);
		// my_print("RootNode Last mtime: %d", root_inode->vstat.st_mtime);

		// struct dirent *dirents = malloc(BLOCK_SIZE);
		// bio_read(sb->d_start_blk + root_inode->direct_ptr[0], dirents);

		// my_print("Dot Found at |%s|", dirents[0].name);
		// my_print("DotDot Found at |%s|", dirents[1].name);

		// my_print("Finding |.| in root: status |%d|", dir_find(root_inode->ino, ".", strlen("."), NULL));
		// my_print("Finding |..| in root: status |%d|", dir_find(root_inode->ino, "..", strlen(".."), NULL));
		// my_print("Adding |.| in root: status |%d|", dir_add(*root_inode, 10, ".", strlen(".")));
		// my_print("Adding |..| in root: status |%d|", dir_add(*root_inode, 10, "..", strlen("..")));
		// my_print("Finding |temp| in root: status |%d|", dir_find(root_inode->ino, "temp2", strlen("temp"), NULL));

		// struct inode *inode_temp = malloc(sizeof(struct inode));
		// inode_temp->ino = get_avail_ino();
		// inode_temp->valid = VALID_INODE;
		// inode_temp->link = 2;
		// inode_temp->size = BLOCK_SIZE;
		// inode_temp->type = __S_IFDIR;
		// for (int i = 0; i < MAX_DIRECT_PTRS; i++)
		// {
		// 	inode_temp->direct_ptr[i] = INVALID_DBLOCK;
		// }
		// inode_temp->direct_ptr[0] = get_avail_blkno();
		// time(&inode_temp->vstat.st_mtime);
		// writei(inode_temp->ino, inode_temp);
		// my_print("Adding |temp| w/ inode |%d| in root: status |%d|", inode_temp->ino, dir_add(*root_inode, inode_temp->ino, "temp2", strlen("temp2")));
		
		// struct inode *inode_cd = malloc(sizeof(struct inode));
		// inode_cd->ino = get_avail_ino();
		// inode_cd->valid = VALID_INODE;
		// inode_cd->link = 2;
		// inode_cd->size = BLOCK_SIZE;
		// inode_cd->type = __S_IFDIR;
		// for (int i = 0; i < MAX_DIRECT_PTRS; i++)
		// {
		// 	inode_cd->direct_ptr[i] = INVALID_DBLOCK;
		// }
		// inode_cd->direct_ptr[0] = get_avail_blkno();
		// time(&inode_cd->vstat.st_mtime);
		// writei(inode_cd->ino, inode_cd);
		// my_print("Adding |temp/cd| w/ inode |%d| in temp: status |%d|", inode_cd->ino, dir_add(*inode_temp, inode_cd->ino, "cd", strlen("cd")));


		// my_print("Finding |temp| in root: status |%d|", dir_find(root_inode->ino, "temp", strlen("temp"), NULL));
		// bio_read(sb->d_start_blk + root_inode->direct_ptr[0], dirents);
		// struct dirent *temp = dirents + sizeof(struct dirent) * 2;
		// my_print("Dot Found at |%s|", dirents[0].name);
		// my_print("DotDot Found at |%s|", dirents[1].name);
		// my_print("Temp Found at |%s|", dirents[2].name);
		
		// struct inode* new_in = malloc(sizeof(struct inode));
		// char* path = "/././temp/cd";
		// int stat = get_node_by_path(path, root_inode->ino, new_in);
		// my_print("Get Node at |%s| with stat |%d| and inode |%d|", path, stat, new_in->ino);

		// my_print("%d %d",S_ISDIR(inode_temp->type), S_ISDIR(inode_cd->type));
	}

	// Step 1a: If disk file is not found, call mkfs

	// Step 1b: If disk file is found, just initialize in-memory data structures
	// and read superblock from disk

	return NULL;
}
int amount_of_dblocks_used(){
	bio_read(sb->d_bitmap_blk, dblock_bm);
	int count = 0;
	for (int i = 0; i < sb->max_dnum; i++)
	{
		if (get_bitmap(dblock_bm, i) == 1)
			count++;
	}
	return count;
	
}
static void rufs_destroy(void *userdata)
{
	my_print("DESTROY START");
	//calculate how many d blocks used for report:
	my_print_always("Amount of dblocks used on this DISKFILE: %d dblocks", amount_of_dblocks_used());
	
	// Step 1: De-allocate in-memory data structures
	free(sb);
	free(inode_bm);
	free(dblock_bm);
	// Step 2: Close diskfile
	dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf)
{
	my_print("GET_ATTR START");

	// Step 1: call get_node_by_path() to get inode from path
	struct inode* in = malloc(sizeof(struct inode));
	int stat = get_node_by_path(path, 0, in);
	if(stat == -1){
		free(in);
		errno = ENOENT;
		return -ENOENT;  
	}
	// Step 2: fill attribute of file into stbuf from inode

	// stbuf->st_mode = __S_IFDIR | 0755;
	
	// stbuf->st_nlink = 2;
	// time(&stbuf->st_mtime);
	
	// st_uid
	// st_gid
	// st_nlink
	// st_size
	// st_mtime
	// st_mode
	if(stbuf != NULL){
		stbuf->st_uid = in->vstat.st_uid;
		stbuf->st_gid = in->vstat.st_gid;
		stbuf->st_nlink = in->link;
		stbuf->st_size = in->size;
		stbuf->st_mtime = in->vstat.st_mtime;
		if(S_ISDIR(in->type))
			stbuf->st_mode = in->type | 0755; //0777
		else //reg file
			stbuf->st_mode = in->type | 0644;  //TODO: 0666 in testcases
	}

	free(in);
	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi)
{
	my_print("OPEN DIR START");

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1
	struct inode* in = malloc(sizeof(struct inode));
	int stat = get_node_by_path(path, 0, in);
	if(stat == -1 || !S_ISDIR(in->type)){
		free(in);
		return -1;
	}
	free(in);
	return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	my_print("READ DIR START |%s|", path);

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode* in = malloc(sizeof(struct inode));
	int stat = get_node_by_path(path, 0, in);
	if(stat == -1 || !S_ISDIR(in->type)){
		free(in);
		return -1;
	}
	// Step 2: Read directory entries from its data blocks, and copy them to filler
	struct inode* temp = malloc(sizeof(struct inode));
	struct dirent* dirents = malloc(BLOCK_SIZE);
	for(int i = 0; i < MAX_DIRECT_PTRS; i++){
		if(in->direct_ptr[i] == INVALID_DBLOCK)
			break;
		bio_read(sb->d_start_blk + in->direct_ptr[i], dirents);
		for(int j = 0; j < MAX_DIRENTS_PER_DIRECT_PTR; j++){
			if(dirents[j].valid == VALID_DIRENT){
				my_print("filling in |%s|", dirents[j].name);
				filler(buffer, dirents[j].name, NULL, 0);
			}

		}
	}
	free(temp);
	free(dirents);
	free(in);
	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode)
{
	my_print("MAKE DIR |%s|", path);
	

	//note 2:, must call getattr() to see if file already exisits
	if(strlen(path) == 0){
		return -1;
	}
	if(rufs_getattr(path, NULL) == 0){
		return -1;
	}

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	// int len_path = strlen(path);
	// char* non_const_path = malloc(len_path+ 1);
	// strcpy(non_const_path, path);
	// my_print("%s -> %s", path, non_const_path);
	char* base_name = basename(strdup(path));
	char* dir_name = dirname(strdup(path));
	my_print("Splting |%s| into Dirname: |%s| Basname: |%s|", path, dir_name, base_name);
	if(strlen(dir_name) == 0 || strlen(base_name) == 0){
		return -1;
	}

	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode* parrent_inode = malloc(sizeof(struct inode));
	int stat = get_node_by_path(dir_name, 0, parrent_inode);
	if(stat == -1){
		return -1;
	}


	// Step 3: Call get_avail_ino() to get an available inode number
	int base_ino = get_avail_ino();
	my_print("NEW DIR INODE |%d| ----------------", base_ino);

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	stat = dir_add(*parrent_inode, base_ino, base_name, strlen(base_name));
	if(stat == -1){
		my_print("MKDIR ERRO: could not add in dir add");
		return -1;
	}

	// Step 5: Update inode for target directory
	struct inode* base_inode = malloc(sizeof(struct inode));
	base_inode->ino = base_ino;
	base_inode->link = 2;
	base_inode->size = BLOCK_SIZE;
	base_inode->type = __S_IFDIR;
	base_inode->valid = VALID_INODE;
	for(int i = 0; i < MAX_DIRECT_PTRS; i++){
		base_inode->direct_ptr[i] = INVALID_DBLOCK;
	}
	base_inode->direct_ptr[0] = get_avail_blkno();
	time(&base_inode->vstat.st_mtime);
	base_inode->vstat.st_uid = getuid();
	base_inode->vstat.st_gid = getgid();

	struct dirent* dirents = calloc(1, BLOCK_SIZE);
	dirents[0].ino = base_ino;
	strcpy(dirents[0].name, ".");
	dirents[0].len = strlen(dirents[0].name);
	dirents[0].valid = VALID_DIRENT;
	dirents[1].ino = parrent_inode->ino;
	strcpy(dirents[1].name, "..");
	dirents[1].len = strlen(dirents[1].name);
	dirents[1].valid = VALID_DIRENT;

	// Step 6: Call writei() to write inode to disk
	bio_write(sb->d_start_blk + base_inode->direct_ptr[0], dirents);
	writei(base_ino, base_inode);
	free(base_inode);
	free(parrent_inode);
	return 0;
}

// Required for 518
static int rufs_rmdir(const char *path)
{

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	my_print("CREATE FILE at |%s|", path);

	//note 2:, must call getattr() to see if file already exisits
	if(strlen(path) == 0){
		return -1;
	}
	if(rufs_getattr(path, NULL) == 0){
		return -1;
	}

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	char* base_name = basename(strdup(path));
	char* dir_name = dirname(strdup(path));
	if(strlen(dir_name) == 0 || strlen(base_name) == 0){
		return -1;
	}

	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode* parrent_inode = malloc(sizeof(struct inode));
	int stat = get_node_by_path(dir_name, 0, parrent_inode);
	if(stat == -1){
		return -1;
	}

	// Step 3: Call get_avail_ino() to get an available inode number
	int base_ino = get_avail_ino();
	my_print("NEW FILE INODE |%d| ----------------", base_ino);
	fi->fh = base_ino;

	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	stat = dir_add(*parrent_inode, base_ino, base_name, strlen(base_name));
	if(stat == -1){
		return -1;
	}

	// Step 5: Update inode for target file
	struct inode* base_inode = malloc(sizeof(struct inode));
	base_inode->ino = base_ino;
	base_inode->link = 1;
	base_inode->size = 0;
	base_inode->type = __S_IFREG;
	base_inode->valid = VALID_INODE;
	for(int i = 0; i < MAX_DIRECT_PTRS; i++){
		base_inode->direct_ptr[i] = INVALID_DBLOCK;
	}
	// //TODO: remove
	// base_inode->direct_ptr[0] = get_avail_blkno();
	// base_inode->direct_ptr[1] = get_avail_blkno();
	// base_inode->direct_ptr[2] = get_avail_blkno();
	
	time(&base_inode->vstat.st_mtime);
	base_inode->vstat.st_uid = getuid();
	base_inode->vstat.st_gid = getgid();

	// Step 6: Call writei() to write inode to disk
	writei(base_ino, base_inode);
	free(base_inode);
	free(parrent_inode);
	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi)
{
	my_print("OPEN FILE START");

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1
	struct inode* in = malloc(sizeof(struct inode));
	int stat = get_node_by_path(path, 0, in);
	if(stat == -1 || !S_ISREG(in->type)){
		free(in);
		return -1;
	}
	fi->fh = in->ino;
	free(in);
	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	// offset = 10;
	// size = 4 * BLOCK_SIZE - 1000;
	my_print("READ |%d| bytes from |%s| starting from |%d|", size, path, offset);

	// Step 1: Use fi to get ino of the file
	struct inode* f_inode = malloc(sizeof(struct inode));
	readi(fi->fh, f_inode);
	if(offset > f_inode->size){
		free(f_inode);
		return -1;
	}
	// Step 2: Based on size and offset, read its data blocks from disk


	void* data_block = malloc(BLOCK_SIZE);
	
	int sor_i = offset / BLOCK_SIZE; //starting direct pointer index
	int i = sor_i;
	int eof_i = f_inode->size / BLOCK_SIZE;
	int rem = size;
	int total = 0;
	my_print("Starting Block: %d", sor_i);
	my_print("Actual EOF Blokc: %d", eof_i);
	my_print("Remaing to read: %d", rem);
	while(rem > 0 && f_inode->direct_ptr[i] != INVALID_DBLOCK) {
		//if at starting block (aka the block that contatins the offset), make sure you start to read from the offset
		int start = (i == sor_i) ? (offset % BLOCK_SIZE): 0;
		//max amount of data we can read from this block. if at the end of file, then max amount of data is diff. And must subtract offset if at starting block
		int data_in_block = ((i == eof_i) ? f_inode->size % BLOCK_SIZE : BLOCK_SIZE) - start;
		//the amount of bytes to read based on how much is left to read
		int bytes_to_read =( rem > data_in_block) ? data_in_block : rem;
		
		bio_read(sb->d_start_blk + f_inode->direct_ptr[i], data_block);
		memcpy(buffer, data_block + start, bytes_to_read);
		my_print("Copied |%d| starting from |%d| @ block i:%d|%d|w|%d|", bytes_to_read, start, i, f_inode->direct_ptr[i], data_in_block);
		
		//update ptrs and counts
		buffer += bytes_to_read;
		total += bytes_to_read;
		rem -= bytes_to_read;
		i++;
	}
	my_print("TOTAL AMOUNT READ |%d| bytes", total);
	my_print("Freeing Data Block");
	free(data_block);
	my_print("Freed Data Block. Feeing INode");
	free(f_inode);
	my_print("Freed Inode");
	return total;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	my_print("WRITE |%d| bytes to |%s| starting from |%d|", size, path, offset);

	// Step 1: Use fi to get ino of file
	struct inode* f_inode = malloc(sizeof(struct inode));
	readi(fi->fh, f_inode);
	printf("Found Inode #%d of ISDIR=%d", f_inode->ino, S_ISDIR(f_inode->type));

	if(offset > f_inode->size){
		free(f_inode);
		return -1;
	}
	void* data_block = malloc(BLOCK_SIZE);

	int sow_i = offset / BLOCK_SIZE;
	int i = sow_i;
	int eow_i = (offset + size) / BLOCK_SIZE;
	int rem = size;
	int total = 0;
	my_print("starting in Block i:%d|%d|", i, f_inode->direct_ptr[i]);
	my_print("starting while loop");
	while(rem > 0 && i < MAX_DIRECT_PTRS) {
		int start = (i == sow_i) ? (offset % BLOCK_SIZE) : 0;
		int data_in_block = ((i == eow_i) ? (offset + size) % BLOCK_SIZE : BLOCK_SIZE) - start;
		int bytes_to_write = (rem > (BLOCK_SIZE - start)) ? (BLOCK_SIZE - start) : rem;

		if(f_inode->direct_ptr[i] == INVALID_DBLOCK) {
			f_inode->direct_ptr[i] = get_avail_blkno();
			my_print("New Block Allocated at i:%d|%d|", i , f_inode->direct_ptr[i]);
		} else if(i == sow_i || i == eow_i){
			bio_read(sb->d_start_blk + f_inode->direct_ptr[i], data_block);
		}
		memcpy(data_block + start, buffer, bytes_to_write);
		bio_write(sb->d_start_blk + f_inode->direct_ptr[i], data_block);
		my_print("Writen |%d| starting from |%d| @ block i:%d|%d|w|%d|", bytes_to_write, start, i, f_inode->direct_ptr[i], (BLOCK_SIZE - start));

		buffer += bytes_to_write;
		total += bytes_to_write;
		rem -= bytes_to_write;
		i++;
	}
	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	my_print("TOTAL AMOUNT WRiting |%d| bytes", total);
	f_inode->size = offset + total;
	writei(f_inode->ino, f_inode);
	free(f_inode);
	free(data_block);
	return total;
}

// Required for 518

static int rufs_unlink(const char *path)
{

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2])
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static struct fuse_operations rufs_ope = {
	.init = rufs_init,
	.destroy = rufs_destroy,

	.getattr = rufs_getattr,
	.readdir = rufs_readdir,
	.opendir = rufs_opendir,
	.releasedir = rufs_releasedir,
	.mkdir = rufs_mkdir,
	.rmdir = rufs_rmdir,

	.create = rufs_create,
	.open = rufs_open,
	.read = rufs_read,
	.write = rufs_write,
	.unlink = rufs_unlink,

	.truncate = rufs_truncate,
	.flush = rufs_flush,
	.utimens = rufs_utimens,
	.release = rufs_release};

int main(int argc, char *argv[])
{
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");
	my_print("Sizeof dirent %d MAx %d", sizeof(struct dirent), MAX_DIRENTS_PER_DIRECT_PTR);
	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}
