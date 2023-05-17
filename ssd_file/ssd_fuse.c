/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};


static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule{
    unsigned int pca;
    struct{
        unsigned int page : 16; //total 20 in each block
        unsigned int block: 16; //total 8 blocks

    } fields;
};
typedef union block_rule BLOCK_RULE;
union block_rule{
    //unsigned int block;
    struct{
        unsigned int block_number : 8;
        char state : 8; // O : open, S : static
        unsigned int INV_num : 8; //total 8 blocks
        unsigned int start_from : 8;
    } content;
};


PCA_RULE curr_pca, emp_bolck_pca;
BLOCK_RULE *curr_nand_block;

unsigned int* L2P;
BLOCK_RULE *AV_Q, *OP_Q;


static int ssd_resize(size_t new_size){
    //set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024  ){
        return -ENOMEM;
    }
    else{
        logic_size = new_size;
        return 0;
    }

}

static int ssd_expand(size_t new_size){
    //logic must less logic limit

    if (new_size > logic_size){
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca){
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);
    //read
    if ( (fptr = fopen(nand_name, "r") )){
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else{
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char* buf, int pca){
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //write
    // printf("buff:%c\n", buf[0]);
    if ( (fptr = fopen(nand_name, "r+"))){
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size ++;
    }
    else{
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block){
    char nand_name[100];
	int found = 0;
    FILE* fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    //erase
    if ( (fptr = fopen(nand_name, "w"))){
        fclose(fptr);
        found=1;
    }
    else{
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }


	if (found == 0){
		printf("nand erase not found\n");
		return -EINVAL;
	}

    printf("nand erase %d pass\n", block);
    return 1;
}

static void gc_only_1(int block_num){
    PCA_RULE pca, emp_b_pca;
    emp_b_pca.pca = 0;
    emp_b_pca.fields.block = OP_Q[0].content.block_number;
    emp_b_pca.fields.page = 0;
    // printf("i am here!!!!!!!!!!!!!!!!\n");
    char* tmp_buf;
    tmp_buf = malloc(512 * sizeof(char));
    memset(tmp_buf, 0, 512 * sizeof(char));

    AV_Q[0].content.state = 'O';
    AV_Q[0].content.INV_num = 0;
    AV_Q[0].content.start_from = 0;
    
    for(int k = 0; k<LOGICAL_NAND_NUM * PHYSICAL_PAGE_NUM; ++k){
        pca.pca = L2P[k];
        if(pca.fields.block == block_num){
            nand_read(tmp_buf, pca.pca);
            nand_write(tmp_buf, emp_b_pca.pca);
            pca.fields.block = OP_Q[0].content.block_number;
            pca.fields.page = OP_Q[0].content.start_from;
            emp_b_pca.fields.page = OP_Q[0].content.start_from;
        }
    }
    nand_erase(block_num);
    typeof(AV_Q[0]) temp_b = OP_Q[0];
    for(int i = 0; i<PHYSICAL_NAND_NUM-LOGICAL_NAND_NUM-1; i++){
        OP_Q[i] = OP_Q[i+1];
    }
    OP_Q[PHYSICAL_NAND_NUM-LOGICAL_NAND_NUM-1] = AV_Q[0];
    AV_Q[0] = temp_b;
}

static void gc(){
    int block_1, block_2, j, found=0;
    
    PCA_RULE pca, emp_b_pca;
    emp_b_pca.pca = 0;
    emp_b_pca.fields.block = OP_Q[0].content.block_number;
    emp_b_pca.fields.page = 0;
    
    char* tmp_buf;
    tmp_buf = malloc(512 * sizeof(char));
    memset(tmp_buf, 0, 512 * sizeof(char));
    
    for(int i = 0;i<LOGICAL_NAND_NUM-1;i++){
        // printf("block_state of %d :%c\n", i, AV_Q[i].content.state);
        if (AV_Q[i].content.state == 'O' || AV_Q[i].content.INV_num == 0){
            // printf("check i_origin0:%d\n",i);
            if (i == LOGICAL_NAND_NUM - 2) return;
            continue;
        }
        int valid_num_1 = PHYSICAL_PAGE_NUM - AV_Q[i].content.INV_num;
        for (j = i+1; j<LOGICAL_NAND_NUM;j++){
            if (AV_Q[j].content.state == 'O' || AV_Q[j].content.INV_num == 0){
                if (j == LOGICAL_NAND_NUM -1) return;
                // printf("check j_origin1:%d\n",j);
                // printf("check i_origin1:%d\n",i);
                continue;
            }
            int valid_num_2 = PHYSICAL_PAGE_NUM - AV_Q[j].content.INV_num;
            if(valid_num_1+valid_num_2<=PHYSICAL_PAGE_NUM){
                block_1 = AV_Q[i].content.block_number;
                block_2 = AV_Q[j].content.block_number;
                AV_Q[i].content.state = 'O';
                AV_Q[j].content.state = 'O';
                AV_Q[i].content.INV_num = 0;
                AV_Q[j].content.INV_num = 0;
                AV_Q[i].content.start_from = 0;
                AV_Q[j].content.start_from = 0;
                found = 1;
                // printf("check j_origin2:%d\n",j);
                // printf("check i_origin2:%d\n",i);
                break;
            }
        }
        if(found) break;
    }
    for(int k = 0;k<LOGICAL_NAND_NUM * PHYSICAL_PAGE_NUM;k++){
        pca.pca = L2P[k];
        if(pca.fields.block == block_1 || pca.fields.block == block_2){
            // printf("============ vaild ============\n");
            // printf("valid block: %u | vaild page: %u\n", pca.fields.block, pca.fields.page);
            nand_read(tmp_buf, pca.pca);
            nand_write(tmp_buf, emp_b_pca.pca);
            pca.fields.block = OP_Q[0].content.block_number;
            pca.fields.page = OP_Q[0].content.start_from;
            if(++OP_Q[0].content.start_from == PHYSICAL_PAGE_NUM){
                OP_Q[0].content.state = 'S';
            }
            emp_b_pca.fields.page = OP_Q[0].content.start_from;
        }
    }
    nand_erase(block_1);
    nand_erase(block_2);
    typeof(AV_Q[0]) temp_b = OP_Q[0];
    for(int i = 0;i<PHYSICAL_NAND_NUM-LOGICAL_NAND_NUM-1;i++){
        OP_Q[i] = OP_Q[i+1];
    }
    // printf("check j: %d, check op_idx: %d\n", j, PHYSICAL_NAND_NUM-LOGICAL_NAND_NUM-1);
    OP_Q[PHYSICAL_NAND_NUM-LOGICAL_NAND_NUM-1] = AV_Q[j]; 
    AV_Q[j] = temp_b;
    //---------------------------------------//
    // printf("=================queue info=================\n");
    // for (int idx = 0; idx < PHYSICAL_NAND_NUM; idx++){
    //     if (idx<LOGICAL_NAND_NUM){
    //         printf("b_num: %d | inv_num: %d | start: %d | state: %c\n", 
    //                 AV_Q[idx].content.block_number,
    //                 AV_Q[idx].content.INV_num,
    //                 AV_Q[idx].content.start_from,
    //                 AV_Q[idx].content.state);
    //     }
    //     else{
    //         printf("b_num: %d | inv_num: %d | start: %d | state: %c\n", 
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.block_number,
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.INV_num,
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.start_from,
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.state);
    //     }
    // }
    // printf("=================queue info=================\n");
    //---------------------------------------//
    return;
}

static unsigned int get_next_nand_block(){
    curr_nand_block->content.state = 'S';
    // typeof(AV_Q[0]) = 1;
    
    typeof(AV_Q[0]) temp_b = AV_Q[0];
    for (int i = 0 ;i<LOGICAL_NAND_NUM-1;i++){
        AV_Q[i] = AV_Q[i+1];
    }
    AV_Q[LOGICAL_NAND_NUM - 1] = temp_b;
    for (int j = 0; j < LOGICAL_NAND_NUM - 2; ++j){
        if (AV_Q[j].content.state == 'S' && AV_Q[j].content.state == 'O'){
            temp_b = AV_Q[j];
            AV_Q[j] = AV_Q[j + 1];
            AV_Q[j + 1] = temp_b;
        }else if(AV_Q[j].content.state == 'S' && AV_Q[j].content.state == 'S'){
            if (AV_Q[j].content.INV_num < AV_Q[j + 1].content.INV_num){
                temp_b = AV_Q[j];
                AV_Q[j] = AV_Q[j + 1];
                AV_Q[j + 1] = temp_b;
            }
        } 
    }
    curr_nand_block = &AV_Q[0];
    if (curr_nand_block->content.state =='S'){
        if(curr_nand_block->content.start_from == 5){
            return FULL_PCA;
        }
        gc_only_1(curr_nand_block->content.block_number);
    }
    // printf("============block_sorted============\n");
    // for(int i = 0; i<LOGICAL_NAND_NUM; ++i){
    //     printf("blocknumber:%d | invalid num:%d\n", AV_Q[i].content.block_number, AV_Q[i].content.INV_num);
    // }
    // printf("============block_sorted============\n");
    return curr_nand_block->content.block_number;
}
static unsigned int get_next_pca(){
    if (curr_pca.pca == INVALID_PCA){
        //init
        curr_pca.pca = 0;
        curr_nand_block = &AV_Q[0];
        curr_nand_block->content.start_from = 1;
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA){
        //full ssd, no pca can allocate
        printf("\n\n\nNo new PCA\n\n\n");
        return FULL_PCA;
    }

    if ( curr_pca.fields.page == PHYSICAL_PAGE_NUM - 1){
        unsigned int block_num = get_next_nand_block();
        if (block_num == FULL_PCA){
            printf("\n\n\nNo new empty BLOCK!!\n\n\n");
            return FULL_PCA;
        }
        curr_pca.fields.block = block_num;
    }

    curr_pca.fields.page = curr_nand_block->content.start_from; //(curr_pca.fields.page + 1 ) % PHYSICAL_PAGE_NUM;
    curr_nand_block->content.start_from += 1;
    // printf("========================================\n");
    // printf("pca_page: %u | start_from:%u\n", curr_pca.fields.page, curr_nand_block.content.start_from);
    // printf("-------------------------------\n\n\n");

    if (curr_pca.fields.page >= PHYSICAL_PAGE_NUM){
        printf("No new PCA\n");
        curr_pca.pca = FULL_PCA;
        return FULL_PCA;
    }
    else{
        printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
        return curr_pca.pca;
    }

}


static int ftl_read( char* buf, size_t lba){
    PCA_RULE pca;

    for(int i = 0 ; i<5 ; i++){
	    pca.pca = L2P[i];
        printf("block: %u | page: %u\n", pca.fields.block, pca.fields.page);
    }
    pca.pca = L2P[lba];
	if (pca.pca == INVALID_PCA) {
	    //non write data, return 0
	    return 0;
	}
	else {
	    return nand_read(buf, pca.pca);
	}
}

static int ftl_write(const char* buf, size_t lba_rnage, size_t lba){
    /*  only simple write, need to consider other cases  */
    PCA_RULE pca, tmp_pca;
    BLOCK_RULE *over_write_nand;
    pca.pca = get_next_pca();
    if (nand_write( buf, pca.pca) > 0){
        if(L2P[lba] != INVALID_PCA){
            tmp_pca.pca = L2P[lba];
            for(int i=0; i<LOGICAL_NAND_NUM; ++i){
                if (AV_Q[i].content.block_number == tmp_pca.fields.block){
                    over_write_nand = &AV_Q[i];
                }
            }
            ++ over_write_nand->content.INV_num;
            // printf("============block_over============\n");
            // for(int i = 0; i<LOGICAL_NAND_NUM; ++i){
            //     printf("blocknumber:%d | invalid num:%d\n", 
            //             AV_Q[i].content.block_number, 
            //             AV_Q[i].content.INV_num);
            // }
            // printf("============block_over============\n");
        }
        L2P[lba] = pca.pca;

        return 512 ;
    }
    else{
        printf(" --> Write fail !!!\n");
        return -EINVAL;
    }
}

static int ssd_file_type(const char* path){
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

static int ssd_getattr(const char* path, struct stat* stbuf,struct fuse_file_info* fi){
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path)){
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char* path, struct fuse_file_info* fi){
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE){
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char* buf, size_t size, off_t offset){
    int tmp_lba, tmp_lba_range, rst ;
    char* tmp_buf;

    // off limit
    if ((offset ) >= logic_size){
        return 0;
    }
    if ( size > logic_size - offset){
        //is valid data section
        //printf("@ssd_do_read |size:%ld\n",size);
        size = logic_size - offset;
        //printf("@ssd_do_read |logic_size%ld, size:%ld\n",logic_size, size);
    }

    tmp_lba = offset / 512;
	tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));
    // printf("\n\noffset:%ld, size:%ld, lba:%d, tmp_lba_range:%d\n",offset, size, tmp_lba, tmp_lba_range);
    for (int i = 0; i < tmp_lba_range; i++) {
        rst = ftl_read(tmp_buf + i * 512, tmp_lba++);
        if ( rst == 0){
            //is zero, read unwrite data
            memset(tmp_buf + i * 512, 0, 512);
        }
        else if (rst < 0 ){
            free(tmp_buf);
            return rst;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}

static int ssd_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi){
    (void) fi;
    //printf("@@@@@@@@@@@@@@@@@@ssd_read offset: %ld\n", offset);
    if (ssd_file_type(path) != SSD_FILE){
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static int ssd_do_write(const char* buf, size_t size, off_t offset){
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;

    host_write_size += size; // raise after ssd_write
    if (ssd_expand(offset + size) != 0){
        return -ENOMEM;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    //printf("\n\n\n\n!!write!! offset:%ld, lba:%d, tmp_lba_range:%d\n",offset, tmp_lba, tmp_lba_range);
    for (idx = 0; idx < tmp_lba_range; idx++){
        //  example only align 512, need to implement other cases  
        //offset % 512 == 0 && size % 512 == 0
    
        rst = ftl_write(buf + process_size, 1, tmp_lba + idx);
        if ( rst == 0 ){
            //write full return -enomem;
            return -ENOMEM;
        }
        else if (rst < 0){
            //error
            return rst;
        }
        curr_size += 512;
        remain_size -= 512;
        process_size += 512;
        offset += 512;
    
    }
    gc();
    // printf("=================queue info=================\n");
    // for (int idx = 0; idx < PHYSICAL_NAND_NUM; idx++){
    //     if (idx<LOGICAL_NAND_NUM){
    //         printf("b_num: %d | inv_num: %d | start: %d | state: %c\n", 
    //                 AV_Q[idx].content.block_number,
    //                 AV_Q[idx].content.INV_num,
    //                 AV_Q[idx].content.start_from,
    //                 AV_Q[idx].content.state);
    //     }
    //     else{
    //         printf("b_num: %d | inv_num: %d | start: %d | state: %c\n", 
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.block_number,
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.INV_num,
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.start_from,
    //                 OP_Q[idx - LOGICAL_NAND_NUM].content.state);
    //     }
    // }
    // printf("=================queue info=================\n");
    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi){
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE){
        return -EINVAL;
    }
    size_t clean_size = ((size+offset)/512+1)*512;
    char* clean_buf = malloc( clean_size );
    memset(clean_buf, 0, clean_size);
    memcpy(clean_buf+offset, buf, size);
    
    //printf("buf: %s \nclean_buf: %s\n", buf, clean_buf);
    
    
    // arg from ssd_fuse_dut 'w' command size offset
    // printf("@ssd_write size:%ld, offset:%ld\n", size, offset);
    return ssd_do_write(clean_buf, size, offset);
}

static int ssd_truncate(const char* path, off_t size, struct fuse_file_info* fi){
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE){
        return -EINVAL;
    }

    return ssd_resize(size);
}

static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags){
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT){
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data){

    if (ssd_file_type(path) != SSD_FILE){
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT){
        return -ENOSYS;
    }
    switch (cmd){
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}
static const struct fuse_operations ssd_oper ={
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};
int main(int argc, char* argv[]){
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * (1024 / 512) * sizeof(int)); //5*20*4 byte = 16384 *4 byte
    AV_Q = malloc(sizeof(BLOCK_RULE) * LOGICAL_NAND_NUM); // block_rule data size
    OP_Q = malloc(sizeof(BLOCK_RULE) * (PHYSICAL_NAND_NUM - LOGICAL_NAND_NUM));
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512);
    memset(AV_Q, 0, sizeof(BLOCK_RULE)*LOGICAL_NAND_NUM);
    memset(OP_Q, 0, sizeof(BLOCK_RULE)*(PHYSICAL_NAND_NUM - LOGICAL_NAND_NUM));

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++){
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL){
            printf("open fail");
        }
        fclose(fptr);
        
        if (idx<LOGICAL_NAND_NUM){
            AV_Q[idx].content.block_number = idx;
            AV_Q[idx].content.INV_num = 0;
            AV_Q[idx].content.start_from = 0;
            AV_Q[idx].content.state = 'O';
        }else{
            OP_Q[idx - LOGICAL_NAND_NUM].content.block_number = idx;
            OP_Q[idx - LOGICAL_NAND_NUM].content.INV_num = 0;
            OP_Q[idx - LOGICAL_NAND_NUM].content.start_from = 0;
            OP_Q[idx - LOGICAL_NAND_NUM].content.state = 'O';
        }
    }
    printf("logiv_n_num:%d, nand_size: %d, total: %ld\n", LOGICAL_NAND_NUM,NAND_SIZE_KB,LOGICAL_NAND_NUM * NAND_SIZE_KB * (1024 / 512) * sizeof(int));
    return fuse_main(argc, argv, &ssd_oper, NULL);
}