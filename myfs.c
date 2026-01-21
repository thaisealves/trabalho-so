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

// Declaracoes globais
// superbloco pro so saber onde estão as coisas
#define MYFS_MAGIC 0x4D594653
#define INODE_BEGINSECTOR 2     // Setor a partir do qual i-nodes são gravados
#define INODE_SIZE 16           // Tamanho do i-node em numero de unsigned ints

typedef struct
{
	unsigned int magic;		// identificador do sistema de arquivos
	unsigned int blockSize; // bytes
	unsigned int numBlocks;
	unsigned int numInodes;
	unsigned int inodeTableStart;
	unsigned int dataBlockStart;
	unsigned int freeBlockList;
	unsigned int rootInode; // diretorio raiz, numero do inode
} superblock;

superblock sb;

// Estrutura para descritor de arquivo aberto
typedef struct {
	int used;              // 1 = em uso, 0 = livre
	Disk *disk;            // disco associado
	unsigned int inodeNum; // numero do inode do arquivo
	unsigned int cursor;   // posicao atual de leitura/escrita
	Inode *inode;          // ponteiro para o inode em memoria
} FileDescriptor;

// Tabela de descritores de arquivo (MAX_FDS = 128, definido em vfs.h)
static FileDescriptor fdTable[MAX_FDS];

// Funcao para verificacao se o sistema de arquivos está ocioso, ou seja,
// se nao ha quisquer descritores de arquivos em uso atualmente. Retorna
// um positivo se ocioso ou, caso contrario, 0.
int myFSIsIdle(Disk *d)
{
	// Percorre a tabela de descritores procurando algum em uso neste disco
	for (int i = 0; i < MAX_FDS; i++) {
		if (fdTable[i].used && fdTable[i].disk == d) {
			printf("[DEBUG myFSIsIdle] Descritor %d ainda em uso no disco %p\n", i, (void *)d);
			return 0;  // Encontrou arquivo aberto, NAO esta ocioso
		}
	}
	printf("[DEBUG myFSIsIdle] Sistema de arquivos está ocioso\n");
	return 1;  // Nenhum arquivo aberto, sistema OCIOSO
}

// Funcao auxiliar para alocar um bloco livre no disco
// Retorna o setor inicial do bloco alocado, ou 0 se nenhum bloco livre
static unsigned int allocateFreeBlock(Disk *d)
{
	if (d == NULL || sb.magic != MYFS_MAGIC) {
		printf("[DEBUG allocateFreeBlock] ERRO: Disco ou superbloco inválido\n");
		return 0;
	}

	unsigned int sectorsPerBlock = sb.blockSize / DISK_SECTORDATASIZE;
	unsigned int totalBlocks = sb.numBlocks;
	unsigned int dataBlockStart = sb.dataBlockStart;

	printf("[DEBUG allocateFreeBlock] Procurando bloco livre (total: %u blocos)\n", totalBlocks);

	// Criar um array para rastrear blocos usados
	// Verificar todos os inodes e marcar blocos alocados
	unsigned char usedBlocks[totalBlocks];
	memset(usedBlocks, 0, sizeof(usedBlocks));

	// Percorrer todos os inodes possíveis e marcar seus blocos como usados
	for (unsigned int inodeNum = 1; inodeNum <= sb.numInodes; inodeNum++) {
		Inode *checkInode = inodeLoad(inodeNum, d);
		if (checkInode != NULL) {
			// Marcar blocos deste inode como usados
			unsigned int numBlocksInInode = inodeNumBlockAddresses();
			for (unsigned int i = 0; i < numBlocksInInode; i++) {
				unsigned int blockAddr = inodeGetBlockAddr(checkInode, i);
				if (blockAddr > 0) {
					// Calcular número do bloco a partir do setor
					unsigned int blockNum = (blockAddr - dataBlockStart) / sectorsPerBlock;
					if (blockNum < totalBlocks) {
						usedBlocks[blockNum] = 1;
					}
				}
			}
			free(checkInode);
		}
	}

	// Encontrar primeiro bloco não usado
	for (unsigned int blockNum = 0; blockNum < totalBlocks; blockNum++) {
		if (usedBlocks[blockNum] == 0) {
			unsigned int blockSector = dataBlockStart + (blockNum * sectorsPerBlock);
			printf("[DEBUG allocateFreeBlock] Alocando bloco %u no setor %u\n", blockNum, blockSector);
			return blockSector;
		}
	}

	printf("[DEBUG allocateFreeBlock] ERRO: Nenhum bloco livre encontrado\n");
	return 0; // Nenhum bloco livre
}

// Funcao para formatacao de um disco com o novo sistema de arquivos
// com tamanho de blocos igual a blockSize. Retorna o numero total de
// blocos disponiveis no disco, se formatado com sucesso. Caso contrario,
// retorna -1.
int myFSFormat(Disk *d, unsigned int blockSize)
{
	// PASSO 1: Validar parametros
	// printf("\n[DEBUG myFSFormat] Entrada: d=%p, blockSize=%u\n", (void*)d, blockSize);

	if (d == NULL || blockSize == 0 || blockSize % DISK_SECTORDATASIZE != 0) {
		// printf("[DEBUG myFSFormat] ERRO: Validacao falhou!\n");
		return -1;  // blockSize deve ser multiplo do tamanho do setor
	}

	// PASSO 2: Calcular layout do disco
	unsigned long diskSizeBytes = diskGetSize(d);
	unsigned long numSectors = diskGetNumSectors(d);

	// printf("[DEBUG myFSFormat] Disco: %lu bytes (%lu setores)\n", diskSizeBytes, numSectors);

	// Numero fixo de i-nodes (opcao A - simples)
	unsigned int numInodes = 128;

	// Calcular quantos setores os i-nodes ocupam
	unsigned int inodesPerSector = inodeNumInodesPerSector();
	unsigned int inodeSectors = (numInodes + inodesPerSector - 1) / inodesPerSector;

	// I-nodes começam no setor 2 (setor 0-1 reservado para superbloco)
	unsigned int inodeTableStart = 2;

	// Blocos de dados começam após os i-nodes
	unsigned int sectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
	unsigned int dataBlockStart = inodeTableStart + inodeSectors;

	// Calcular numero de blocos disponiveis para dados
	unsigned int dataAreaSectors = numSectors - dataBlockStart;
	unsigned int numBlocks = dataAreaSectors / sectorsPerBlock;

	// PASSO 3: Preencher superbloco
	sb.magic = MYFS_MAGIC;
	sb.blockSize = blockSize;
	sb.numBlocks = numBlocks;
	sb.numInodes = numInodes;
	sb.inodeTableStart = inodeTableStart;
	sb.dataBlockStart = dataBlockStart;
	sb.freeBlockList = 0;  // pode ser usado depois para bitmap
	sb.rootInode = 1;      // diretorio raiz sera o i-node 1 (inode 0 nao e valido)

	// PASSO 4: Escrever superbloco no disco (setor 0)
	// printf("[DEBUG myFSFormat] Alocando buffer...\n");
	unsigned char buffer[DISK_SECTORDATASIZE];

	// Converter campos do superbloco para bytes
	// printf("[DEBUG myFSFormat] Convertendo superbloco para bytes...\n");
	// printf("[DEBUG myFSFormat] magic=0x%X, blockSize=%u, numBlocks=%u\n",
	//        sb.magic, sb.blockSize, sb.numBlocks);

	ul2char(sb.magic, &buffer[0]);
	ul2char(sb.blockSize, &buffer[4]);
	ul2char(sb.numBlocks, &buffer[8]);
	ul2char(sb.numInodes, &buffer[12]);
	ul2char(sb.inodeTableStart, &buffer[16]);
	ul2char(sb.dataBlockStart, &buffer[20]);
	ul2char(sb.freeBlockList, &buffer[24]);
	ul2char(sb.rootInode, &buffer[28]);

	// printf("[DEBUG myFSFormat] Conversao concluida\n");

	// Preencher resto do buffer com zeros
	for (int i = 32; i < DISK_SECTORDATASIZE; i++) {
		buffer[i] = 0;
	}

	// Depuração: Verificar superbloco antes de gravar
	printf("[DEBUG myFSFormat] Superbloco: magic=0x%X, blockSize=%u, numBlocks=%u\n",
		sb.magic, sb.blockSize, sb.numBlocks);

	if (diskWriteSector(d, 0, buffer) != 0) {
		printf("[DEBUG myFSFormat] ERRO: Falha ao escrever superbloco no setor 0\n");
		return -1;  // falha ao escrever superbloco
	}

	printf("[DEBUG myFSFormat] Superbloco gravado com sucesso no setor 0\n");

	// PASSO 5: Criar i-node do diretorio raiz
	// printf("[DEBUG myFSFormat] Criando i-node raiz (numero 1)...\n");
	Inode *rootInode = inodeCreate(1, d);
	if (rootInode == NULL) {
		// printf("[DEBUG myFSFormat] ERRO: inodeCreate retornou NULL\n");
		return -1;  // falha ao criar i-node raiz
	}
	// printf("[DEBUG myFSFormat] I-node raiz criado com sucesso\n");

	// Configurar i-node raiz como diretorio
	inodeSetFileType(rootInode, FILETYPE_DIR);
	inodeSetFileSize(rootInode, 0);
	inodeSetOwner(rootInode, 0);
	inodeSetGroupOwner(rootInode, 0);
	inodeSetPermission(rootInode, 0755);

	// Salvar i-node raiz no disco
	if (inodeSave(rootInode) != 0) {
		free(rootInode);
		return -1;  // falha ao salvar i-node raiz
	}

	free(rootInode);

	// PASSO 6: Inicializar bitmap de blocos livres (TODO: implementar depois)
	// Por enquanto, assumimos que todos os blocos estao livres

	// PASSO 7: Retornar numero de blocos disponiveis
	// printf("[DEBUG myFSFormat] Sucesso! Retornando %u blocos\n", numBlocks);
	return numBlocks;
}

// Funcao para montagem/desmontagem do sistema de arquivos, se possível.
// Na montagem (x=1) e' a chance de se fazer inicializacoes, como carregar
// o superbloco na memoria. Na desmontagem (x=0), quaisquer dados pendentes
// de gravacao devem ser persistidos no disco. Retorna um positivo se a
// montagem ou desmontagem foi bem sucedida ou, caso contrario, 0.
int myFSxMount(Disk *d, int x)
{
	// PASSO 1: Validar parametros
	if (d == NULL) {
		return 0;  // falha: disco invalido
	}

	// PASSO 2: MONTAGEM (x=1)
	if (x == 1) {
		// Ler superbloco do disco (setor 0)
		unsigned char buffer[DISK_SECTORDATASIZE];
		if (diskReadSector(d, 0, buffer) != 0) {
			return 0;  // falha ao ler superbloco
		}

		// Deserializar campos do superbloco
		char2ul(&buffer[0], &sb.magic);
		char2ul(&buffer[4], &sb.blockSize);
		char2ul(&buffer[8], &sb.numBlocks);
		char2ul(&buffer[12], &sb.numInodes);
		char2ul(&buffer[16], &sb.inodeTableStart);
		char2ul(&buffer[20], &sb.dataBlockStart);
		char2ul(&buffer[24], &sb.freeBlockList);
		char2ul(&buffer[28], &sb.rootInode);

		// Depuração: Verificar leitura do superbloco
		printf("[DEBUG myFSxMount] Superbloco lido: magic=0x%X, blockSize=%u, numBlocks=%u\n",
			sb.magic, sb.blockSize, sb.numBlocks);

		// Validar magic number
		if (sb.magic != MYFS_MAGIC) {
			printf("[DEBUG myFSxMount] ERRO: Magic number inválido (esperado=0x%X, encontrado=0x%X)\n",
				MYFS_MAGIC, sb.magic);
			return 0;  // falha: sistema de arquivos nao reconhecido
		}

		// Validar blockSize (deve ser multiplo de 512)
		if (sb.blockSize == 0 || sb.blockSize % DISK_SECTORDATASIZE != 0) {
			printf("[DEBUG myFSxMount] ERRO: blockSize inválido (%u)\n", sb.blockSize);
			return 0;  // falha: blockSize invalido
		}

		// Validar outros campos basicos do superbloco
		if (sb.numBlocks == 0 || sb.numInodes == 0) {
			return 0;  // falha: superbloco corrompido
		}

		// Inicializar tabela de descritores de arquivo
		memset(fdTable, 0, sizeof(fdTable));
		for (int i = 0; i < MAX_FDS; i++) {
			fdTable[i].used = 0;
			fdTable[i].disk = NULL;
			fdTable[i].inodeNum = 0;
			fdTable[i].cursor = 0;
			fdTable[i].inode = NULL;
		}

		return 1;  // sucesso na montagem
	}

	// PASSO 3: DESMONTAGEM (x=0)
	if (x == 0) {
		// Verificar se o disco esta ocioso (nenhum arquivo aberto)
		if (!myFSIsIdle(d)) {
			return 0;  // falha: ainda ha arquivos abertos neste disco
		}

		// Limpar descritores de arquivo associados a este disco
		for (int i = 0; i < MAX_FDS; i++) {
			if (fdTable[i].disk == d) {
				fdTable[i].used = 0;
				fdTable[i].disk = NULL;
				fdTable[i].inodeNum = 0;
				fdTable[i].cursor = 0;
				if (fdTable[i].inode != NULL) {
					free(fdTable[i].inode);
					fdTable[i].inode = NULL;
				}
			}
		}

		// Limpar superbloco para evitar inconsistencias
		memset(&sb, 0, sizeof(sb));

		return 1;  // sucesso na desmontagem
	}

	// PASSO 4: Parametro x invalido
	return 0;  // falha: valor de x nao reconhecido (deve ser 0 ou 1)
}

// Funcao para abertura de um arquivo, a partir do caminho especificado
// em path, no disco montado especificado em d, no modo Read/Write,
// criando o arquivo se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpen(Disk *d, const char *path)
{
    // Validar parametros
    if (d == NULL || path == NULL || strlen(path) == 0) {
        printf("[DEBUG myFSOpen] ERRO: Parâmetros inválidos\n");
        return -1;
    }

    // Procurar um descritor livre na tabela
    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fdTable[i].used) {
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        printf("[DEBUG myFSOpen] ERRO: Tabela de descritores cheia\n");
        return -1; // Sem espaço na tabela de descritores
    }

    // Criar ou localizar o inode do arquivo
    printf("[DEBUG myFSOpen] Tentando carregar inode para o arquivo '%s'\n", path);
    unsigned long inodeSectorAddr = INODE_BEGINSECTOR + (1 - 1) * INODE_SIZE * sizeof(unsigned int) / DISK_SECTORDATASIZE;
    printf("[DEBUG myFSOpen] Calculado endereço do setor do inode: %lu\n", inodeSectorAddr);

    Inode *inode = inodeLoad(1, d); // Exemplo: carregar inode 1 (ajustar lógica conforme necessário)
    if (inode == NULL) {
        printf("[DEBUG myFSOpen] Inode não encontrado. Tentando criar um novo inode para '%s'\n", path);
        inode = inodeCreate(1, d); // Criar inode se não existir
        if (inode == NULL) {
            printf("[DEBUG myFSOpen] ERRO: Falha ao criar inode para '%s'\n", path);
            return -1;
        }
        printf("[DEBUG myFSOpen] Sucesso: Inode criado para '%s'\n", path);
    } else {
        printf("[DEBUG myFSOpen] Sucesso: Inode carregado para '%s'\n", path);
    }

    // Preencher o descritor de arquivo
    fdTable[fd].used = 1;
    fdTable[fd].disk = d;
    fdTable[fd].inodeNum = 1; // Exemplo: associar ao inode 1 (ajustar conforme necessário)
    fdTable[fd].cursor = 0;
    fdTable[fd].inode = inode;

    printf("[DEBUG myFSOpen] Sucesso: Arquivo aberto com descritor %d (retornando %d)\n", fd, fd + 1);
    return fd + 1; // Retornar fd+1 pois o sistema espera descritores > 0
}

// Funcao para a leitura de um arquivo, a partir de um descritor de arquivo
// existente. Os dados devem ser lidos a partir da posicao atual do cursor
// e copiados para buf. Terao tamanho maximo de nbytes. Ao fim, o cursor
// deve ter posicao atualizada para que a proxima operacao ocorra a partir
// do próximo byte apos o ultimo lido. Retorna o numero de bytes
// efetivamente lidos em caso de sucesso ou -1, caso contrario.
int myFSRead(int fd, char *buf, unsigned int nbytes)
{
	// Converter descritor de 1-based para 0-based (índice da tabela)
	int idx = fd - 1;

	printf("[DEBUG myFSRead] Iniciando leitura: fd=%d, idx=%d, nbytes=%u\n", fd, idx, nbytes);

	// Validar descritor
	if (idx < 0 || idx >= MAX_FDS || !fdTable[idx].used) {
		printf("[DEBUG myFSRead] ERRO: Descritor inválido ou não usado\n");
		return -1;
	}

	// Validar buffer
	if (buf == NULL || nbytes == 0) {
		printf("[DEBUG myFSRead] ERRO: Buffer inválido ou nbytes = 0\n");
		return -1;
	}

	// Obter informações do descritor
	Inode *inode = fdTable[idx].inode;
	Disk *disk = fdTable[idx].disk;
	if (inode == NULL || disk == NULL) {
		printf("[DEBUG myFSRead] ERRO: Inode ou disco não carregado\n");
		return -1;
	}

	// Obter tamanho do arquivo
	unsigned int fileSize = inodeGetFileSize(inode);
	unsigned int cursor = fdTable[idx].cursor;

	printf("[DEBUG myFSRead] Tamanho do arquivo: %u bytes, cursor atual: %u\n", fileSize, cursor);

	// Verificar se já chegamos ao fim do arquivo
	if (cursor >= fileSize) {
		printf("[DEBUG myFSRead] Fim do arquivo atingido\n");
		return 0; // EOF
	}

	// Calcular quantos bytes podemos ler
	unsigned int bytesToRead = nbytes;
	if (cursor + bytesToRead > fileSize) {
		bytesToRead = fileSize - cursor;
	}

	printf("[DEBUG myFSRead] Bytes a ler: %u\n", bytesToRead);

	// Ler dados bloco por bloco
	unsigned int totalRead = 0;
	unsigned int blockSize = sb.blockSize; // Tamanho do bloco do superbloco

	while (totalRead < bytesToRead) {
		// Calcular bloco e offset dentro do bloco
		unsigned int currentPos = cursor + totalRead;
		unsigned int blockNum = currentPos / blockSize;
		unsigned int offsetInBlock = currentPos % blockSize;

		// Obter endereço do bloco
		unsigned int blockAddr = inodeGetBlockAddr(inode, blockNum);
		if (blockAddr == 0) {
			printf("[DEBUG myFSRead] AVISO: Bloco %u não alocado\n", blockNum);
			break; // Bloco não alocado
		}

		// Ler bloco do disco
		unsigned char blockData[blockSize];
		unsigned int numSectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
		for (unsigned int i = 0; i < numSectorsPerBlock; i++) {
			if (diskReadSector(disk, blockAddr + i, blockData + i * DISK_SECTORDATASIZE) != 0) {
				printf("[DEBUG myFSRead] ERRO: Falha ao ler setor %u\n", blockAddr + i);
				return -1;
			}
		}

		// Copiar dados do bloco para o buffer
		unsigned int bytesFromBlock = blockSize - offsetInBlock;
		if (bytesFromBlock > bytesToRead - totalRead) {
			bytesFromBlock = bytesToRead - totalRead;
		}

		memcpy(buf + totalRead, blockData + offsetInBlock, bytesFromBlock);
		totalRead += bytesFromBlock;
	}

	// Atualizar cursor
	fdTable[idx].cursor += totalRead;

	printf("[DEBUG myFSRead] Sucesso: %u bytes lidos, novo cursor: %u\n", totalRead, fdTable[idx].cursor);
	return totalRead;
}

// Funcao para a escrita de um arquivo, a partir de um descritor de arquivo
// existente. Os dados de buf sao copiados para o disco a partir da posição
// atual do cursor e terao tamanho maximo de nbytes. Ao fim, o cursor deve
// ter posicao atualizada para que a proxima operacao ocorra a partir do
// proximo byte apos o ultimo escrito. Retorna o numero de bytes
// efetivamente escritos em caso de sucesso ou -1, caso contrario
int myFSWrite(int fd, const char *buf, unsigned int nbytes)
{
	// Converter descritor de 1-based para 0-based (índice da tabela)
	int idx = fd - 1;

	printf("[DEBUG myFSWrite] Iniciando escrita: fd=%d, idx=%d, nbytes=%u\n", fd, idx, nbytes);

	// Validar descritor
	if (idx < 0 || idx >= MAX_FDS || !fdTable[idx].used) {
		printf("[DEBUG myFSWrite] ERRO: Descritor inválido ou não usado\n");
		return -1;
	}

	// Validar buffer
	if (buf == NULL || nbytes == 0) {
		printf("[DEBUG myFSWrite] ERRO: Buffer inválido ou nbytes = 0\n");
		return -1;
	}

	// Obter informações do descritor
	Inode *inode = fdTable[idx].inode;
	Disk *disk = fdTable[idx].disk;
	if (inode == NULL || disk == NULL) {
		printf("[DEBUG myFSWrite] ERRO: Inode ou disco não carregado\n");
		return -1;
	}

	unsigned int cursor = fdTable[idx].cursor;
	printf("[DEBUG myFSWrite] Cursor atual: %u\n", cursor);

	// Escrever dados bloco por bloco
	unsigned int totalWritten = 0;
	unsigned int blockSize = sb.blockSize; // Tamanho do bloco do superbloco

	while (totalWritten < nbytes) {
		// Calcular bloco e offset dentro do bloco
		unsigned int currentPos = cursor + totalWritten;
		unsigned int blockNum = currentPos / blockSize;
		unsigned int offsetInBlock = currentPos % blockSize;

		// Obter endereço do bloco (ou alocar se necessário)
		unsigned int blockAddr = inodeGetBlockAddr(inode, blockNum);
		if (blockAddr == 0) {
			// Bloco não alocado - alocar um novo
			printf("[DEBUG myFSWrite] Bloco %u não alocado, alocando novo bloco...\n", blockNum);
			blockAddr = allocateFreeBlock(disk);
			if (blockAddr == 0) {
				printf("[DEBUG myFSWrite] ERRO: Falha ao alocar bloco livre\n");
				return -1;
			}

			// Adicionar bloco ao inode (inodeAddBlock salva automaticamente)
			if (inodeAddBlock(inode, blockAddr) != 0) {
				printf("[DEBUG myFSWrite] ERRO: Falha ao adicionar bloco ao inode\n");
				return -1;
			}

			printf("[DEBUG myFSWrite] Bloco %u alocado no setor %u\n", blockNum, blockAddr);
			
			// Verificar se foi adicionado corretamente
			unsigned int verifyAddr = inodeGetBlockAddr(inode, blockNum);
			if (verifyAddr != blockAddr) {
				printf("[DEBUG myFSWrite] AVISO: Endereço verificado (%u) diferente do alocado (%u)\n", verifyAddr, blockAddr);
			}
		}

		// Ler bloco existente (para preservar dados não sobrescritos)
		unsigned char blockData[blockSize];
		unsigned int numSectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
		for (unsigned int i = 0; i < numSectorsPerBlock; i++) {
			if (diskReadSector(disk, blockAddr + i, blockData + i * DISK_SECTORDATASIZE) != 0) {
				printf("[DEBUG myFSWrite] ERRO: Falha ao ler setor %u\n", blockAddr + i);
				return -1;
			}
		}

		// Modificar dados no bloco
		unsigned int bytesToBlock = blockSize - offsetInBlock;
		if (bytesToBlock > nbytes - totalWritten) {
			bytesToBlock = nbytes - totalWritten;
		}

		printf("[DEBUG myFSWrite] Copiando %u bytes para o bloco (offset: %u)\n", bytesToBlock, offsetInBlock);
		printf("[DEBUG myFSWrite] Primeiros bytes a escrever: '%.10s'\n", buf + totalWritten);
		memcpy(blockData + offsetInBlock, buf + totalWritten, bytesToBlock);

		// Escrever bloco de volta no disco
		printf("[DEBUG myFSWrite] Escrevendo bloco no setor %u (%u setores)\n", blockAddr, numSectorsPerBlock);
		for (unsigned int i = 0; i < numSectorsPerBlock; i++) {
			if (diskWriteSector(disk, blockAddr + i, blockData + i * DISK_SECTORDATASIZE) != 0) {
				printf("[DEBUG myFSWrite] ERRO: Falha ao escrever setor %u\n", blockAddr + i);
				return -1;
			}
		}
		printf("[DEBUG myFSWrite] Bloco escrito com sucesso\n");

		totalWritten += bytesToBlock;
	}

	// Atualizar cursor
	fdTable[idx].cursor += totalWritten;

	// Atualizar tamanho do arquivo se necessário
	unsigned int newSize = fdTable[idx].cursor;
	unsigned int oldSize = inodeGetFileSize(inode);
	if (newSize > oldSize) {
		inodeSetFileSize(inode, newSize);
		// Salvar o inode atualizado no disco
		if (inodeSave(inode) != 0) {
			printf("[DEBUG myFSWrite] AVISO: Falha ao salvar inode atualizado\n");
		}
	}

	printf("[DEBUG myFSWrite] Sucesso: %u bytes escritos, novo cursor: %u\n", totalWritten, fdTable[idx].cursor);
	return totalWritten;
}

// Funcao para fechar um arquivo, a partir de um descritor de arquivo
// existente. Retorna 0 caso bem sucedido, ou -1 caso contrario
int myFSClose(int fd)
{
    // Converter descritor de 1-based para 0-based (índice da tabela)
    int idx = fd - 1;

    // Validar descritor de arquivo
    if (idx < 0 || idx >= MAX_FDS) {
        printf("[DEBUG myFSClose] ERRO: Descritor de arquivo inválido (%d)\n", fd);
        return -1; // Descritor fora do intervalo válido
    }

    if (!fdTable[idx].used) {
        printf("[DEBUG myFSClose] ERRO: Descritor de arquivo não está em uso (%d)\n", fd);
        return -1; // Descritor não está em uso
    }

    // Liberar recursos associados ao descritor
    if (fdTable[idx].inode != NULL) {
        free(fdTable[idx].inode); // Liberar memória do inode
        fdTable[idx].inode = NULL;
    }

    // Marcar descritor como não usado
    fdTable[idx].used = 0;
    fdTable[idx].disk = NULL;
    fdTable[idx].inodeNum = 0;
    fdTable[idx].cursor = 0;

    printf("[DEBUG myFSClose] Sucesso: Descritor de arquivo fechado (%d)\n", fd);
    return 0; // Sucesso
}

// Funcao para abertura de um diretorio, a partir do caminho
// especificado em path, no disco indicado por d, no modo Read/Write,
// criando o diretorio se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpenDir(Disk *d, const char *path)
{
	return -1;
}

// Funcao para a leitura de um diretorio, identificado por um descritor
// de arquivo existente. Os dados lidos correspondem a uma entrada de
// diretorio na posicao atual do cursor no diretorio. O nome da entrada
// e' copiado para filename, como uma string terminada em \0 (max 255+1).
// O numero do inode correspondente 'a entrada e' copiado para inumber.
// Retorna 1 se uma entrada foi lida, 0 se fim de diretorio ou -1 caso
// mal sucedido
int myFSReadDir(int fd, char *filename, unsigned int *inumber)
{
	return -1;
}

// Funcao para adicionar uma entrada a um diretorio, identificado por um
// descritor de arquivo existente. A nova entrada tera' o nome indicado
// por filename e apontara' para o numero de i-node indicado por inumber.
// Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSLink(int fd, const char *filename, unsigned int inumber)
{
	return -1;
}

// Funcao para remover uma entrada existente em um diretorio,
// identificado por um descritor de arquivo existente. A entrada e'
// identificada pelo nome indicado em filename. Retorna 0 caso bem
// sucedido, ou -1 caso contrario.
int myFSUnlink(int fd, const char *filename)
{
	return -1;
}

// Funcao para fechar um diretorio, identificado por um descritor de
// arquivo existente. Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSCloseDir(int fd)
{
	return -1;
}

// Variavel estatica para persistir apos installMyFS retornar
static FSInfo myFSInfo;

// Funcao para instalar seu sistema de arquivos no S.O., registrando-o junto
// ao virtual FS (vfs). Retorna um identificador unico (slot), caso
// o sistema de arquivos tenha sido registrado com sucesso.
// Caso contrario, retorna -1
// envio o end. de todas as funcoes implementadas acima pro vfs ter onde buscar
int installMyFS(void)
{
	// printf("[DEBUG installMyFS] Iniciando instalacao...\n");

	// Preencher struct estatica (persiste apos funcao retornar)
	myFSInfo.fsid = 0;  // ID do filesystem (importante!)
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

	// Verificar se o fsid é único
	if (myFSInfo.fsid == 0) {
		printf("[DEBUG installMyFS] Aviso: fsid está definido como 0. Certifique-se de que não há conflito.\n");
	}

	// Registrar no VFS
	int slot = vfsRegisterFS(&myFSInfo);
	if (slot < 0) {
		printf("[DEBUG installMyFS] ERRO: vfsRegisterFS retornou %d. Registro falhou.\n", slot);
		return -1;
	}

	printf("[DEBUG installMyFS] Sucesso! Slot = %d\n", slot);
	return slot;
}
