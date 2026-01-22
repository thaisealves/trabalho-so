/*
 *  myfs.c - Implementacao do sistema de arquivos MyFS
 *
 *  Autores: Hugo Ricardo Giles Nicolau - 202435003 e Thaíse Silva Alves - 202435038
 *  Projeto: Trabalho Pratico II - Sistemas Operacionais
 *  Organizacao: Universidade Federal de Juiz de Fora
 *  Departamento: Dep. Ciencia da Computacao
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"

#define MYFS_MAGIC 0x4D594653
#define INODE_BEGINSECTOR 2
#define INODE_SIZE 16

typedef struct
{
	unsigned int magic;
	unsigned int blockSize;
	unsigned int numBlocks;
	unsigned int numInodes;
	unsigned int inodeTableStart;
	unsigned int dataBlockStart;
	unsigned int freeBlockList;
	unsigned int rootInode;
} superblock;

superblock sb;

typedef struct
{
	int used;
	Disk *disk;
	unsigned int inodeNum;
	unsigned int cursor;
	Inode *inode;
} FileDescriptor;

static FileDescriptor fdTable[MAX_FDS];

#define MAX_FILE_ENTRIES 128
typedef struct
{
	char path[MAX_FILENAME_LENGTH + 1];
	unsigned int inodeNum;
} FileEntry;

static FileEntry fileTable[MAX_FILE_ENTRIES];

static int findFileEntry(const char *path)
{
	for (int i = 0; i < MAX_FILE_ENTRIES; i++)
	{
		if (fileTable[i].inodeNum != 0 && strcmp(fileTable[i].path, path) == 0)
		{
			return i;
		}
	}
	return -1;
}

static int addFileEntry(const char *path, unsigned int inodeNum)
{
	for (int i = 0; i < MAX_FILE_ENTRIES; i++)
	{
		if (fileTable[i].inodeNum == 0)
		{
			fileTable[i].inodeNum = inodeNum;
			strncpy(fileTable[i].path, path, MAX_FILENAME_LENGTH);
			fileTable[i].path[MAX_FILENAME_LENGTH] = '\0';
			return i;
		}
	}
	return -1;
}

int myFSIsIdle(Disk *d)
{
	for (int i = 0; i < MAX_FDS; i++)
	{
		if (fdTable[i].used && fdTable[i].disk == d)
		{
			return 0;
		}
	}
	return 1;
}

static unsigned int allocateFreeBlock(Disk *d)
{
	if (d == NULL || sb.magic != MYFS_MAGIC)
	{
		return 0;
	}

	if (sb.freeBlockList == 0)
	{
		return 0;
	}

	unsigned int freeBlock = sb.freeBlockList;

	unsigned char buffer[DISK_SECTORDATASIZE];
	if (diskReadSector(d, freeBlock, buffer) != 0)
	{
		return 0;
	}

	unsigned int nextFree;
	char2ul(buffer, &nextFree);

	sb.freeBlockList = nextFree;

	unsigned char sbBuffer[DISK_SECTORDATASIZE];
	ul2char(sb.magic, &sbBuffer[0]);
	ul2char(sb.blockSize, &sbBuffer[4]);
	ul2char(sb.numBlocks, &sbBuffer[8]);
	ul2char(sb.numInodes, &sbBuffer[12]);
	ul2char(sb.inodeTableStart, &sbBuffer[16]);
	ul2char(sb.dataBlockStart, &sbBuffer[20]);
	ul2char(sb.freeBlockList, &sbBuffer[24]);
	ul2char(sb.rootInode, &sbBuffer[28]);
	for (int i = 32; i < DISK_SECTORDATASIZE; i++)
	{
		sbBuffer[i] = 0;
	}
	diskWriteSector(d, 0, sbBuffer);

	return freeBlock;
}

int myFSFormat(Disk *d, unsigned int blockSize)
{
	if (d == NULL || blockSize == 0 || blockSize % DISK_SECTORDATASIZE != 0)
	{
		return -1;
	}

	unsigned long numSectors = diskGetNumSectors(d);
	unsigned int numInodes = 128;
	unsigned int inodesPerSector = inodeNumInodesPerSector();
	unsigned int inodeSectors = (numInodes + inodesPerSector - 1) / inodesPerSector;
	unsigned int inodeTableStart = 2;
	unsigned int sectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
	unsigned int dataBlockStart = inodeTableStart + inodeSectors;
	unsigned int dataAreaSectors = numSectors - dataBlockStart;
	unsigned int numBlocks = dataAreaSectors / sectorsPerBlock;

	sb.magic = MYFS_MAGIC;
	sb.blockSize = blockSize;
	sb.numBlocks = numBlocks;
	sb.numInodes = numInodes;
	sb.inodeTableStart = inodeTableStart;
	sb.dataBlockStart = dataBlockStart;
	sb.freeBlockList = 0;
	sb.rootInode = 1;

	unsigned char buffer[DISK_SECTORDATASIZE];
	ul2char(sb.magic, &buffer[0]);
	ul2char(sb.blockSize, &buffer[4]);
	ul2char(sb.numBlocks, &buffer[8]);
	ul2char(sb.numInodes, &buffer[12]);
	ul2char(sb.inodeTableStart, &buffer[16]);
	ul2char(sb.dataBlockStart, &buffer[20]);
	ul2char(sb.freeBlockList, &buffer[24]);
	ul2char(sb.rootInode, &buffer[28]);

	for (int i = 32; i < DISK_SECTORDATASIZE; i++)
	{
		buffer[i] = 0;
	}

	if (diskWriteSector(d, 0, buffer) != 0)
	{
		return -1;
	}

	unsigned char zeroBuffer[DISK_SECTORDATASIZE];
	memset(zeroBuffer, 0, DISK_SECTORDATASIZE);

	for (unsigned int i = 0; i < inodeSectors; i++)
	{
		if (diskWriteSector(d, inodeTableStart + i, zeroBuffer) != 0)
		{
			return -1;
		}
	}

	unsigned char blockBuffer[DISK_SECTORDATASIZE];
	memset(blockBuffer, 0, DISK_SECTORDATASIZE);

	for (unsigned int i = 0; i < numBlocks; i++)
	{
		unsigned int currentBlockSector = dataBlockStart + (i * sectorsPerBlock);
		unsigned int nextBlockSector;

		if (i < numBlocks - 1)
		{
			nextBlockSector = dataBlockStart + ((i + 1) * sectorsPerBlock);
		}
		else
		{
			nextBlockSector = 0;
		}

		ul2char(nextBlockSector, blockBuffer);

		if (diskWriteSector(d, currentBlockSector, blockBuffer) != 0)
		{
			return -1;
		}
	}

	sb.freeBlockList = dataBlockStart;

	ul2char(sb.magic, &buffer[0]);
	ul2char(sb.blockSize, &buffer[4]);
	ul2char(sb.numBlocks, &buffer[8]);
	ul2char(sb.numInodes, &buffer[12]);
	ul2char(sb.inodeTableStart, &buffer[16]);
	ul2char(sb.dataBlockStart, &buffer[20]);
	ul2char(sb.freeBlockList, &buffer[24]);
	ul2char(sb.rootInode, &buffer[28]);

	if (diskWriteSector(d, 0, buffer) != 0)
	{
		return -1;
	}

	for (unsigned int inodeNum = 1; inodeNum <= numInodes; inodeNum++)
	{
		Inode *tmp = inodeCreate(inodeNum, d);
		if (tmp == NULL)
		{
			return -1;
		}
		free(tmp);
	}

	Inode *rootInode = inodeLoad(1, d);
	if (rootInode == NULL)
	{
		return -1;
	}

	unsigned int rootBlock = allocateFreeBlock(d);
	if (rootBlock == 0)
	{
		free(rootInode);
		return -1;
	}

	if (inodeAddBlock(rootInode, rootBlock) != 0)
	{
		free(rootInode);
		return -1;
	}

	inodeSetFileType(rootInode, FILETYPE_DIR);
	inodeSetFileSize(rootInode, 0);
	inodeSetOwner(rootInode, 0);
	inodeSetGroupOwner(rootInode, 0);
	inodeSetPermission(rootInode, 0755);

	if (inodeSave(rootInode) != 0)
	{
		free(rootInode);
		return -1;
	}

	free(rootInode);

	return numBlocks;
}

int myFSxMount(Disk *d, int x)
{
	if (d == NULL)
	{
		return 0;
	}

	if (x == 1)
	{
		unsigned char buffer[DISK_SECTORDATASIZE];
		if (diskReadSector(d, 0, buffer) != 0)
		{
			return 0;
		}

		char2ul(&buffer[0], &sb.magic);
		char2ul(&buffer[4], &sb.blockSize);
		char2ul(&buffer[8], &sb.numBlocks);
		char2ul(&buffer[12], &sb.numInodes);
		char2ul(&buffer[16], &sb.inodeTableStart);
		char2ul(&buffer[20], &sb.dataBlockStart);
		char2ul(&buffer[24], &sb.freeBlockList);
		char2ul(&buffer[28], &sb.rootInode);

		if (sb.magic != MYFS_MAGIC)
		{
			return 0;
		}

		if (sb.blockSize == 0 || sb.blockSize % DISK_SECTORDATASIZE != 0)
		{
			return 0;
		}

		if (sb.numBlocks == 0 || sb.numInodes == 0)
		{
			return 0;
		}

		memset(fdTable, 0, sizeof(fdTable));
		memset(fileTable, 0, sizeof(fileTable));

		return 1;
	}

	if (x == 0)
	{
		if (!myFSIsIdle(d))
		{
			return 0;
		}

		for (int i = 0; i < MAX_FDS; i++)
		{
			if (fdTable[i].disk == d)
			{
				fdTable[i].used = 0;
				fdTable[i].disk = NULL;
				fdTable[i].inodeNum = 0;
				fdTable[i].cursor = 0;
				if (fdTable[i].inode != NULL)
				{
					free(fdTable[i].inode);
					fdTable[i].inode = NULL;
				}
			}
		}

		memset(&sb, 0, sizeof(sb));

		return 1;
	}

	return 0;
}

int myFSOpen(Disk *d, const char *path)
{
	if (d == NULL || path == NULL || strlen(path) == 0)
	{
		return -1;
	}

	if (strlen(path) > MAX_FILENAME_LENGTH)
	{
		return -1;
	}

	int fd = -1;
	for (int i = 0; i < MAX_FDS; i++)
	{
		if (!fdTable[i].used)
		{
			fd = i;
			break;
		}
	}

	if (fd == -1)
	{
		return -1;
	}

	unsigned int inodeNum = 0;
	Inode *inode = NULL;

	int entryIdx = findFileEntry(path);
	if (entryIdx >= 0)
	{
		inodeNum = fileTable[entryIdx].inodeNum;
		inode = inodeLoad(inodeNum, d);
		if (inode == NULL)
		{
			return -1;
		}
	}
	else
	{
		inodeNum = inodeFindFreeInode(2, d);
		if (inodeNum == 0)
		{
			return -1;
		}

		unsigned int firstBlock = allocateFreeBlock(d);
		if (firstBlock == 0)
		{
			return -1;
		}

		inode = inodeCreate(inodeNum, d);
		if (inode == NULL)
		{
			return -1;
		}

		inodeSetFileType(inode, FILETYPE_REGULAR);
		inodeSetFileSize(inode, 0);
		inodeSetOwner(inode, 0);
		inodeSetGroupOwner(inode, 0);
		inodeSetPermission(inode, 0644);

		if (inodeAddBlock(inode, firstBlock) != 0)
		{
			free(inode);
			return -1;
		}

		if (addFileEntry(path, inodeNum) < 0)
		{
			free(inode);
			return -1;
		}
	}

	fdTable[fd].used = 1;
	fdTable[fd].disk = d;
	fdTable[fd].inodeNum = inodeNum;
	fdTable[fd].cursor = 0;
	fdTable[fd].inode = inode;

	return fd + 1;
}

int myFSRead(int fd, char *buf, unsigned int nbytes)
{
	int idx = fd - 1;

	if (idx < 0 || idx >= MAX_FDS || !fdTable[idx].used)
	{
		return -1;
	}

	if (buf == NULL || nbytes == 0)
	{
		return -1;
	}

	Inode *inode = fdTable[idx].inode;
	Disk *disk = fdTable[idx].disk;
	if (inode == NULL || disk == NULL)
	{
		return -1;
	}

	unsigned int fileSize = inodeGetFileSize(inode);
	unsigned int cursor = fdTable[idx].cursor;

	if (cursor >= fileSize)
	{
		return 0;
	}

	unsigned int bytesToRead = nbytes;
	if (cursor + bytesToRead > fileSize)
	{
		bytesToRead = fileSize - cursor;
	}

	unsigned int totalRead = 0;
	unsigned int blockSize = sb.blockSize;

	while (totalRead < bytesToRead)
	{
		unsigned int currentPos = cursor + totalRead;
		unsigned int blockNum = currentPos / blockSize;
		unsigned int offsetInBlock = currentPos % blockSize;

		unsigned int blockAddr = inodeGetBlockAddr(inode, blockNum);
		if (blockAddr == 0)
		{
			break;
		}

		unsigned char blockData[blockSize];
		unsigned int numSectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
		for (unsigned int i = 0; i < numSectorsPerBlock; i++)
		{
			if (diskReadSector(disk, blockAddr + i, blockData + i * DISK_SECTORDATASIZE) != 0)
			{
				return -1;
			}
		}

		unsigned int bytesFromBlock = blockSize - offsetInBlock;
		if (bytesFromBlock > bytesToRead - totalRead)
		{
			bytesFromBlock = bytesToRead - totalRead;
		}

		memcpy(buf + totalRead, blockData + offsetInBlock, bytesFromBlock);
		totalRead += bytesFromBlock;
	}

	fdTable[idx].cursor += totalRead;

	return totalRead;
}

int myFSWrite(int fd, const char *buf, unsigned int nbytes)
{
	int idx = fd - 1;

	if (idx < 0 || idx >= MAX_FDS || !fdTable[idx].used)
	{
		return -1;
	}

	if (buf == NULL || nbytes == 0)
	{
		return -1;
	}

	Inode *inode = fdTable[idx].inode;
	Disk *disk = fdTable[idx].disk;
	if (inode == NULL || disk == NULL)
	{
		return -1;
	}

	unsigned int cursor = fdTable[idx].cursor;
	unsigned int totalWritten = 0;
	unsigned int blockSize = sb.blockSize;

	while (totalWritten < nbytes)
	{
		unsigned int currentPos = cursor + totalWritten;
		unsigned int blockNum = currentPos / blockSize;
		unsigned int offsetInBlock = currentPos % blockSize;

		unsigned int blockAddr = inodeGetBlockAddr(inode, blockNum);
		if (blockAddr == 0)
		{
			blockAddr = allocateFreeBlock(disk);
			if (blockAddr == 0)
			{
				return -1;
			}

			if (inodeAddBlock(inode, blockAddr) != 0)
			{
				return -1;
			}
		}

		unsigned char blockData[blockSize];
		unsigned int numSectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
		for (unsigned int i = 0; i < numSectorsPerBlock; i++)
		{
			if (diskReadSector(disk, blockAddr + i, blockData + i * DISK_SECTORDATASIZE) != 0)
			{
				return -1;
			}
		}

		unsigned int bytesToBlock = blockSize - offsetInBlock;
		if (bytesToBlock > nbytes - totalWritten)
		{
			bytesToBlock = nbytes - totalWritten;
		}

		memcpy(blockData + offsetInBlock, buf + totalWritten, bytesToBlock);

		for (unsigned int i = 0; i < numSectorsPerBlock; i++)
		{
			if (diskWriteSector(disk, blockAddr + i, blockData + i * DISK_SECTORDATASIZE) != 0)
			{
				return -1;
			}
		}

		totalWritten += bytesToBlock;
	}

	fdTable[idx].cursor += totalWritten;

	unsigned int newSize = fdTable[idx].cursor;
	unsigned int oldSize = inodeGetFileSize(inode);
	if (newSize > oldSize)
	{
		inodeSetFileSize(inode, newSize);
		inodeSave(inode);
	}

	return totalWritten;
}

int myFSClose(int fd)
{
	int idx = fd - 1;

	if (idx < 0 || idx >= MAX_FDS)
	{
		return -1;
	}

	if (!fdTable[idx].used)
	{
		return -1;
	}

	if (fdTable[idx].inode != NULL)
	{
		free(fdTable[idx].inode);
		fdTable[idx].inode = NULL;
	}

	fdTable[idx].used = 0;
	fdTable[idx].disk = NULL;
	fdTable[idx].inodeNum = 0;
	fdTable[idx].cursor = 0;

	return 0;
}

int myFSOpenDir(Disk *d, const char *path)
{
	(void)d;
	(void)path;
	return -1;
}

int myFSReadDir(int fd, char *filename, unsigned int *inumber)
{
	(void)fd;
	(void)filename;
	(void)inumber;
	return -1;
}

int myFSLink(int fd, const char *filename, unsigned int inumber)
{
	(void)fd;
	(void)filename;
	(void)inumber;
	return -1;
}

int myFSUnlink(int fd, const char *filename)
{
	(void)fd;
	(void)filename;
	return -1;
}

int myFSCloseDir(int fd)
{
	(void)fd;
	return -1;
}

static FSInfo myFSInfo;

int installMyFS(void)
{
	myFSInfo.fsid = 0;
	myFSInfo.fsname = "MyFS";
	myFSInfo.isidleFn = myFSIsIdle;
	myFSInfo.formatFn = myFSFormat;
	myFSInfo.xMountFn = myFSxMount;
	myFSInfo.openFn = myFSOpen;
	myFSInfo.readFn = myFSRead;
	myFSInfo.writeFn = myFSWrite;
	myFSInfo.closeFn = myFSClose;
	myFSInfo.opendirFn = myFSOpenDir;
	myFSInfo.readdirFn = myFSReadDir;
	myFSInfo.linkFn = myFSLink;
	myFSInfo.unlinkFn = myFSUnlink;
	myFSInfo.closedirFn = myFSCloseDir;

	int slot = vfsRegisterFS(&myFSInfo);
	if (slot < 0)
	{
		return -1;
	}

	return slot;
}
