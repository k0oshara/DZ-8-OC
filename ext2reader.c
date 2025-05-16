#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <ext2fs/ext2_fs.h>

#define ERROR(msg) do { fprintf(stderr, "ERROR: %s\n", msg); } while(0)
#define CHECK_NULL(ptr, msg) if (!(ptr)) { ERROR(msg); goto cleanup; }

static inline uint16_t le16(uint16_t v) { return le16toh(v); }
static inline uint32_t le32(uint32_t v) { return le32toh(v); }

typedef enum {
    RET_OK,
    RET_IO_ERROR,
    RET_INVALID_FS,
    RET_MEM_ERROR,
    RET_INVALID_INODE
} RetCode;

static RetCode rd_indir(FILE *f, uint32_t indir, uint32_t idx, uint32_t bsz, uint32_t *result) {
    if (!indir) {
        *result = 0;
        return RET_OK;
    }
    
    uint32_t *buf = malloc(bsz);
    if (!buf) {
        ERROR("Memory allocation failed");
        return RET_MEM_ERROR;
    }
    
    if (fseek(f, indir * bsz, SEEK_SET)) {
        ERROR("Seek error in indirect block");
        free(buf);
        return RET_IO_ERROR;
    }
    
    if (fread(buf, bsz, 1, f) != 1) {
        ERROR("Failed to read indirect block");
        free(buf);
        return RET_IO_ERROR;
    }
    
    *result = le32(buf[idx]);
    free(buf);
    return RET_OK;
}

static RetCode get_blk(FILE *f, uint32_t log_blk, struct ext2_inode *ino, uint32_t bsz, uint32_t *phys_blk) {
    uint32_t ents = bsz / 4;
    RetCode rc = RET_OK;
    uint32_t tmp;

    if (log_blk < EXT2_NDIR_BLOCKS) {
        *phys_blk = le32(ino->i_block[log_blk]);
        return RET_OK;
    }

    if (log_blk < (EXT2_NDIR_BLOCKS + ents)) {
        uint32_t sgl = le32(ino->i_block[EXT2_IND_BLOCK]);
        if ((rc = rd_indir(f, sgl, log_blk - EXT2_NDIR_BLOCKS, bsz, &tmp)) != RET_OK)
            return rc;
        *phys_blk = tmp;
        return RET_OK;
    }

    if (log_blk < (EXT2_NDIR_BLOCKS + ents + ents*ents)) {
        uint32_t block = log_blk - (EXT2_NDIR_BLOCKS + ents);
        uint32_t dbl = le32(ino->i_block[EXT2_DIND_BLOCK]);
        uint32_t idx1 = block / ents;
        uint32_t idx2 = block % ents;
        
        if ((rc = rd_indir(f, dbl, idx1, bsz, &tmp)) != RET_OK) return rc;
        if ((rc = rd_indir(f, tmp, idx2, bsz, &tmp)) != RET_OK) return rc;
        
        *phys_blk = tmp;
        return RET_OK;
    }

    uint32_t block = log_blk - (EXT2_NDIR_BLOCKS + ents + ents*ents);
    uint32_t tri = le32(ino->i_block[EXT2_TIND_BLOCK]);
    uint32_t idx1 = block / (ents*ents);
    uint32_t idx2 = (block / ents) % ents;
    uint32_t idx3 = block % ents;

    if ((rc = rd_indir(f, tri, idx1, bsz, &tmp)) != RET_OK) return rc;
    if ((rc = rd_indir(f, tmp, idx2, bsz, &tmp)) != RET_OK) return rc;
    if ((rc = rd_indir(f, tmp, idx3, bsz, &tmp)) != RET_OK) return rc;
    
    *phys_blk = tmp;
    return RET_OK;
}

int main(int argc, char **argv) {
    RetCode ret = RET_OK;
    FILE *f = NULL;
    void *ino_buf = NULL;
    char *dat = NULL;
    uint32_t max_groups = 0;

    if (argc != 3) {
        ERROR("Usage: ext2reader <image> <inode>");
        return RET_INVALID_INODE;
    }

    if (!(f = fopen(argv[1], "rb"))) {
        perror("Failed to open image");
        return RET_IO_ERROR;
    }

    struct ext2_super_block sb;
    if (fseek(f, 1024, SEEK_SET) || 
        fread(&sb, sizeof(sb), 1, f) != 1) {
        perror("Superblock read failed");
        ret = RET_IO_ERROR;
        goto cleanup;
    }

    if (le16(sb.s_magic) != EXT2_SUPER_MAGIC) {
        ERROR("Not an ext2 filesystem");
        ret = RET_INVALID_FS;
        goto cleanup;
    }

    uint32_t bsz = 1024 << le32(sb.s_log_block_size);
    if (bsz < 1024 || bsz > 4096) {
        ERROR("Invalid block size");
        ret = RET_INVALID_FS;
        goto cleanup;
    }

    uint32_t ino_num = atoi(argv[2]);
    max_groups = le32(sb.s_blocks_count) / le32(sb.s_blocks_per_group);
    if (ino_num == 0 || ino_num > le32(sb.s_inodes_count)) {
        ERROR("Invalid inode number");
        ret = RET_INVALID_INODE;
        goto cleanup;
    }

    uint32_t ino_per_grp = le32(sb.s_inodes_per_group);
    uint32_t grp = (ino_num - 1) / ino_per_grp;
    if (grp >= max_groups) {
        ERROR("Inode group out of range");
        ret = RET_INVALID_INODE;
        goto cleanup;
    }

    struct ext2_group_desc gd;
    uint32_t gdt_blk = (bsz == 1024) ? 2 : 1;
    if (fseek(f, gdt_blk * bsz + grp * sizeof(gd), SEEK_SET) ||
        fread(&gd, sizeof(gd), 1, f) != 1) {
        perror("Group descriptor read failed");
        ret = RET_IO_ERROR;
        goto cleanup;
    }

    uint32_t ino_sz = le32(sb.s_inode_size);
    ino_buf = malloc(ino_sz);
    CHECK_NULL(ino_buf, "Inode buffer allocation failed");
    
    uint32_t ino_tbl = le32(gd.bg_inode_table);
    uint32_t ino_idx = (ino_num - 1) % ino_per_grp;
    
    if (fseek(f, ino_tbl * bsz + ino_idx * ino_sz, SEEK_SET) ||
        fread(ino_buf, ino_sz, 1, f) != 1) {
        perror("Inode read failed");
        ret = RET_IO_ERROR;
        goto cleanup;
    }

    struct ext2_inode *ino = (struct ext2_inode *)ino_buf;
    uint64_t f_sz = ((uint64_t)le32(ino->i_size_high) << 32) | le32(ino->i_size);
    uint64_t max_blk = (f_sz + bsz - 1) / bsz;

    for (uint64_t b = 0; b < max_blk; b++) {
        uint32_t pb;
        size_t len = (b == max_blk - 1) ? (f_sz % bsz) : bsz;
        if (len == 0) len = bsz;

        if ((ret = get_blk(f, b, ino, bsz, &pb)) != RET_OK) {
            ERROR("Block lookup failed");
            goto cleanup;
        }

        if (!pb) {
            char zero = 0;
            for (size_t i = 0; i < len; i++) {
                if (fwrite(&zero, 1, 1, stdout) != 1) {
                    ERROR("Write failed");
                    ret = RET_IO_ERROR;
                    goto cleanup;
                }
            }
        } else {
            dat = malloc(bsz);
            CHECK_NULL(dat, "Block buffer allocation failed");
            
            if (fseek(f, pb * bsz, SEEK_SET) ||
                fread(dat, bsz, 1, f) != 1) {
                perror("Block read failed");
                free(dat);
                ret = RET_IO_ERROR;
                goto cleanup;
            }
            
            if (fwrite(dat, 1, len, stdout) != len) {
                ERROR("Data write failed");
                free(dat);
                ret = RET_IO_ERROR;
                goto cleanup;
            }
            free(dat);
            dat = NULL;
        }
    }

cleanup:
    if (dat) free(dat);
    if (ino_buf) free(ino_buf);
    if (f) fclose(f);
    return ret;
}
